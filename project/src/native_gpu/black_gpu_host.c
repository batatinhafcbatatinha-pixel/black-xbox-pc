/* Optional host UI is outside the guest D3D API; no fake game API results. */
#include "hle_options.h"
#include "black_gpu.h"
#include "hle_gpu_api.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <SDL2/SDL.h>
#endif
/* Native CPU hosts use preemptive OS threads, with no global guest-CPU lock.
 * A future serialized host must replace this scheduling boundary explicitly. */
int guest_cpu_suspend(void){return 0;}
void guest_cpu_resume(int token){(void)token;}
void hle_stall_tick(void){} /* diagnostic watchdog is not part of frame semantics */
/* hle_gpu_api.c: D3D11 or Vulkan (DXVK in vulkan\ next to the executable). */

static BlackGpuVolumeCallback s_volume_callback;
void black_options_volume(float volume) { if (s_volume_callback) s_volume_callback(volume); }
void BlackGpuInitOptions(BlackGpuVolumeCallback callback) {
    s_volume_callback = callback;
#ifdef _WIN32
    SetProcessDPIAware();
#endif
    hle_options_init();
    hle_gpu_api_load();             /* before any D3D11/DXGI call: binds DXVK when Vulkan is chosen */
}

/* Opt-in timing evidence: the AOT timer at 0x82210 accumulates milliseconds
 * from the scaled RDTSC; 0x2BFF9C counts its update cycles. Read only. */
void black_options_timing(void) {
    static int enabled = -1;
    static ULONGLONG start, last;
    if (enabled < 0) enabled = getenv("BLACK_TIMING_TRACE") != NULL;
    if (!enabled) return;
#ifdef _WIN32
    ULONGLONG now = GetTickCount64();
#else
    ULONGLONG now = SDL_GetTicks64();
#endif
    if (!start) start = now;
    if (now - last < 1000) return;
    last = now;
    extern ptrdiff_t g_xbox_mem_offset;
    uint32_t ms = *(volatile uint32_t *)(g_xbox_mem_offset + 0x2C02A0);
    uint32_t cycles = *(volatile uint32_t *)(g_xbox_mem_offset + 0x2BFF9C);
    fprintf(stderr, "[BLACK-TIMING] wall_ms=%llu swaps=%llu timer_ms=%u cycles=%u vsync=%d fps=%d\n",
        now-start, (unsigned long long)g_hle.swaps, ms, cycles, g_opt.vsync, g_opt.fps);
}
