/*
 * Native GPU synchronisation (Street 1 hle_sync.c, RECOMP_HLE_NOGPU).
 *
 * 0x26CF10 is CDevice::KickOff (this = edx), the one place the library hands commands to the hardware: a PFB
 * write-back flush (0x26D0C0), PUT into the FIFO channel's user registers, and a wait for the GPU when the
 * GPU-less flag at 0x279B48 is set. The library also has its own GPU-less path (device flag 0x2000): a FIFO
 * block in its own data (0x27BFB0) with GET = PUT and every fence up to the current time complete. Natively
 * nothing executes the pushbuffer -- the renderer draws each call as it is made -- so every KickOff takes that
 * path, and the device is switched to it on the first one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "black_native.h"
#include "black_win32.h"
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* Vertical blank, natively. On the console the miniport's interrupt sets the device's vblank KEVENT (device +0x1DBC,
 * header written by D3D itself) 60 times a second, and D3D's BlockUntilVerticalBlank -- inlined in several game
 * threads, e.g. the loop at 0x1FBD80 -- resets SignalState and waits on it. With no GPU nothing set it: on Windows
 * every wait failed at once and those threads spun (tens of millions of KeWaitForSingleObject calls), on Linux the
 * guest address reached the POSIX layer as a handle. A host thread now sets it at the NTSC rate through the kernel's
 * title-event support (RECOMP_TITLE_KEVENTS=1, set by main.c). */
#define DEV_VBLANK_EVENT 0x1DBCu
void xbox_KeSetGuestEvent(uint32_t guest_va);
static volatile uint32_t s_vblank_dev;

#ifdef _WIN32
static DWORD WINAPI vblank_thread(LPVOID unused)
{
    LARGE_INTEGER f, t0, now;
    unsigned long long n = 0;
    (void)unused;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        long long due = t0.QuadPart + (long long)((double)f.QuadPart * (double)++n / 59.94);
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= due) break;
            if (due - now.QuadPart > f.QuadPart / 500) Sleep(1); else SwitchToThread();
        }
        xbox_KeSetGuestEvent(s_vblank_dev + DEV_VBLANK_EVENT);
    }
    return 0;
}
#else
static void *vblank_thread(void *unused)
{
    struct timespec t;
    (void)unused;
    clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        t.tv_nsec += 16683350;                     /* 59.94 Hz */
        if (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        xbox_KeSetGuestEvent(s_vblank_dev + DEV_VBLANK_EVENT);
    }
    return NULL;
}
#endif

static void vblank_start(uint32_t dev)
{
    if (s_vblank_dev)
        return;
    s_vblank_dev = dev;
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL));
#else
    {
        pthread_t th;
        pthread_create(&th, NULL, vblank_thread, NULL);
        pthread_detach(th);
    }
#endif
    fprintf(stderr, "[BLACK-SYNC] native vertical blank: event at %08X set at 59.94 Hz\n", dev + DEV_VBLANK_EVENT);
}

#define DEV_PUT        0x000u
#define DEV_FLAGS      0x008u
#define DEV_TIME       0x02Cu      /* fence time the library hands out */
#define DEV_SEMAPHORE  0x030u      /* -> fence time the GPU would write back */
#define DEV_FIFO       0x1C20u     /* -> FIFO user registers: +0x40 PUT, +0x44 GET */
#define GPULESS_FIFO   (0x27BFF0u - 0x40u)

volatile uint32_t black_kick_count;

/* BLACK_PB_CENSUS=<kicks>: diagnostic only -- count the NV097 methods written between kicks (nothing executes
 * them) and print the histogram every N kicks, to find what the game itself still pushes. */
static uint32_t s_scan;
static unsigned long long s_methods[0x800], s_words;
static void census(uint32_t put)
{
    static int every = -1;
    uint32_t va = s_scan, budget = 0x100000u;
    if (every < 0) { const char *e = getenv("BLACK_PB_CENSUS"); every = e ? atoi(e) : 0; }
    if (!every) return;
    if (va) {
        while (va != put && budget--) {
            uint32_t w = MEM32(va), count, i;
            if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
                va = 0x80000000u | (((w & 3u) == 1u ? w & 0x0FFFFFFCu : w & 0x1FFFFFFCu) & 0x0FFFFFFFu);
                continue;
            }
            count = (w >> 18) & 0x7FFu;
            for (i = 0; i < count; i++)
                s_methods[(((w & 0xE0000000u) == 0x40000000u ? w & 0x1FFCu : (w & 0x1FFCu) + 4 * i) >> 2) & 0x7FF]++;
            s_words += 1 + count;
            va += 4 + 4 * count;
        }
    }
    s_scan = put;
    if (black_kick_count % (uint32_t)every == 0) {
        fprintf(stderr, "[BLACK-PB] kick %u: %llu words;", black_kick_count, s_words);
        for (int m = 0; m < 0x800; m++)
            if (s_methods[m]) fprintf(stderr, " %04X=%llu", m * 4, s_methods[m]);
        fputc(10, stderr);
        memset(s_methods, 0, sizeof s_methods);
        s_words = 0;
    }
}

void sub_0026CF10(void)
{
    uint32_t dev = g_edx, put;
    black_kick_count++;
    vblank_start(dev);
    if (!(MEM32(dev + DEV_FLAGS) & 0x2000u)) {
        MEM32(dev + DEV_FLAGS) |= 0x2000u;
        fprintf(stderr, "[BLACK-SYNC] native KickOff: library GPU-less path (nothing reaches an NV2A)\n");
    }
    put = MEM32(dev + DEV_PUT) & 0x0FFFFFFFu;
    census(MEM32(dev + DEV_PUT));
    MEM32(dev + DEV_FIFO) = GPULESS_FIFO;
    MEM32(GPULESS_FIFO + 0x40) = put;
    MEM32(GPULESS_FIFO + 0x44) = put;
    MEM32(MEM32(dev + DEV_SEMAPHORE)) = MEM32(dev + DEV_TIME) - 2u;
    MEM32(dev + 0x1DE4) = MEM32(dev + 0x2478);
    g_esp += 4;                     /* ret */
}
