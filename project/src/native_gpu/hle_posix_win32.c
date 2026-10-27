/* Linux/macOS implementations for hle_posix_win32.h (renderer files only). */
#ifndef _WIN32
#define _GNU_SOURCE
#include "hle_posix_win32.h"
#include <errno.h>
#include <limits.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

enum { H_THREAD = 1, H_EVENT = 2 };

typedef struct PosixHandle {
    int kind;
    pthread_t thread;
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
    pthread_mutex_t m;
    pthread_cond_t c;
    int manual, signaled;
} PosixHandle;

static void *thread_main(void *p)
{
    PosixHandle *h = (PosixHandle *)p;
    h->fn(h->arg);
    return NULL;
}

HANDLE CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid)
{
    PosixHandle *h = (PosixHandle *)calloc(1, sizeof *h);
    pthread_attr_t a;
    (void)sa; (void)flags;
    if (!h)
        return NULL;
    h->kind = H_THREAD;
    h->fn = fn;
    h->arg = arg;
    pthread_attr_init(&a);
    if (stack)
        pthread_attr_setstacksize(&a, stack < PTHREAD_STACK_MIN ? PTHREAD_STACK_MIN : stack);
    if (pthread_create(&h->thread, &a, thread_main, h) != 0) {
        pthread_attr_destroy(&a);
        free(h);
        return NULL;
    }
    pthread_attr_destroy(&a);
    pthread_detach(h->thread);
    if (tid)
        *tid = 0;
    return (HANDLE)h;
}

HANDLE CreateEventW(void *sa, BOOL manual_reset, BOOL initial, const wchar_t *name)
{
    PosixHandle *h = (PosixHandle *)calloc(1, sizeof *h);
    (void)sa; (void)name;
    if (!h)
        return NULL;
    h->kind = H_EVENT;
    h->manual = manual_reset != 0;
    h->signaled = initial != 0;
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    return (HANDLE)h;
}

BOOL SetEvent(HANDLE hh)
{
    PosixHandle *h = (PosixHandle *)hh;
    if (!h || h->kind != H_EVENT)
        return FALSE;
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    if (h->manual)
        pthread_cond_broadcast(&h->c);
    else
        pthread_cond_signal(&h->c);
    pthread_mutex_unlock(&h->m);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE hh, DWORD ms)
{
    PosixHandle *h = (PosixHandle *)hh;
    struct timespec dl;
    int r = 0;
    if (!h || h->kind != H_EVENT)
        return WAIT_FAILED;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += ms / 1000u;
        dl.tv_nsec += (long)(ms % 1000u) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&h->m);
    while (!h->signaled && r != ETIMEDOUT)
        r = ms == INFINITE ? pthread_cond_wait(&h->c, &h->m) : pthread_cond_timedwait(&h->c, &h->m, &dl);
    if (h->signaled) {
        if (!h->manual)
            h->signaled = 0;
        pthread_mutex_unlock(&h->m);
        return WAIT_OBJECT_0;
    }
    pthread_mutex_unlock(&h->m);
    return WAIT_TIMEOUT;
}

BOOL CloseHandle(HANDLE hh)
{
    PosixHandle *h = (PosixHandle *)hh;
    /* Threads are detached and own their block; events live as long as the
     * process here (the renderer never closes one it still waits on). */
    (void)h;
    return TRUE;
}

DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n)
{
    char p[PATH_MAX];
    ssize_t len = -1;
    size_t i;
    (void)m;
#if defined(__APPLE__)
    uint32_t sz = sizeof p;
    if (_NSGetExecutablePath(p, &sz) == 0)
        len = (ssize_t)strlen(p);
#else
    len = readlink("/proc/self/exe", p, sizeof p - 1);
#endif
    if (len <= 0 || !n)
        return 0;
    p[len] = 0;
    i = mbstowcs(out, p, n - 1);
    if (i == (size_t)-1)
        i = 0;
    out[i] = 0;
    for (size_t k = 0; k < i; k++)
        if (out[k] == L'/')
            out[k] = L'\\';
    return (DWORD)i;
}

void hle_posix_path(const wchar_t *wpath, char *out, size_t n)
{
    size_t i = wcstombs(out, wpath, n - 1);
    if (i == (size_t)-1)
        i = 0;
    out[i] = 0;
    for (size_t k = 0; k < i; k++)
        if (out[k] == '\\')
            out[k] = '/';
}

FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[PATH_MAX], m[16];
    hle_posix_path(path, p, sizeof p);
    hle_posix_path(mode, m, sizeof m);
    return fopen(p, m);
}

/* ---------------------------------------------------------------- ini */

static int line_key(const wchar_t *line, const wchar_t *key, const wchar_t **value)
{
    size_t k = wcslen(key);
    while (*line == L' ' || *line == L'\t')
        line++;
    if (wcsncasecmp(line, key, k))
        return 0;
    line += k;
    while (*line == L' ' || *line == L'\t')
        line++;
    if (*line != L'=')
        return 0;
    *value = line + 1;
    return 1;
}

static int line_section(const wchar_t *line, const wchar_t *section)
{
    size_t k = wcslen(section);
    while (*line == L' ' || *line == L'\t')
        line++;
    return *line == L'[' && !wcsncasecmp(line + 1, section, k) && line[1 + k] == L']';
}

DWORD GetPrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *def,
                               wchar_t *out, DWORD n, const wchar_t *file)
{
    FILE *f = _wfopen(file, L"r");
    wchar_t line[512];
    int in = 0;
    wcsncpy(out, def ? def : L"", n - 1);
    out[n - 1] = 0;
    if (!f)
        return (DWORD)wcslen(out);
    while (fgetws(line, 512, f)) {
        const wchar_t *v;
        size_t len;
        if (line[0] == L'[') {
            in = line_section(line, section);
            continue;
        }
        if (in && line_key(line, key, &v)) {
            wcsncpy(out, v, n - 1);
            out[n - 1] = 0;
            len = wcslen(out);
            while (len && (out[len - 1] == L'\n' || out[len - 1] == L'\r' || out[len - 1] == L' '))
                out[--len] = 0;
            break;
        }
    }
    fclose(f);
    return (DWORD)wcslen(out);
}

BOOL WritePrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *value,
                                const wchar_t *file)
{
    wchar_t (*lines)[512] = NULL, line[512];
    int n = 0, cap = 0, i, sec = -1, at = -1, in = 0;
    FILE *f = _wfopen(file, L"r");
    if (f) {
        while (fgetws(line, 512, f)) {
            const wchar_t *v;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                lines = realloc(lines, sizeof *lines * (size_t)cap);
                if (!lines) { fclose(f); return FALSE; }
            }
            wcscpy(lines[n], line);
            if (line[0] == L'[') {
                in = line_section(line, section);
                if (in)
                    sec = n;
            } else if (in && at < 0 && line_key(line, key, &v)) {
                at = n;
            }
            n++;
        }
        fclose(f);
    }
    f = _wfopen(file, L"w");
    if (!f) {
        free(lines);
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        if (i == at) {
            fwprintf(f, L"%ls=%ls\n", key, value);
            continue;
        }
        fputws(lines[i], f);
        if (i == sec && at < 0) {
            /* Key not there yet: right after the section header. */
            fwprintf(f, L"%ls=%ls\n", key, value);
        }
    }
    if (sec < 0)
        fwprintf(f, L"[%ls]\n%ls=%ls\n", section, key, value);
    fclose(f);
    free(lines);
    return TRUE;
}

#endif /* !_WIN32 */
