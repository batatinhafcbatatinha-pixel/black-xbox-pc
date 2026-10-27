#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef BLACK_GPU_BUILD
#define BLACK_GPU_API __declspec(dllexport)
#else
#define BLACK_GPU_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Snapshot BEFORE the original CPU library call. Keep it for both phases.
 * gpr order: eax,ecx,edx,ebx,esp,ebp,esi,edi. stack[] excludes return PC.
 * Address mapping must include the guest's contiguous-memory aliases.
 * Routing observes native work; it does not implement kernel/miniport init. */
typedef struct BlackGpuCall { uint32_t gpr[8]; uint32_t stack[6]; } BlackGpuCall;
typedef void (*BlackGpuVolumeCallback)(float volume);
BLACK_GPU_API void BlackGpuInitOptions(BlackGpuVolumeCallback volume_callback);
typedef void (*BlackGpuKeyCallback)(int key, int down);
BLACK_GPU_API int BlackGpuAttachMemory(ptrdiff_t address_offset, BlackGpuKeyCallback key_callback);
BLACK_GPU_API int BlackGpuRoute(uint32_t function_va, int phase, int call_depth,
                               const BlackGpuCall* before, uint32_t returned_eax);
BLACK_GPU_API const char* BlackGpuApiName(uint32_t function_va);
BLACK_GPU_API int BlackGpuApiCount(void);
BLACK_GPU_API int BlackGpuReady(void);
/* Mouse for the modern controls (black_mouse.c): relative motion and buttons since the last read (raw input on the
 * game window), and whether the cursor is held in the window and hidden (gameplay) or free (menus). */
enum { BLACK_MOUSE_LEFT = 1, BLACK_MOUSE_RIGHT = 2, BLACK_MOUSE_MIDDLE = 4, BLACK_MOUSE_X1 = 8, BLACK_MOUSE_X2 = 16 };
BLACK_GPU_API void BlackGpuMouseRead(int* dx, int* dy, unsigned* buttons, int* wheel);
BLACK_GPU_API void BlackGpuMouseCapture(int on);
#ifdef __cplusplus
}
#endif
