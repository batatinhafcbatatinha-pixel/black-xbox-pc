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
typedef void (*BlackGpuKeyCallback)(int key, int down);
BLACK_GPU_API int BlackGpuAttachMemory(ptrdiff_t address_offset, BlackGpuKeyCallback key_callback);
BLACK_GPU_API int BlackGpuRoute(uint32_t function_va, int phase, int call_depth,
                               const BlackGpuCall* before, uint32_t returned_eax);
BLACK_GPU_API const char* BlackGpuApiName(uint32_t function_va);
BLACK_GPU_API int BlackGpuApiCount(void);
BLACK_GPU_API int BlackGpuReady(void);
#ifdef __cplusplus
}
#endif
