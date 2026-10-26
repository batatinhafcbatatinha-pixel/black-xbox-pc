// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/* Graphics API choice (hle_gpu_api.c): Direct3D 11 or Vulkan (DXVK). */
#ifndef HLE_GPU_API_H
#define HLE_GPU_API_H

enum { HLE_API_D3D11 = 0, HLE_API_VULKAN = 1 };

/* RECOMP_HLE_API, read once. */
int hle_gpu_api(void);
const char *hle_gpu_api_name(void);
/* A Vulkan loader on the system and DXVK next to the executable. */
int hle_gpu_vulkan_available(void);
/* The system has a D3D11 of its own (Windows); elsewhere only Vulkan. */
int hle_gpu_d3d11_available(void);
/* Before any D3D11/DXGI call: binds DXVK when Vulkan is chosen (falls back
 * to D3D11 when it cannot). */
void hle_gpu_api_load(void);

#endif
