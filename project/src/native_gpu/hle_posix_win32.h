/*
 * Linux/macOS: the Win32 the renderer files use (hle_d3d11.c, hle_draw.c,
 * hle_options.c, ...), on top of DXVK native's <windows.h> -- the D3D11/DXGI
 * headers come from DXVK there, with its own Win32 base types, so these files
 * cannot share the runtime's win32_compat layer (different WCHAR, LONG...).
 * Only what those files call; nothing here is Xbox semantics.
 *
 * Paths: GetModuleFileNameW answers with '\\' separators, as on Windows, so
 * the callers' "replace what follows the last backslash" code is unchanged;
 * every file function here (_wfopen, the ini calls) turns them back into '/'.
 */
#ifndef HLE_POSIX_WIN32_H
#define HLE_POSIX_WIN32_H

#ifndef _WIN32

#include <windows.h>          /* DXVK native: base types, GUIDs, HRESULTs */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <unistd.h>

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

/* ---- locks ---- */
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_lock(&c->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_unlock(&c->m); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_destroy(&c->m); }

typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }

/* ---- atomics ---- */
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c)
{
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
static inline void YieldProcessor(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/* ---- time ---- */
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c->QuadPart = (LONGLONG)t.tv_sec * 1000000000LL + t.tv_nsec;
    return TRUE;
}
static inline DWORD GetTickCount(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (DWORD)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
static inline void Sleep(DWORD ms)
{
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&t, NULL);
}

/* ---- threads and events (handles are hle_posix_win32.c objects) ---- */
/* Own names: the runtime's win32_compat (linked into the same program)
 * defines these too, with handles of its own. */
#define CreateThread hle_CreateThread
#define CreateEventW hle_CreateEventW
#define SetEvent hle_SetEvent
#define WaitForSingleObject hle_WaitForSingleObject
#define CloseHandle hle_CloseHandle
#define GetModuleFileNameW hle_GetModuleFileNameW
#define _wfopen hle_wfopen
#define GetPrivateProfileStringW hle_GetPrivateProfileStringW
#define WritePrivateProfileStringW hle_WritePrivateProfileStringW
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
HANDLE CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid);
HANDLE CreateEventW(void *sa, BOOL manual_reset, BOOL initial, const wchar_t *name);
BOOL SetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);

/* ---- process, environment, strings ---- */
DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n);
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
void hle_posix_path(const wchar_t *wpath, char *out, size_t n);
static inline void ExitProcess(UINT code) { fflush(stdout); fflush(stderr); _exit((int)code); }
static inline int _putenv_s(const char *name, const char *value)
{
    return value && *value ? setenv(name, value, 1) : unsetenv(name);
}
static inline int _wtoi(const wchar_t *s) { return (int)wcstol(s, NULL, 10); }
#define _wcsicmp wcscasecmp
#define _snwprintf swprintf

/* fifastreet.ini: [section] key=value, as GetPrivateProfileString keeps it. */
DWORD GetPrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *def,
                               wchar_t *out, DWORD n, const wchar_t *file);
BOOL WritePrivateProfileStringW(const wchar_t *section, const wchar_t *key, const wchar_t *value,
                                const wchar_t *file);

/* Rectangle helpers the options menu's mouse code uses. */
static inline BOOL PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
static inline BOOL SetRectEmpty(RECT *r) { r->left = r->top = r->right = r->bottom = 0; return TRUE; }
#endif /* !_WIN32 */
#endif
