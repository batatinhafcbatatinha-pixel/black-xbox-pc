// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/*
 * Native D3D11 backend for the D3D8 HLE layer (hle_d3d8.c).
 *
 * Shadow mode: it watches the game's D3D8 calls and renders the same frame
 * into its own window while the original library keeps driving the NV2A path
 * that is shown in the main window. Enabled by RECOMP_HLE_SHADOW=1.
 */
#ifndef HLE_D3D11_H
#define HLE_D3D11_H

#include <stdint.h>

enum {
#define HLE_API(va, name, conv, args) HLE_##name,
#include "hle_d3d8_table.h"
#undef HLE_API
    HLE_COUNT
};

int  hle_native_enabled(void);
/* Primary renderer and RECOMP_HLE_LIBDRAW unset: game draws are not passed
 * to the original library (no pushbuffer for them). */
int  hle_native_owns_draws(void);
/* ... and state setters keep only the library's CPU-side copies (no push). */
int  hle_native_owns_state(void);
void hle_native_device_created(uint32_t pp_va);
void hle_native_clear(uint32_t count, uint32_t rects_va, uint32_t flags,
                      uint32_t color, float z, uint32_t stencil);
void hle_native_swap(uint32_t flags);
/* CopyRects between surfaces the backend renders (GPU to GPU), and LockRect
 * of one: its native pixels written back to guest memory first. */
void hle_native_copy_rects(uint32_t src, uint32_t rects, uint32_t count, uint32_t dst, uint32_t points);
void hle_native_lock_rect(uint32_t surf, uint32_t locked);
/* Frame pacing after the library's Swap (primary mode). */
void hle_native_pace(void);
/* Every routed call: depth 0 = made by the game, > 0 = made by the library
 * itself; phase 0 before the library body with its stack arguments a[] and
 * fastcall registers, phase 1 after it with eax. */
void hle_native_observe(int id, int depth, int phase, const uint32_t a[6],
                        uint32_t ecx, uint32_t edx, uint32_t eax);

#endif
