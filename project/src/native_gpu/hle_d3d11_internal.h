// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/* Shared between hle_d3d11.c (device, surfaces, present) and hle_draw.c. */
#ifndef HLE_D3D11_INTERNAL_H
#define HLE_D3D11_INTERNAL_H

#define COBJMACROS
#ifdef _WIN32
#include <windows.h>
#else
#include "hle_posix_win32.h"      /* DXVK native's <windows.h> + the rest */
#endif
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include "hle_d3d11.h"

#define D3D_DEVICE_PTR_VA   0x27BFF8u
#define DEV_RENDER_TARGET   0x1A04u
#define DEV_DEPTH_STENCIL   0x1A08u
#define DEV_VERTEX_SHADER   0x0794u

typedef struct HleSurface {
    uint32_t data, format, size;          /* guest identity */
    uint32_t w, h;                        /* logical size */
    uint32_t tw, th;                      /* native texture size */
    float xk;                             /* tw / (w * scale): > 1 on widened surfaces */
    int depth;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;        /* colour surfaces, lazily */
    uint64_t last_use;
} HleSurface;

#define MAX_SURFACES 64

typedef struct HleBackend {
    int state;                            /* 0 unknown, 1 on, -1 off/failed */
    int log;
    int primary;                          /* RECOMP_HLE_PRIMARY: on screen, NV2A off */
    unsigned fps_reports;
    float scale;                          /* RECOMP_HLE_SCALE: 1..6, fractional for "native" */
    float aspect;                         /* RECOMP_HLE_ASPECT, 0 = the game's own */
    int anamorphic;                       /* the game draws 16:9 itself (Xbox set to widescreen):
                                             widened surfaces are a plain horizontal stretch */
    uint32_t pp_w, pp_h;
    CRITICAL_SECTION lock;

    HWND hwnd;
    HANDLE window_ready;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11DeviceContext1 *ctx1;
    IDXGIFactory2 *factory;
    IDXGISwapChain1 *swap;
    uint32_t swap_w, swap_h;

    HleSurface surf[MAX_SURFACES];
    uint64_t swaps, clears, partial_clears;
    uint64_t title_swaps, title_draws;
    DWORD title_tick;

    const char *dump_prefix;
    unsigned dump_every;
    uint32_t last_present;                /* data address of the last presented target */
    int front_wanted;                     /* something samples the front buffer */
    uint64_t copies, readbacks;
} HleBackend;

extern HleBackend g_hle;

uint32_t hle_device_va(void);
HleSurface *hle_surface_for(uint32_t va);
ID3D11ShaderResourceView *hle_surface_srv(uint32_t data);
int hle_surface_is_depth(uint32_t data);
/* A texture over the front buffer's memory: the last presented frame. */
ID3D11ShaderResourceView *hle_front_srv(uint32_t data);
/* Logical x (game pixels) to native x on surface s: centred, so a widened
 * surface keeps the game's 4:3 picture in the middle. */
static __inline float hle_map_x(const HleSurface *s, float x)
{
    if (g_hle.anamorphic && s->xk > 1.001f)
        return x * (float)s->tw / (float)s->w;
    return (float)s->tw * 0.5f + (x - (float)s->w * 0.5f) * (float)g_hle.scale;
}

/* D3DCompile for every shader the backend builds (hle_gpu_api.c): the
 * system's d3dcompiler on Windows, vkd3d-shader on Linux, and with
 * RECOMP_HLE_DUMP_HLSL=<dir> each source is also written there. */
HRESULT hle_d3d_compile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defines,
                        ID3DInclude *include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                        ID3DBlob **code, ID3DBlob **errors);

/* hle_draw.c */
int  hle_draw_init(void);
void hle_draw_flush(void);
/* The draw path's cached context bindings are stale (another binder ran). */
void hle_draw_state_invalidate(void);
void hle_draw_clear_rect(HleSurface *rt, HleSurface *ds, const D3D11_RECT *r, uint32_t flags,
                         const float c[4], float z, uint32_t stencil);
void hle_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *tris);
/* Time spent since the last call compiling vertex shaders and decoding textures. */
void hle_draw_frame_costs(double *vs_ms, unsigned *vs_n, double *tex_ms, unsigned *tex_n);
/* Time spent in the backend's draws since the last call. */
double hle_draw_frame_draw_ms(void);

#endif
