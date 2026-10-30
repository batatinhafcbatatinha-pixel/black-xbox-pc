// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/*
 * Native D3D11 backend for the D3D8 HLE layer -- shadow mode.
 *
 * hle_d3d8.c calls in here around the original library bodies. Nothing is
 * read back into guest memory: the game keeps running on the original D3D8
 * library and the NV2A path, and this renders its own copy of each frame
 * into a separate window ("sombra"), so the two can be compared until this
 * one draws the same image and the NV2A path can be switched off.
 *
 * Render targets are not tracked from the calls but read from the device
 * itself at the moment they matter (Clear, Swap, later draws): the library
 * keeps the current colour/depth surfaces at D3D_g_pDevice+0x1A04/+0x1A08.
 * Each guest surface (Data address + format + size) gets a native texture at
 * the logical size times RECOMP_HLE_SCALE; a back buffer stored with 2x
 * multisampling (twice the size given at CreateDevice) maps to the logical
 * size, as the D3D8 API works in logical coordinates.
 *
 * RECOMP_HLE_SHADOW=1     enable
 * RECOMP_HLE_SCALE=<1..4> internal resolution multiplier (default 1)
 * RECOMP_HLE_LOG=1        log surfaces and calls the backend does not handle
 * RECOMP_FB_DUMP=<prefix> with RECOMP_HLE_DUMP_EVERY=<swaps> (default 300):
 *                         write <prefix>-hle<swap>.bmp
 */
#define COBJMACROS
#ifdef _WIN32
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#else
#include "hle_d3d11_internal.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../recomp/recomp_types.h"
#include "hle_d3d11_internal.h"
#include "hle_options.h"
#include "hle_gpu_api.h"

/* Xbox D3DCLEAR */
#define XCLEAR_ZBUFFER      0x01u
#define XCLEAR_STENCIL      0x02u
#define XCLEAR_TARGET       0xF0u

HleBackend g_hle;
static void msaa_choose(void);
static int s_present_trace = -1;

static int hle_present_trace_enabled(void)
{
    if (s_present_trace < 0)
        s_present_trace = getenv("RECOMP_HLE_PRESENT_TRACE") != NULL;
    return s_present_trace;
}


/* ---------------------------------------------------------------- window */

extern void xbox_FramebufferKeyEvent(int vk, int down);
extern volatile int xbox_nv2a_draw_disabled;

#ifdef _WIN32
int black_mouse_wndproc(HWND h, UINT m, WPARAM w, LPARAM l);
static LRESULT CALLBACK shadow_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_hle.primary) {
        int r = black_mouse_wndproc(h, m, w, l);   /* raw mouse for the modern controls */
        if (r == 2)
            return TRUE;
        if (m == WM_INPUT)
            return DefWindowProcW(h, m, w, l);
    }
    switch (m) {
    case WM_CLOSE:
        if (g_hle.primary)                /* the game's window: closing quits */
            ExitProcess(0);
        ShowWindow(h, SW_MINIMIZE);       /* shadow: the game owns the lifetime */
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP:
        /* The options menu (F1, Alt+Enter) sees keys first; while it is
         * open the game gets none. System keys still reach Windows so
         * Alt+F4 closes, except Alt+Enter. */
        if (g_hle.primary && hle_options_key(m, w, l)) {
            if (m == WM_KEYDOWN || m == WM_KEYUP || w == VK_RETURN)
                return 0;
            break;
        }
        if (g_hle.primary)
            xbox_FramebufferKeyEvent((int)w, m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
        if (m == WM_KEYDOWN || m == WM_KEYUP)
            return 0;
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MOUSEWHEEL:
        if (g_hle.primary) {
            RECT cr;
            POINT p;
            p.x = (short)LOWORD(l);
            p.y = (short)HIWORD(l);
            if (m == WM_MOUSEWHEEL)               /* wheel positions are on the screen */
                ScreenToClient(h, &p);
            GetClientRect(h, &cr);
            if (hle_options_mouse(m, p.x, p.y, m == WM_MOUSEWHEEL ? (short)HIWORD(w) : 0, cr.right, cr.bottom))
                return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (g_hle.primary)
            xbox_FramebufferKeyEvent(-1, 0);
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN)                   /* Alt+Enter: no menu beep */
            return 0;
        break;
    case WM_SETCURSOR:
        if (g_hle.primary && LOWORD(l) == HTCLIENT && hle_options_hide_cursor()) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI shadow_window_thread(LPVOID unused)
{
    WNDCLASSW wc;
    RECT r = { 0, 0, 640, 480 };
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    MSG msg;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = shadow_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"FifaHleShadow";
    RegisterClassW(&wc);
    if (g_hle.pp_w && g_hle.pp_h) {
        r.right = (LONG)((float)g_hle.pp_w * g_hle.scale + 0.5f);
        r.bottom = (LONG)((float)g_hle.pp_h * g_hle.scale + 0.5f);
        if (g_hle.aspect > 0.0f)
            r.right = (LONG)((float)r.bottom * g_hle.aspect + 0.5f);
    }
    if (g_hle.primary)                    /* window or fullscreen, on the chosen monitor */
        hle_options_initial_window(&style, &r);
    else
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hle.hwnd = CreateWindowExW(0, wc.lpszClassName, g_hle.primary ? L"BLACK" : L"Black - HLE D3D11 (sombra)",
                             style, g_hle.primary ? r.left : CW_USEDEFAULT, g_hle.primary ? r.top : CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetEvent(g_hle.window_ready);
    if (!g_hle.hwnd)
        return 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* Client area in pixels; 0 while minimised. */
static int window_client_size(UINT *w, UINT *h)
{
    RECT cr;
    if (!GetClientRect(g_hle.hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0)
        return 0;
    *w = (UINT)cr.right;
    *h = (UINT)cr.bottom;
    return 1;
}

static void window_set_title(const wchar_t *t)
{
    SetWindowTextW(g_hle.hwnd, t);
}
#else
/* Linux/macOS: an SDL2 window (DXVK native presents into it: its HWND is
 * the SDL_Window*), created and pumped on its own thread like the Win32
 * one. Keys go to the game and the options screen as Windows virtual-key
 * codes, the language the rest of the runtime speaks. */
static int sdl_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
    case SDLK_UP: return 0x26;     case SDLK_DOWN: return 0x28;
    case SDLK_LEFT: return 0x25;   case SDLK_RIGHT: return 0x27;
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_ESCAPE: return 0x1B; case SDLK_BACKSPACE: return 0x08;
    case SDLK_SPACE: return 0x20;  case SDLK_TAB: return 0x09;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_KP_2: return 0x62;   case SDLK_KP_4: return 0x64;
    case SDLK_KP_6: return 0x66;   case SDLK_KP_8: return 0x68;
    }
    return -1;
}

void hle_window_sdl_events(SDL_Event *e);   /* hle_options.c: display changes */
void black_mouse_sdl_event(const SDL_Event *e);

static DWORD shadow_window_thread(LPVOID unused)
{
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 640, h = 480;
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *win;
    SDL_Event e;
    (void)unused;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[HLE-D3D11] SDL video init failed: %s\n", SDL_GetError());
        SetEvent(g_hle.window_ready);
        return 0;
    }
    if (g_hle.pp_w && g_hle.pp_h) {
        w = (int)((float)g_hle.pp_w * g_hle.scale + 0.5f);
        h = (int)((float)g_hle.pp_h * g_hle.scale + 0.5f);
        if (g_hle.aspect > 0.0f)
            w = (int)((float)h * g_hle.aspect + 0.5f);
    }
    if (g_hle.primary)
        hle_options_initial_window_sdl(&x, &y, &w, &h, &flags);
    win = SDL_CreateWindow(g_hle.primary ? "Black 2" : "Black (sombra)", x, y, w, h, flags);
    if (!win)
        fprintf(stderr, "[HLE-D3D11] SDL_CreateWindow failed: %s\n", SDL_GetError());
    g_hle.hwnd = (HWND)win;
    SetEvent(g_hle.window_ready);
    if (!win)
        return 0;
    while (SDL_WaitEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            if (g_hle.primary)
                ExitProcess(0);
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int vk = sdl_vk(e.key.keysym.sym), down = e.type == SDL_KEYDOWN;
            UINT msg = down ? 0x0100u : 0x0101u;               /* WM_KEYDOWN / WM_KEYUP */
            LPARAM lp = e.key.repeat ? (1 << 30) : 0;
            if (vk < 0)
                break;
            if (vk == 0x0D && (e.key.keysym.mod & KMOD_ALT)) { /* Alt+Enter, as WM_SYSKEYDOWN */
                msg = down ? 0x0104u : 0x0105u;
                lp |= 1 << 29;
            }
            if (g_hle.primary && hle_options_key(msg, (WPARAM)vk, lp))
                break;
            if (g_hle.primary && !e.key.repeat)
                xbox_FramebufferKeyEvent(vk, down);
            break;
        }
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEWHEEL:
            if (g_hle.primary) {
                int cw = 0, ch = 0, mx = 0, my = 0;
                UINT msg = 0x0200u;
                SDL_GetWindowSize(win, &cw, &ch);
                SDL_GetMouseState(&mx, &my);
                if (e.type == SDL_MOUSEBUTTONDOWN)
                    msg = e.button.button == SDL_BUTTON_LEFT ? 0x0201u : e.button.button == SDL_BUTTON_RIGHT ? 0x0204u : 0u;
                else if (e.type == SDL_MOUSEWHEEL)
                    msg = 0x020Au;
                if (msg)
                    hle_options_mouse(msg, mx, my, e.type == SDL_MOUSEWHEEL ? e.wheel.y : 0, cw, ch);
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && g_hle.primary)
                xbox_FramebufferKeyEvent(-1, 0);
            break;
        }
        black_mouse_sdl_event(&e);           /* black_mouse.c: the modern controls' mouse */
        hle_window_sdl_events(&e);
    }
    return 0;
}

static int window_client_size(UINT *w, UINT *h)
{
    int pw = 0, ph = 0;
    if (SDL_GetWindowFlags((SDL_Window *)g_hle.hwnd) & SDL_WINDOW_MINIMIZED)
        return 0;
    SDL_Vulkan_GetDrawableSize((SDL_Window *)g_hle.hwnd, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return 0;
    *w = (UINT)pw;
    *h = (UINT)ph;
    return 1;
}

static void window_set_title(const wchar_t *t)
{
    char u[512];
    size_t n = wcstombs(u, t, sizeof u - 1);
    u[n == (size_t)-1 ? 0 : n] = 0;
    SDL_SetWindowTitle((SDL_Window *)g_hle.hwnd, u);
}
#endif

/* ---------------------------------------------------------------- device */

/* RECOMP_HLE_D3DDEBUG=1: the D3D11 debug layer's messages, printed at each
 * Swap (each distinct message id at most 8 times). */
static ID3D11InfoQueue *s_info;

static void debug_messages(void)
{
    static unsigned shown[4096];
    UINT64 n, i;
    if (!s_info)
        return;
    n = ID3D11InfoQueue_GetNumStoredMessages(s_info);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;
        if (FAILED(ID3D11InfoQueue_GetMessage(s_info, i, NULL, &len)) || !len)
            continue;
        m = (D3D11_MESSAGE *)malloc(len);
        if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(s_info, i, m, &len)) &&
            m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING &&
            shown[(unsigned)m->ID & 4095u]++ < 8)
            fprintf(stderr, "[HLE-D3DDBG] swap %llu: %.*s\n", (unsigned long long)g_hle.swaps,
                    (int)m->DescriptionByteLength, m->pDescription);
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(s_info);
}

static int create_device(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC ad;
    HRESULT hr;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const char *dbg = getenv("RECOMP_HLE_D3DDEBUG");
    /* RECOMP_HLE_ADAPTER=<n>: DXGI adapter n (the options' GPU), else the
     * system's default. */
    const char *ad_env = getenv("RECOMP_HLE_ADAPTER");
    IDXGIAdapter1 *chosen = NULL;
    if (dbg && *dbg == '1')
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#ifndef _WIN32
    /* DXVK native presents through SDL2; it needs SDL's video up before it
     * enumerates the Vulkan instance extensions. */
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "[HLE-D3D11] SDL video init failed: %s\n", SDL_GetError());
#endif
    if (ad_env && *ad_env && atoi(ad_env) >= 0) {
        IDXGIFactory1 *f1 = NULL;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) {
            if (FAILED(IDXGIFactory1_EnumAdapters1(f1, (UINT)atoi(ad_env), &chosen)))
                chosen = NULL;
            IDXGIFactory1_Release(f1);
        }
        if (!chosen)
            fprintf(stderr, "[HLE-D3D11] adapter %s not found: using the default\n", ad_env);
    }
    hr = D3D11CreateDevice((IDXGIAdapter *)chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                           levels, 2, D3D11_SDK_VERSION, &g_hle.dev, NULL, &g_hle.ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice((IDXGIAdapter *)chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                               levels + 1, 1, D3D11_SDK_VERSION, &g_hle.dev, NULL, &g_hle.ctx);
    if (FAILED(hr) && chosen) {
        fprintf(stderr, "[HLE-D3D11] chosen adapter failed 0x%08lX: using the default\n", (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                               levels, 2, D3D11_SDK_VERSION, &g_hle.dev, NULL, &g_hle.ctx);
    }
    if (chosen)
        IDXGIAdapter1_Release(chosen);
    if (SUCCEEDED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) &&
        SUCCEEDED(ID3D11Device_QueryInterface(g_hle.dev, &IID_ID3D11InfoQueue, (void **)&s_info)))
        fprintf(stderr, "[HLE-D3D11] debug layer on (RECOMP_HLE_D3DDEBUG)\n");
    if (FAILED(hr)) {
        fprintf(stderr, "[HLE-D3D11] D3D11CreateDevice failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(g_hle.ctx, &IID_ID3D11DeviceContext1, (void **)&g_hle.ctx1)))
        g_hle.ctx1 = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_hle.dev, &IID_IDXGIDevice, (void **)&dxgi_dev))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter))) {
            IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&g_hle.factory);
            if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)))
                fprintf(stderr, "[HLE-D3D11] device on %ls (%s), scale %.3gx, ClearView %s\n",
                        ad.Description, hle_gpu_api_name(), (double)g_hle.scale, g_hle.ctx1 ? "yes" : "no");
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi_dev);
    }
    if (!g_hle.factory) {
        fprintf(stderr, "[HLE-D3D11] no IDXGIFactory2\n");
        return 0;
    }
    g_hle.window_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, shadow_window_thread, NULL, 0, NULL));
    WaitForSingleObject(g_hle.window_ready, 5000);
    if (!g_hle.hwnd) {
        fprintf(stderr, "[HLE-D3D11] shadow window failed\n");
        return 0;
    }
    return 1;
}

int hle_native_enabled(void)
{
    if (g_hle.state == 0) {
        const char *e = getenv("RECOMP_HLE_SHADOW"), *pr = getenv("RECOMP_HLE_PRIMARY");
        g_hle.primary = pr && atoi(pr);
        g_hle.state = ((e && atoi(e)) || g_hle.primary) ? 1 : -1;
        if (g_hle.state > 0) {
            const char *sc = getenv("RECOMP_HLE_SCALE");
            const char *lg = getenv("RECOMP_HLE_LOG");
            const char *de = getenv("RECOMP_HLE_DUMP_EVERY");
            const char *as = getenv("RECOMP_HLE_ASPECT");
            /* "native" (or 0): as many lines as the monitor has. */
            if (sc && (*sc == 'n' || (*sc && atof(sc) == 0.0)))
                g_hle.scale = hle_options_native_scale();
            else
                g_hle.scale = sc && *sc ? (float)atof(sc) : 1.0f;
            if (!(g_hle.scale >= 1.0f && g_hle.scale <= 9.0f))     /* 9 = 4320 lines (8K) */
                g_hle.scale = 1.0f;
            if (as && *as) {
                float a = 0.0f, b = 0.0f;
                if (sscanf(as, "%f:%f", &a, &b) == 2 && b > 0.0f)
                    a /= b;
                g_hle.aspect = a > 1.0f && a < 4.0f ? a : 0.0f;
                {
                    const char *ws = getenv("RECOMP_XBOX_WIDESCREEN");
                    g_hle.anamorphic = g_hle.aspect > 0.0f && ws && *ws && *ws != '0';
                    /* The game's wide camera is 16:9; other shapes get its
                     * 16:9 picture with bars rather than a distorted one. */
                    if (g_hle.anamorphic)
                        g_hle.aspect = 16.0f / 9.0f;
                }
            }
            g_hle.log = lg && atoi(lg);
            g_hle.dump_prefix = getenv("RECOMP_FB_DUMP");
            g_hle.dump_every = de ? (unsigned)atoi(de) : 300u;
            InitializeCriticalSection(&g_hle.lock);
        }
    }
    return g_hle.state > 0;
}

int hle_native_owns_draws(void)
{
    static int lib = -1;
    if (lib < 0) {
        const char *e = getenv("RECOMP_HLE_LIBDRAW");
        lib = e && atoi(e);
    }
    return g_hle.state > 0 && g_hle.primary && g_hle.dev && !lib;
}

/* Primary renderer with the library's draws off: state setters whose only
 * effect beyond the library's own CPU-side copies is pushbuffer for the NV2A
 * keep those copies and skip the push. RECOMP_HLE_LIBSTATE=1 runs the
 * original bodies again. */
int hle_native_owns_state(void)
{
    static int lib = -1;
    if (lib < 0) {
        const char *e = getenv("RECOMP_HLE_LIBSTATE");
        lib = e && atoi(e);
    }
    return hle_native_owns_draws() && !lib;
}

static void disable(const char *why)
{
    fprintf(stderr, "[HLE-D3D11] shadow renderer disabled: %s\n", why);
    g_hle.state = -1;
}

/* -------------------------------------------------------------- surfaces */

static int xfmt_is_depth(uint32_t f)
{
    return f >= 0x2A && f <= 0x31;
}

static void surface_release(HleSurface *h)
{
    if (h->rtv) ID3D11RenderTargetView_Release(h->rtv);
    if (h->dsv) ID3D11DepthStencilView_Release(h->dsv);
    if (h->srv) ID3D11ShaderResourceView_Release(h->srv);
    if (h->single_rtv) ID3D11RenderTargetView_Release(h->single_rtv);
    if (h->single_srv) ID3D11ShaderResourceView_Release(h->single_srv);
    if (h->single) ID3D11Texture2D_Release(h->single);
    if (h->tex) ID3D11Texture2D_Release(h->tex);
    memset(h, 0, sizeof *h);
}

/* Native surface for the guest D3DSurface at va (Common, Data, Lock, Format,
 * Size, Parent), created on first use. */
HleSurface *hle_surface_for(uint32_t va)
{
    uint32_t data, format, size, xfmt, w, h;
    HleSurface *slot = NULL, *lru = NULL;
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    int i;

    if (!va)
        return NULL;
    data = MEM32(va + 4);
    format = MEM32(va + 12);
    size = MEM32(va + 16);
    for (i = 0; i < MAX_SURFACES; i++) {
        HleSurface *c = &g_hle.surf[i];
        if (c->tex && c->data == data && c->format == format && c->size == size) {
            c->last_use = g_hle.swaps + 1;
            return c;
        }
        if (!c->tex && !slot)
            slot = c;
        if (c->tex && (!lru || c->last_use < lru->last_use))
            lru = c;
    }
    if (!slot) {
        slot = lru;
        surface_release(slot);
    }

    xfmt = (format >> 8) & 0xFF;
    if (size) {
        w = (size & 0xFFF) + 1;
        h = ((size >> 12) & 0xFFF) + 1;
    } else {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    }
    /* Back buffer / auto depth stored supersampled: back to logical size. */
    if (g_hle.pp_w && g_hle.pp_h) {
        if (w == 2 * g_hle.pp_w && (h == g_hle.pp_h || h == 2 * g_hle.pp_h))
            w = g_hle.pp_w;
        if (h == 2 * g_hle.pp_h && w == g_hle.pp_w)
            h = g_hle.pp_h;
    }

    memset(&td, 0, sizeof td);
    td.Width = (UINT)((float)w * g_hle.scale + 0.5f);
    td.Height = (UINT)((float)h * g_hle.scale + 0.5f);
    /* Widescreen (hor+): surfaces the size of the back buffer -- the back
     * buffer, its depth buffer, same-size targets -- get the wider aspect;
     * draws into them squeeze clip-space x by 1/xk, so the 4:3 picture keeps
     * its proportions in the middle and the 3D scene extends to the sides. */
    if (g_hle.aspect > (float)w / (float)h + 0.01f && w == g_hle.pp_w && h == g_hle.pp_h) {
        uint32_t nw = (uint32_t)((float)td.Height * g_hle.aspect + 0.5f);
        td.Width = (nw + 1) & ~1u;
    }
    td.MipLevels = 1;
    td.ArraySize = 1;
    /* MSAA (options): the back buffer, its depth buffer and same-size
     * targets -- the ones drawn and depth-tested together. */
    if (!g_hle.msaa)
        msaa_choose();
    td.SampleDesc.Count = g_hle.msaa > 1 && w == g_hle.pp_w && h == g_hle.pp_h ? g_hle.msaa : 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    slot->depth = xfmt_is_depth(xfmt);
    if (slot->depth) {
        /* Typeless: the depth buffer is also a texture when the title
         * points a texture header at its memory (game 0x191A30, +0x160). */
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    } else {
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    }
    hr = ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &slot->tex);
    if (SUCCEEDED(hr)) {
        if (slot->depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dd;
            memset(&dd, 0, sizeof dd);
            dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = ID3D11Device_CreateDepthStencilView(g_hle.dev, (ID3D11Resource *)slot->tex, &dd, &slot->dsv);
        }
        else
            hr = ID3D11Device_CreateRenderTargetView(g_hle.dev, (ID3D11Resource *)slot->tex, NULL, &slot->rtv);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[HLE-D3D11] surface %08X %ux%u fmt %02X: create failed 0x%08lX\n",
                data, w, h, xfmt, (unsigned long)hr);
        surface_release(slot);
        return NULL;
    }
    slot->data = data;
    slot->format = format;
    slot->size = size;
    slot->w = w;
    slot->h = h;
    slot->tw = td.Width;
    slot->th = td.Height;
    slot->samples = td.SampleDesc.Count;
    slot->xk = (float)td.Width / ((float)w * g_hle.scale);
    slot->last_use = g_hle.swaps + 1;
    if (g_hle.log)
        fprintf(stderr, "[HLE-D3D11] surface obj %08X data %08X fmt %02X size %08X -> %ux%u (native %ux%u)%s\n",
                va, data, xfmt, size, w, h, slot->tw, slot->th, slot->depth ? " depth" : "");
    return slot;
}

/* A colour surface the game now samples as a texture (render to texture):
 * the native render target itself, never a copy through guest memory. */
ID3D11ShaderResourceView *hle_surface_srv(uint32_t data)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++) {
        HleSurface *c = &g_hle.surf[i];
        if (c->tex && !c->depth && c->data == data) {
            if (c->samples > 1)
                return hle_surface_resolved(c) ? c->single_srv : NULL;
            if (!c->srv)
                ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)c->tex, NULL, &c->srv);
            return c->srv;
        }
    }
    /* A depth buffer read as a texture: its depth (R24); the shader turns it
     * into the bytes the Xbox's D24S8 memory holds (hle_surface_is_depth). */
    for (i = 0; i < MAX_SURFACES; i++) {
        HleSurface *c = &g_hle.surf[i];
        if (c->tex && c->depth && c->data == data) {
            if (c->samples > 1) {
                if (!c->srv)
                    fprintf(stderr, "[HLE-D3D11] depth buffer %08X sampled as a texture (MSAA: sample 0)\n", data);
                return hle_surface_resolved(c) ? c->single_srv : NULL;
            }
            if (!c->srv) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                memset(&sd, 0, sizeof sd);
                sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)c->tex, &sd, &c->srv);
                fprintf(stderr, "[HLE-D3D11] depth buffer %08X sampled as a texture\n", data);
            }
            return c->srv;
        }
    }
    return NULL;
}

int hle_surface_is_depth(uint32_t data)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (g_hle.surf[i].tex && g_hle.surf[i].data == data)
            return g_hle.surf[i].depth;
    return 0;
}

/* ------------------------------------------------------------------ MSAA */

/* Samples for the back-buffer-sized surfaces: the options' choice (-1 = what
 * the game asked for at CreateDevice), lowered to what the card renders in
 * both surface formats. */
static void msaa_choose(void)
{
    UINT want = g_opt.msaa < 0 ? g_hle.game_msaa : (UINT)g_opt.msaa, q;
    if (want > 8)
        want = 8;
    while (want & (want - 1))
        want &= want - 1;   /* a power of two: 9 samples -> 8 */
    for (; want > 1; want /= 2) {
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(g_hle.dev, DXGI_FORMAT_B8G8R8A8_UNORM, want, &q)) && q &&
            SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(g_hle.dev, DXGI_FORMAT_D24_UNORM_S8_UINT, want, &q)) && q)
            break;
    }
    g_hle.msaa = want > 1 ? want : 1;
    fprintf(stderr, "[HLE-D3D11] MSAA: %ux (option %d, the game asked for %ux)\n", g_hle.msaa, g_opt.msaa,
            g_hle.game_msaa);
}

/* Fullscreen-triangle copies for MSAA surfaces: ps_copy writes a texel of a
 * single-sample source (offset by c.xy) into a multisampled target, ps_depth
 * sample 0 of a multisampled depth buffer into an R32_FLOAT texture. */
static ID3D11VertexShader *s_ms_vs;
static ID3D11PixelShader *s_ms_copy_ps, *s_ms_depth_ps;
static ID3D11Buffer *s_ms_cb;
static ID3D11RasterizerState *s_ms_rs;
static ID3D11DepthStencilState *s_ms_dss;

static int msaa_shaders(void)
{
    static const char source[] =
        "cbuffer C:register(b0){int4 c;};"
        "Texture2D<float4> t:register(t0); Texture2DMS<float4> m:register(t0);"
        "float4 vs(uint id:SV_VertexID):SV_Position{float2 q=float2((id<<1)&2,id&2);"
        "return float4(q*float2(2,-2)+float2(-1,1),0,1);}"
        "float4 ps_copy(float4 p:SV_Position):SV_Target{return t.Load(int3(int2(p.xy)+c.xy,0));}"
        "float ps_depth(float4 p:SV_Position):SV_Target{return m.Load(int2(p.xy),0).r;}";
    static int failed;
    const char *entry[3] = { "vs", "ps_copy", "ps_depth" }, *profile[3] = { "vs_5_0", "ps_5_0", "ps_5_0" };
    ID3DBlob *code[3] = { NULL, NULL, NULL };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;
    int i, ok = 1;

    if (s_ms_vs)
        return 1;
    if (failed)
        return 0;
    for (i = 0; i < 3 && ok; i++) {
        ID3DBlob *error = NULL;
        if (FAILED(hle_d3d_compile(source, sizeof source - 1, "hle_msaa", NULL, NULL, entry[i], profile[i],
                                   0, 0, &code[i], &error)))
            ok = 0;
        if (error) {
            if (!ok)
                fprintf(stderr, "[HLE-D3D11] MSAA %s: %s\n", entry[i], (const char *)ID3D10Blob_GetBufferPointer(error));
            ID3D10Blob_Release(error);
        }
    }
    if (ok)
        ok = SUCCEEDED(ID3D11Device_CreateVertexShader(g_hle.dev, ID3D10Blob_GetBufferPointer(code[0]),
                                                       ID3D10Blob_GetBufferSize(code[0]), NULL, &s_ms_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(code[1]),
                                                      ID3D10Blob_GetBufferSize(code[1]), NULL, &s_ms_copy_ps)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(code[2]),
                                                      ID3D10Blob_GetBufferSize(code[2]), NULL, &s_ms_depth_ps));
    for (i = 0; i < 3; i++)
        if (code[i]) ID3D10Blob_Release(code[i]);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (ok)
        ok = SUCCEEDED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &s_ms_cb)) &&
             SUCCEEDED(ID3D11Device_CreateRasterizerState(g_hle.dev, &rd, &s_ms_rs)) &&
             SUCCEEDED(ID3D11Device_CreateDepthStencilState(g_hle.dev, &dd, &s_ms_dss));
    if (!ok) {
        failed = 1;
        fprintf(stderr, "[HLE-D3D11] MSAA copy shaders unavailable\n");
    }
    return ok;
}

/* Draws `src` into `rtv` over the rectangle r, reading texel (x + ox, y + oy)
 * of the source for target pixel (x, y). */
static void msaa_draw(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *src, ID3D11PixelShader *ps,
                      const D3D11_RECT *r, int ox, int oy)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *none = NULL;

    hle_draw_state_invalidate();
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)s_ms_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        int c[4] = { ox, oy, 0, 0 };
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)s_ms_cb, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->right; vp.Height = (float)r->bottom;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(g_hle.ctx, 1, r);
    ID3D11DeviceContext_RSSetState(g_hle.ctx, s_ms_rs);
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, s_ms_dss, 0);
    ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_hle.ctx, s_ms_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_hle.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_hle.ctx, 0, 1, &s_ms_cb);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(g_hle.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 0, NULL, NULL);
}

ID3D11Texture2D *hle_surface_resolved(HleSurface *s)
{
    D3D11_TEXTURE2D_DESC td;

    if (!s || !s->tex)
        return NULL;
    if (s->samples <= 1)
        return s->tex;
    if (!s->single) {
        ID3D11Texture2D_GetDesc(s->tex, &td);
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (s->depth) {
            td.Format = DXGI_FORMAT_R32_FLOAT;
            td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        if (FAILED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &s->single)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)s->single, NULL, &s->single_srv)) ||
            (s->depth && FAILED(ID3D11Device_CreateRenderTargetView(g_hle.dev, (ID3D11Resource *)s->single, NULL,
                                                                    &s->single_rtv)))) {
            fprintf(stderr, "[HLE-D3D11] MSAA: no single-sample copy for surface %08X\n", s->data);
            return NULL;
        }
    }
    if (!s->depth) {
        ID3D11DeviceContext_ResolveSubresource(g_hle.ctx, (ID3D11Resource *)s->single, 0, (ID3D11Resource *)s->tex, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
    } else {
        D3D11_RECT r = { 0, 0, (LONG)s->tw, (LONG)s->th };
        if (!msaa_shaders())
            return NULL;
        if (!s->srv) {   /* the depth buffer's own (multisampled) view */
            D3D11_SHADER_RESOURCE_VIEW_DESC sd;
            memset(&sd, 0, sizeof sd);
            sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            if (FAILED(ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)s->tex, &sd, &s->srv)))
                return NULL;
        }
        msaa_draw(s->single_rtv, s->srv, s_ms_depth_ps, &r, 0, 0);
    }
    return s->single;
}

/* CopyRects with a multisampled source or destination (D3D11 copies only
 * whole subresources between equal sample counts): from the source's
 * single-sample copy; into a multisampled destination with a draw. */
static void msaa_copy(HleSurface *dst, HleSurface *src, const D3D11_BOX *box, UINT ox, UINT oy)
{
    ID3D11Texture2D *from;
    if (src->depth || dst->depth || !(from = hle_surface_resolved(src)))
        return;
    if (dst->samples <= 1) {
        ID3D11DeviceContext_CopySubresourceRegion(g_hle.ctx, (ID3D11Resource *)dst->tex, 0, ox, oy, 0,
                                                  (ID3D11Resource *)from, 0, box);
    } else if (msaa_shaders()) {
        D3D11_RECT r = { (LONG)ox, (LONG)oy, (LONG)(ox + box->right - box->left), (LONG)(oy + box->bottom - box->top) };
        ID3D11ShaderResourceView *view = src->samples > 1 ? src->single_srv : hle_surface_srv(src->data);
        if (view)
            msaa_draw(dst->rtv, view, s_ms_copy_ps, &r, (int)box->left - (int)ox, (int)box->top - (int)oy);
    }
}

/* The front buffer. On the Xbox every Swap leaves the finished frame there
 * (the library filters the back buffer into it), and the replay's effect
 * samples it through a texture header over that memory (game 0x1936B0). The
 * backend presents straight from the render target, so the front buffer's
 * memory is never written: once a texture over it is sampled, each Swap
 * copies the presented target into a native surface for the front buffer,
 * which tex_get then finds by address. */
static void front_update(HleSurface *rt)
{
    uint32_t dev = hle_device_va(), fo = dev ? MEM32(dev + 0x1A18) : 0;
    HleSurface *fr;
    if (!fo || !rt || MEM32(fo + 4) == rt->data)
        return;
    fr = hle_surface_for(fo);
    if (fr && fr != rt && !fr->depth && fr->tw == rt->tw && fr->th == rt->th && fr->samples == rt->samples)
        ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)fr->tex, (ID3D11Resource *)rt->tex);
}

ID3D11ShaderResourceView *hle_front_srv(uint32_t data)
{
    uint32_t dev = hle_device_va(), fo = dev ? MEM32(dev + 0x1A18) : 0;
    int i;
    if (!fo || MEM32(fo + 4) != data)
        return NULL;
    if (!g_hle.front_wanted) {
        g_hle.front_wanted = 1;
        fprintf(stderr, "[HLE-D3D11] the front buffer (%08X) is sampled: keeping the presented frame there\n", data);
        for (i = 0; i < MAX_SURFACES; i++)
            if (g_hle.surf[i].tex && !g_hle.surf[i].depth && g_hle.surf[i].data == g_hle.last_present) {
                front_update(&g_hle.surf[i]);
                break;
            }
    }
    return hle_surface_srv(data);
}

uint32_t hle_device_va(void)
{
    return MEM32(D3D_DEVICE_PTR_VA);
}

/* ------------------------------------------------------------------ API */

void hle_native_device_created(uint32_t pp_va)
{
    if (!hle_native_enabled())
        return;
    EnterCriticalSection(&g_hle.lock);
    if (pp_va) {
        g_hle.pp_w = MEM32(pp_va + 0);
        g_hle.pp_h = MEM32(pp_va + 4);
        /* D3DMULTISAMPLE_TYPE: x scale in bits 4-7, y scale in bits 0-3
         * (0x0011 none, 0x1121 2 samples quincunx, 0x1022 4 samples ...). */
        {
            uint32_t ms = MEM32(pp_va + 16);
            g_hle.game_msaa = ((ms >> 4) & 15u) * (ms & 15u);
            if (!g_hle.game_msaa)
                g_hle.game_msaa = 1;
        }
        fprintf(stderr, "[HLE-D3D11] CreateDevice: back buffer %ux%u fmt %08X count %u ms %08X "
                        "swap %u autodepth %u/%08X flags %08X interval %08X\n",
                g_hle.pp_w, g_hle.pp_h, MEM32(pp_va + 8), MEM32(pp_va + 12), MEM32(pp_va + 16),
                MEM32(pp_va + 20), MEM32(pp_va + 32), MEM32(pp_va + 36), MEM32(pp_va + 40),
                MEM32(pp_va + 48));
    }
    if (!g_hle.dev) {
        if (!create_device() || !hle_draw_init()) {
            disable("device/window creation failed");
        } else if (g_hle.primary) {
            xbox_nv2a_draw_disabled = 1;
            fprintf(stderr, "[HLE-D3D11] primary renderer: NV2A executor draws disabled\n");
        }
    }
    LeaveCriticalSection(&g_hle.lock);
}

/* D3DDevice_Clear(count, rects, flags, colour, z, stencil). With no rects
 * D3D8 clears the viewport, not the surface: the replay clears a small
 * picture-in-picture viewport after drawing the scene, and clearing the whole
 * target wiped the scene (the "Rewind" screen came out black). A rectangle
 * that covers the whole target takes the view clears; anything smaller is
 * drawn (hle_draw_clear_rect). */
void hle_native_clear(uint32_t count, uint32_t rects_va, uint32_t flags,
                      uint32_t color, float z, uint32_t stencil)
{
    uint32_t dev, i, n;
    HleSurface *rt, *ds, *ref;
    float c[4];
    uint32_t rc[16][4];

    if (!hle_native_enabled() || !g_hle.dev)
        return;
    EnterCriticalSection(&g_hle.lock);
    dev = hle_device_va();
    rt = (flags & XCLEAR_TARGET) ? hle_surface_for(MEM32(dev + DEV_RENDER_TARGET)) : NULL;
    ds = (flags & (XCLEAR_ZBUFFER | XCLEAR_STENCIL)) ? hle_surface_for(MEM32(dev + DEV_DEPTH_STENCIL)) : NULL;
    if (rt && !rt->rtv) rt = NULL;
    if (ds && !ds->dsv) ds = NULL;
    ref = rt ? rt : ds;
    g_hle.clears++;
    if (!ref) {
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    hle_draw_flush();
    c[0] = ((color >> 16) & 0xFF) / 255.0f;
    c[1] = ((color >> 8) & 0xFF) / 255.0f;
    c[2] = (color & 0xFF) / 255.0f;
    c[3] = ((color >> 24) & 0xFF) / 255.0f;

    /* The rectangles, in the surface's own pixels: the given ones, or the
     * viewport (device +0xEE0 x, y, w, h). */
    if (count && rects_va) {
        n = count < 16 ? count : 16;
        for (i = 0; i < n; i++) {
            rc[i][0] = MEM32(rects_va + i * 16 + 0);
            rc[i][1] = MEM32(rects_va + i * 16 + 4);
            rc[i][2] = MEM32(rects_va + i * 16 + 8);
            rc[i][3] = MEM32(rects_va + i * 16 + 12);
        }
    } else {
        n = 1;
        rc[0][0] = MEM32(dev + 0xEE0);
        rc[0][1] = MEM32(dev + 0xEE4);
        rc[0][2] = rc[0][0] + MEM32(dev + 0xEE8);
        rc[0][3] = rc[0][1] + MEM32(dev + 0xEEC);
    }
    for (i = 0; i < n; i++) {
        uint32_t x0 = rc[i][0], y0 = rc[i][1], x1 = rc[i][2], y1 = rc[i][3];
        D3D11_RECT r;
        if (x1 > ref->w) x1 = ref->w;
        if (y1 > ref->h) y1 = ref->h;
        if ((int32_t)x0 < 0) x0 = 0;
        if ((int32_t)y0 < 0) y0 = 0;
        if (x0 >= x1 || y0 >= y1)
            continue;
        if (x0 == 0 && y0 == 0 && x1 == ref->w && y1 == ref->h) {
            /* Whole target, the side bars of a widened one included. */
            if (rt && (flags & XCLEAR_TARGET) == XCLEAR_TARGET)
                ID3D11DeviceContext_ClearRenderTargetView(g_hle.ctx, rt->rtv, c);
            if (ds) {
                UINT f = 0;
                if (flags & XCLEAR_ZBUFFER) f |= D3D11_CLEAR_DEPTH;
                if (flags & XCLEAR_STENCIL) f |= D3D11_CLEAR_STENCIL;
                ID3D11DeviceContext_ClearDepthStencilView(g_hle.ctx, ds->dsv, f, z, (UINT8)stencil);
            }
            if (rt && (flags & XCLEAR_TARGET) != XCLEAR_TARGET) {
                r.left = 0; r.top = 0; r.right = (LONG)rt->tw; r.bottom = (LONG)rt->th;
                hle_draw_clear_rect(rt, NULL, &r, flags & XCLEAR_TARGET, c, z, stencil);
            }
            continue;
        }
        r.left = (LONG)(hle_map_x(ref, (float)x0) + 0.5f);
        r.right = (LONG)(hle_map_x(ref, (float)x1) + 0.5f);
        if (x0 == 0 && x1 == ref->w) {
            r.left = 0;
            r.right = (LONG)ref->tw;
        }
        r.top = (LONG)((float)y0 * g_hle.scale + 0.5f);
        r.bottom = (LONG)((float)y1 * g_hle.scale + 0.5f);
        hle_draw_clear_rect(rt, ds, &r, flags, c, z, stencil);
    }
    LeaveCriticalSection(&g_hle.lock);
}

static void write_bmp(const char *path, const uint8_t *bgra, uint32_t pitch, uint32_t w, uint32_t h)
{
    uint8_t hdr[54];
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;
    FILE *f = fopen(path, "wb");
    uint8_t *line;
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    line = (uint8_t *)calloc(1, row + pad);
    for (y = h; y-- > 0;) {
        const uint8_t *src = bgra + (size_t)y * pitch;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

static void dump_surface_tag(HleSurface *h, const char *tag);

static void dump_surface(HleSurface *h)
{
    dump_surface_tag(h, NULL);
}

/* Traced frames: the render target being left, before each SetRenderTarget. */
void hle_trace_dump_rt(void)
{
    static unsigned seq;
    char tag[64];
    HleSurface *rt;
    if (!hle_native_enabled() || !g_hle.dev || !g_hle.dump_prefix)
        return;
    EnterCriticalSection(&g_hle.lock);
    hle_draw_flush();
    rt = hle_surface_for(MEM32(hle_device_va() + DEV_RENDER_TARGET));
    if (rt && rt->rtv) {
        snprintf(tag, sizeof tag, "t%02u-%08X", seq++, rt->data);
        dump_surface_tag(rt, tag);
    }
    LeaveCriticalSection(&g_hle.lock);
}

static void dump_surface_tag(HleSurface *h, const char *tag)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[600];

    ID3D11Texture2D *tex = hle_surface_resolved(h);
    if (!tex)
        return;
    ID3D11Texture2D_GetDesc(tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &staging)))
        return;
    ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        if (tag)
            snprintf(path, sizeof path, "%s-hle%06llu-%s.bmp", g_hle.dump_prefix, (unsigned long long)g_hle.swaps, tag);
        else
            snprintf(path, sizeof path, "%s-hle%06llu.bmp", g_hle.dump_prefix, (unsigned long long)g_hle.swaps);
        write_bmp(path, (const uint8_t *)m.pData, m.RowPitch, td.Width, td.Height);
        ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

static int ensure_swapchain(uint32_t w, uint32_t h)
{
    HRESULT hr;
    if (g_hle.swap && g_hle.swap_w == w && g_hle.swap_h == h)
        return 1;
    if (g_hle.swap) {
        hr = IDXGISwapChain1_ResizeBuffers(g_hle.swap, 0, w, h, DXGI_FORMAT_UNKNOWN,
                                           DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    } else {
        DXGI_SWAP_CHAIN_DESC1 d;
        memset(&d, 0, sizeof d);
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(g_hle.factory, (IUnknown *)g_hle.dev, g_hle.hwnd, &d, NULL, NULL, &g_hle.swap);
        if (SUCCEEDED(hr))
            IDXGIFactory2_MakeWindowAssociation(g_hle.factory, g_hle.hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[HLE-D3D11] swap chain %ux%u failed 0x%08lX\n", w, h, (unsigned long)hr);
        return 0;
    }
    g_hle.swap_w = w;
    g_hle.swap_h = h;
    return 1;
}

/* Optional host presentation cap, independent of VSync. Black's animation
 * clock uses elapsed RDTSC time (0x82210 -> 0x841A0 / 0x841C0), not a fixed
 * tick per swap. Default fps=0 disables this wait. See runs/pc-settings for
 * the comparison of the game clock against wall time at different rates.
 * The menu holds the game at this boundary, outside the renderer lock. */
int guest_cpu_suspend(void);
void guest_cpu_resume(int token);
static long long s_pace_wait, s_pace_wait_total, s_pace_freq = 1;

void hle_native_pace(void)
{
    static int fps = -1;
    static int trace = -1;
    static LARGE_INTEGER freq;
    static long long next;
    static uint64_t calls;
#ifdef _WIN32
    static HANDLE timer;
#endif
    LARGE_INTEGER now;
    long long period;
    int token;

    if (trace < 0)
        trace = getenv("RECOMP_HLE_PACE_TRACE") != NULL;
    calls++;
    if (trace)
        fprintf(stderr, "[PACE-TRACE] call %llu enter\n", (unsigned long long)calls);

    hle_options_frame();                      /* F1 / Back+Start: may hold the game here */
    if (fps < 0) {
        QueryPerformanceFrequency(&freq);
        s_pace_freq = freq.QuadPart;
#ifdef _WIN32
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }
    fps = g_opt.fps;                          /* RECOMP_HLE_FPS or the options, live */
    if (fps <= 0 || !g_hle.primary) {
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu bypass fps=%d primary=%d\n",
                    (unsigned long long)calls, fps, g_hle.primary);
        return;
    }
    period = freq.QuadPart / fps;
    QueryPerformanceCounter(&now);
    if (!next || now.QuadPart - next > 2 * period) {
        next = now.QuadPart + period;           /* late by more than 2 frames: resync */
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu resync\n", (unsigned long long)calls);
        return;
    }
    if (now.QuadPart < next) {
        s_pace_wait += next - now.QuadPart;
        s_pace_wait_total += next - now.QuadPart;
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu suspend begin\n", (unsigned long long)calls);
        token = guest_cpu_suspend();
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu suspended\n", (unsigned long long)calls);
        long long left = next - now.QuadPart;
        /* Sleep until ~0.5 ms before the deadline, then spin. */
        long long sleep100ns = left * 10000000 / freq.QuadPart - 5000;
#ifdef _WIN32
        if (sleep100ns > 0 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -sleep100ns;
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                if (trace)
                    fprintf(stderr, "[PACE-TRACE] call %llu timer wait begin\n", (unsigned long long)calls);
                WaitForSingleObject(timer, 100);
                if (trace)
                    fprintf(stderr, "[PACE-TRACE] call %llu timer wait end\n", (unsigned long long)calls);
            }
        }
#else
        if (sleep100ns > 0) {
            struct timespec ts = { (time_t)(sleep100ns / 10000000), (long)(sleep100ns % 10000000) * 100 };
            nanosleep(&ts, NULL);
        }
#endif
        do {
            YieldProcessor();
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < next);
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu spin complete\n", (unsigned long long)calls);
        guest_cpu_resume(token);
        if (trace)
            fprintf(stderr, "[PACE-TRACE] call %llu resumed\n", (unsigned long long)calls);
    }
    next += period;
    if (trace)
        fprintf(stderr, "[PACE-TRACE] call %llu exit\n", (unsigned long long)calls);
}

volatile int hle_dump_request;
int hle_trace_black_wanted(void);
void hle_trace_arm_next(void);

/* Every 30 swaps with RECOMP_HLE_TRACE=black: sample a 16x12 grid in the
 * middle half of the presented target; mostly black while the frame had
 * more than 100 draws arms a trace of the next frame. */
static void black_probe(HleSurface *rt)
{
    static uint64_t last_draws;
    uint64_t draws, skipped, tris;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned black = 0, x, y;

    hle_draw_stats(&draws, &skipped, &tris);
    if (g_hle.swaps % 30 || !hle_trace_black_wanted()) {
        last_draws = draws;
        return;
    }
    if (draws - last_draws < 100) {
        last_draws = draws;
        return;
    }
    last_draws = draws;
    if (rt->samples > 1)
        return;   /* diagnostics: single-sample targets only */
    ID3D11Texture2D_GetDesc(rt->tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < 12; y++)
            for (x = 0; x < 16; x++) {
                uint32_t px = td.Width / 4 + x * (td.Width / 2) / 16;
                uint32_t py = td.Height / 4 + y * (td.Height / 2) / 12;
                uint32_t v = ((const uint32_t *)((const uint8_t *)m.pData + (size_t)py * m.RowPitch))[px];
                if (((v >> 16) & 0xFF) < 8 && ((v >> 8) & 0xFF) < 8 && (v & 0xFF) < 8)
                    black++;
            }
        ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    if (black >= 180)
        hle_trace_arm_next();
}

static void capture_backbuffer(ID3D11Texture2D *bb, UINT w, UINT h, const char *tag)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[512];
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        snprintf(path, sizeof path, "%s-options-%s.bmp", g_hle.dump_prefix, tag);
        write_bmp(path, (const uint8_t *)m.pData, m.RowPitch, w, h);
        ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)st, 0);
        fprintf(stderr, "[OPTIONS] captured %s (%ux%u)\n", path, w, h);
    }
    ID3D11Texture2D_Release(st);
}

/* rt onto the window: back buffer the size of the client area, the picture
 * letterboxed (hle_native_present.c), the options menu over it. */
static long long s_present_ticks;
static int s_present_token;

static void present_rt(HleSurface *rt)
{
    extern int hle_present_to(HleSurface *source, ID3D11RenderTargetView *target, UINT bw, UINT bh, int fxaa);
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    UINT cw, ch;
    hle_draw_state_invalidate();              /* present and the overlay bind their own state */
    hle_options_apply_display(rt->tw, rt->th);
    if (!window_client_size(&cw, &ch))
        return;                               /* minimised */
    if (!ensure_swapchain(cw, ch) ||
        FAILED(IDXGISwapChain1_GetBuffer(g_hle.swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_hle.dev, (ID3D11Resource *)bb, NULL, &rtv))) {
        if (hle_present_to(rt, rtv, cw, ch, g_opt.aa) < 0)
            fprintf(stderr, "[HLE-PRESENT] frame failed; preserving previous display\n");
        hle_options_draw_overlay(rtv, cw, ch);
        hle_options_draw_button(rtv);
        ID3D11RenderTargetView_Release(rtv);
    }
    {
        const char *tag = hle_options_capture_tag();
        if (tag && g_hle.dump_prefix)
            capture_backbuffer(bb, cw, ch, tag);
    }
    ID3D11Texture2D_Release(bb);
    {
        /* VSync on: one frame in the queue (DXGI's default is 3). The game
         * makes 60 frames a second, so on a 60 Hz monitor the queue fills
         * slowly with the clocks' drift and would then hold the picture up
         * to three refreshes behind the controls. Off: the default again. */
        static int latency_for = -1;
        if (latency_for != (g_opt.vsync != 0)) {
            IDXGIDevice1 *d1 = NULL;
            latency_for = g_opt.vsync != 0;
            if (SUCCEEDED(ID3D11Device_QueryInterface(g_hle.dev, &IID_IDXGIDevice1, (void **)&d1))) {
                IDXGIDevice1_SetMaximumFrameLatency(d1, latency_for ? 1 : 3);
                IDXGIDevice1_Release(d1);
            }
        }
    }
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        /* VSync: Present waits for the monitor. The guest CPU is let go
         * first so the game's other threads run meanwhile (as they would
         * while the Xbox waits for its flip); it is taken back only after
         * the renderer lock is released (hle_native_swap), never while
         * holding it -- a guest thread waiting for that lock holds the CPU. */
        if (g_opt.vsync && !s_present_token)
            s_present_token = guest_cpu_suspend();
        if (hle_present_trace_enabled())
            fprintf(stderr, "[PRESENT-TRACE] DXGI begin swap %llu interval %d\n",
                    (unsigned long long)g_hle.swaps, g_opt.vsync ? 1 : 0);
        IDXGISwapChain1_Present(g_hle.swap, g_opt.vsync ? 1 : 0, 0);
        QueryPerformanceCounter(&b);
        s_present_ticks += b.QuadPart - a.QuadPart;
        if (hle_present_trace_enabled())
            fprintf(stderr, "[PRESENT-TRACE] DXGI end swap %llu\n",
                    (unsigned long long)g_hle.swaps);
    }
    debug_messages();
}

/* Host-only settings presentation: no XBE, guest memory or device model. */
static int s_settings_only;

static void present_settings(void)
{
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    UINT w, h;
    static const float background[4] = {0.025f, 0.035f, 0.055f, 1.0f};
    hle_draw_state_invalidate();
    hle_options_apply_display(1280, 720);
    if (!window_client_size(&w, &h) || !ensure_swapchain(w, h)) return;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_hle.swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return;
    if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_hle.dev, (ID3D11Resource *)bb, NULL, &rtv))) {
        ID3D11DeviceContext_ClearRenderTargetView(g_hle.ctx, rtv, background);
        hle_options_draw_overlay(rtv, w, h);
        ID3D11RenderTargetView_Release(rtv);
    }
    const char *tag = hle_options_capture_tag();
    if (tag && g_hle.dump_prefix) capture_backbuffer(bb, w, h, tag);
    ID3D11Texture2D_Release(bb);
    IDXGISwapChain1_Present(g_hle.swap, g_opt.vsync ? 1 : 0, 0);
}

int hle_native_settings(void)
{
    if (!hle_native_enabled()) return 1;
    EnterCriticalSection(&g_hle.lock);
    int ready = create_device();
    LeaveCriticalSection(&g_hle.lock);
    if (!ready) return 1;
    s_settings_only = 1;
    hle_options_standalone();
    s_settings_only = 0;
    return 0;
}

/* The options menu, while the game waits: the last frame again, with the
 * menu as it is now. */
void hle_native_represent(void)
{
    int i;
    if (!hle_native_enabled() || !g_hle.dev)
        return;
    EnterCriticalSection(&g_hle.lock);
    if (s_settings_only) {
        present_settings();
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    for (i = 0; i < MAX_SURFACES; i++) {
        HleSurface *rt = &g_hle.surf[i];
        if (rt->tex && rt->rtv && !rt->depth && g_hle.last_present && rt->data == g_hle.last_present) {
            present_rt(rt);
            break;
        }
    }
    LeaveCriticalSection(&g_hle.lock);
}

void hle_stall_tick(void);

/* RECOMP_HLE_PROFILE=1: every 600 swaps, what a frame cost on average and
 * at worst -- the time between swaps, the draws (backend work for the
 * game's draw calls), Present, the frame limiter's wait, shader/texture
 * builds -- plus the CPU used by this (the game's render) thread and by the
 * whole process, in % of one core. Disable the frame limiter and VSync
 * when comparing throughput; capped runs measure the available headroom. */
static uint64_t profile_cpu_ticks(int process)
{
#ifdef _WIN32
    FILETIME a, b, k, u;
    if (process) {
        if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u)) return 0;
    } else {
        if (!GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u)) return 0;
    }
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) +
           ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
#else
    struct timespec t;
    if (clock_gettime(process ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &t))
        return 0;
    return (uint64_t)t.tv_sec * 10000000u + (uint64_t)t.tv_nsec / 100u;
#endif
}

static uint64_t profile_wall_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1000.0 / (double)freq.QuadPart);
}

static void profile_frame(double ms, double draw_ms, double present_ms, double build_ms)
{
    static int on = -1;
    static unsigned n, slow;
    static double sum_ms, sum_draw, sum_present, sum_build, sum_wait, max_ms, max_draw, worst[4];
    static long long last_wait;
    static uint64_t first_swap, draws0;
    static uint64_t thread0, process0, wall0;
    double wait_ms;
    if (on < 0) {
        const char *e = getenv("RECOMP_HLE_PROFILE");
        on = e && *e && *e != '0';
    }
    if (!on || ms <= 0.0)
        return;
    wait_ms = (double)(s_pace_wait_total - last_wait) * 1000.0 / (double)s_pace_freq;
    last_wait = s_pace_wait_total;
    if (!n) {
        uint64_t skipped, tris;
        static int ab = -1;
        extern int hle_ab_off, hle_ab_new_off;
        if (ab < 0) {
            const char *e = getenv("RECOMP_HLE_AB");
            ab = e && *e && *e != '0' ? (atoi(e) == 2 ? 2 : 1) : 0;
        }
        if (ab == 2)
            hle_ab_new_off = !hle_ab_new_off;
        else if (ab)
            hle_ab_off = !hle_ab_off;
        first_swap = g_hle.swaps;
        hle_draw_stats(&draws0, &skipped, &tris);
        thread0 = profile_cpu_ticks(0);
        process0 = profile_cpu_ticks(1);
        wall0 = profile_wall_ms();
        sum_ms = sum_draw = sum_present = sum_build = sum_wait = max_ms = max_draw = 0.0;
        slow = 0;
        n = 1;
        return;                               /* the first interval starts here */
    }
    sum_ms += ms; sum_draw += draw_ms; sum_present += present_ms; sum_build += build_ms; sum_wait += wait_ms;
    if (ms > 20.0) slow++;
    if (draw_ms > max_draw) max_draw = draw_ms;
    if (ms > max_ms) {
        max_ms = ms;
        worst[0] = draw_ms; worst[1] = present_ms; worst[2] = build_ms; worst[3] = wait_ms;
    }
    if (n++ == 600) {
        uint64_t th, pr, wall = profile_wall_ms() - wall0;
        uint64_t draws, skipped, tris;
        double f = (double)(n - 1);
        hle_draw_stats(&draws, &skipped, &tris);
        th = profile_cpu_ticks(0);
        pr = profile_cpu_ticks(1);
        if (!wall) wall = 1;
        extern int hle_ab_off, hle_ab_new_off;
        fprintf(stderr, "[HLE-PROF]%s%s swaps %llu-%llu: frame %.2f ms (max %.1f, >20 ms %u), %.0f draws/frame, "
                "draw %.3f ms (max %.2f), present %.3f, limiter wait %.2f, builds %.3f; "
                "render thread %.0f%%, process %.0f%% of a core; worst frame: draw %.2f present %.2f builds %.2f wait %.2f\n",
                hle_ab_off ? " [opt off]" : "", hle_ab_new_off ? " [new off]" : "", (unsigned long long)first_swap, (unsigned long long)g_hle.swaps, sum_ms / f, max_ms, slow,
                (double)(draws - draws0) / f, sum_draw / f, max_draw, sum_present / f, sum_wait / f, sum_build / f,
                (double)(th - thread0) / 100.0 / (double)wall, (double)(pr - process0) / 100.0 / (double)wall,
                worst[0], worst[1], worst[2], worst[3]);
        n = 0;
    }
}

void hle_native_swap(uint32_t flags)
{
    HleSurface *rt;
    DWORD now;

    (void)flags;
    if (!hle_native_enabled() || !g_hle.dev)
        return;
    EnterCriticalSection(&g_hle.lock);
    hle_draw_flush();
    g_hle.swaps++;
    hle_stall_tick();
    rt = hle_surface_for(MEM32(hle_device_va() + DEV_RENDER_TARGET));
    if (rt)
        g_hle.last_present = rt->data;
    if (rt && rt->rtv)
        black_probe(rt);
    if (rt && rt->rtv) {
        if (g_hle.dump_prefix && ((g_hle.dump_every && g_hle.swaps % g_hle.dump_every == 0) || hle_dump_request)) {
            hle_dump_request = 0;
            dump_surface(rt);
        }
        if (hle_present_trace_enabled())
            fprintf(stderr, "[PRESENT-TRACE] surface begin swap %llu\n",
                    (unsigned long long)g_hle.swaps);
        present_rt(rt);
        if (hle_present_trace_enabled())
            fprintf(stderr, "[PRESENT-TRACE] surface end swap %llu\n",
                    (unsigned long long)g_hle.swaps);
        if (g_hle.front_wanted)
            front_update(rt);
    }
    {
        /* A frame over 25 ms is a visible hitch: say what it spent on
         * shader compiles and texture decodes (the rest is the game). */
        static LARGE_INTEGER last, freq;
        static unsigned reported;
        LARGE_INTEGER c;
        double vs_ms, tex_ms, ms;
        unsigned vs_n, tex_n;
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&c);
        ms = last.QuadPart ? (double)(c.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
        last = c;
        hle_draw_frame_costs(&vs_ms, &vs_n, &tex_ms, &tex_n);
        {
            double draw_ms = hle_draw_frame_draw_ms();
            double present_ms = (double)s_present_ticks * 1000.0 / (double)freq.QuadPart;
            s_present_ticks = 0;
        if (ms > 25.0 && reported < 2000) {
            reported++;
            fprintf(stderr, "[HLE-SPIKE] swap %llu: %.1f ms (vertex shaders %u: %.1f ms, textures %u: %.1f ms)\n",
                    (unsigned long long)g_hle.swaps, ms, vs_n, vs_ms, tex_n, tex_ms);
            fprintf(stderr, "[HLE-SPIKE]   draws %.1f ms, present %.1f ms\n", draw_ms, present_ms);
        }
            profile_frame(ms, draw_ms, present_ms, vs_ms + tex_ms);
        }
    }
    now = GetTickCount();
    if (now - g_hle.title_tick >= 1000) {
        wchar_t t[160];
        if (g_hle.title_tick) {
            uint64_t draws, skipped, tris;
            hle_draw_stats(&draws, &skipped, &tris);
            double fps = (g_hle.swaps - g_hle.title_swaps) * 1000.0 / (now - g_hle.title_tick);
            _snwprintf(t, 159, L"BLACK - %ls%ls | %ux%u | %.1f fps | draws %llu (skip %llu)",
                       hle_gpu_api() == HLE_API_VULKAN ? L"Vulkan" : L"DirectX 11",
                       g_hle.primary ? L"" : L" (sombra)",
                       rt ? rt->tw : 0, rt ? rt->th : 0, fps,
                       (unsigned long long)(draws - g_hle.title_draws), (unsigned long long)skipped);
            if (++g_hle.fps_reports % 5 == 0)
                fprintf(stderr, "[HLE-D3D11] swap %llu: %.1f fps, idle %.0f%%, %llu draws/s, %llu skipped total\n",
                        (unsigned long long)g_hle.swaps, fps,
                        100.0 * (double)s_pace_wait / (double)s_pace_freq * 1000.0 / (double)(now - g_hle.title_tick),
                        (unsigned long long)(draws - g_hle.title_draws), (unsigned long long)skipped);
            g_hle.title_draws = draws;
            s_pace_wait = 0;
            t[159] = 0;
            window_set_title(t);
        }
        g_hle.title_tick = now;
        g_hle.title_swaps = g_hle.swaps;
    }
    LeaveCriticalSection(&g_hle.lock);
    if (s_present_token) {
        int token = s_present_token;
        s_present_token = 0;
        guest_cpu_resume(token);
    }
    /* Options and optional host pacing must run outside the renderer lock. */
    hle_native_pace();
}

/* An existing colour surface for guest memory at data, or NULL. */
static HleSurface *surface_find(uint32_t data)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++) {
        HleSurface *c = &g_hle.surf[i];
        if (c->tex && !c->depth && c->data == data)
            return c;
    }
    return NULL;
}

/* D3DDevice_CopyRects. The replay grabs the back buffer into a texture this
 * way (game 0x1934F0); the library's blit was for the NV2A, so the texture's
 * guest memory never received the picture. Copying between the native
 * surfaces keeps it on the GPU: the destination becomes a native surface,
 * which tex_get then samples by its data address. A source the backend never
 * rendered is left alone (its memory is what the CPU wrote). */
void hle_native_copy_rects(uint32_t src_obj, uint32_t rects, uint32_t count,
                           uint32_t dst_obj, uint32_t points)
{
    HleSurface *src, *dst;
    uint32_t i, n;

    if (!hle_native_enabled() || !g_hle.dev || !src_obj || !dst_obj)
        return;
    EnterCriticalSection(&g_hle.lock);
    if (!surface_find(MEM32(src_obj + 4))) {
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    hle_draw_flush();
    dst = hle_surface_for(dst_obj);
    src = surface_find(MEM32(src_obj + 4));       /* hle_surface_for may evict */
    if (!dst || !src || dst->depth || dst == src) {
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    g_hle.copies++;
    n = (rects && count) ? count : 1;
    for (i = 0; i < n; i++) {
        uint32_t x0 = 0, y0 = 0, x1 = src->w, y1 = src->h, dx, dy;
        UINT ox, oy;
        D3D11_BOX box;
        if (rects && count) {
            x0 = MEM32(rects + 16 * i); y0 = MEM32(rects + 16 * i + 4);
            x1 = MEM32(rects + 16 * i + 8); y1 = MEM32(rects + 16 * i + 12);
        }
        if (points) {
            dx = MEM32(points + 8 * i); dy = MEM32(points + 8 * i + 4);
        } else {
            dx = x0; dy = y0;
        }
        if (x1 > src->w) x1 = src->w;
        if (y1 > src->h) y1 = src->h;
        if (x0 >= x1 || y0 >= y1)
            continue;
        if (x0 == 0 && y0 == 0 && x1 == src->w && y1 == src->h && dx == 0 && dy == 0 &&
            src->tw == dst->tw && src->th == dst->th && src->samples == dst->samples) {
            ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)dst->tex, (ID3D11Resource *)src->tex);
            continue;
        }
        /* Partial: native coordinates through each surface's mapping (the
         * whole width of a widened surface when the rect spans it). */
        box.left = (UINT)(hle_map_x(src, (float)x0) + 0.5f);
        box.right = (UINT)(hle_map_x(src, (float)x1) + 0.5f);
        ox = (UINT)(hle_map_x(dst, (float)dx) + 0.5f);
        if (x0 == 0 && x1 == src->w && dx == 0) {
            box.left = 0;
            box.right = src->tw;
            ox = 0;
        }
        box.top = (UINT)((float)y0 * g_hle.scale + 0.5f);
        box.bottom = (UINT)((float)y1 * g_hle.scale + 0.5f);
        box.front = 0;
        box.back = 1;
        oy = (UINT)((float)dy * g_hle.scale + 0.5f);
        if (ox >= dst->tw || oy >= dst->th)
            continue;
        if (ox + (box.right - box.left) > dst->tw) box.right = box.left + (dst->tw - ox);
        if (oy + (box.bottom - box.top) > dst->th) box.bottom = box.top + (dst->th - oy);
        if (box.right > box.left && box.bottom > box.top) {
            if (src->samples > 1 || dst->samples > 1)
                msaa_copy(dst, src, &box, ox, oy);
            else
                ID3D11DeviceContext_CopySubresourceRegion(g_hle.ctx, (ID3D11Resource *)dst->tex, 0, ox, oy, 0,
                                                          (ID3D11Resource *)src->tex, 0, &box);
        }
    }
    if (g_hle.log || g_hle.copies <= 4)
        fprintf(stderr, "[HLE-D3D11] CopyRects %08X (%ux%u) -> %08X (%ux%u), %u rect(s)\n",
                src->data, src->w, src->h, dst->data, dst->w, dst->h, n);
    LeaveCriticalSection(&g_hle.lock);
}

/* D3DSurface_LockRect, after the library filled the D3DLOCKED_RECT
 * (+0 pitch, +4 bits): a surface the backend renders -- or the front buffer,
 * which here only ever stands for the presented target -- gets its native
 * pixels (32-bit, nearest sample per logical pixel) written to guest memory
 * so the CPU reads the picture (game 0x192EF0 copies the front buffer). */
void hle_native_lock_rect(uint32_t obj, uint32_t locked)
{
    HleSurface *s;
    uint32_t data, pitch, bits, fmt, x, y, dev, w, h;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE m;

    if (!hle_native_enabled() || !g_hle.dev || !obj || !locked)
        return;
    data = MEM32(obj + 4);
    EnterCriticalSection(&g_hle.lock);
    s = surface_find(data);
    dev = hle_device_va();
    if (!s && dev && MEM32(dev + 0x1A18) && MEM32(MEM32(dev + 0x1A18) + 4) == data)
        s = surface_find(g_hle.last_present);
    fmt = (MEM32(obj + 12) >> 8) & 0xFF;
    pitch = MEM32(locked);
    bits = MEM32(locked + 4);
    w = MEM32(obj + 16) ? (MEM32(obj + 16) & 0xFFF) + 1 : (s ? s->w : 0);
    h = MEM32(obj + 16) ? ((MEM32(obj + 16) >> 12) & 0xFFF) + 1 : (s ? s->h : 0);
    if (!s || !bits || !w || !h || pitch < w * 4) {
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    hle_draw_flush();
    ID3D11Texture2D *tex = hle_surface_resolved(s);
    if (!tex) {
        LeaveCriticalSection(&g_hle.lock);
        return;
    }
    ID3D11Texture2D_GetDesc(tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &staging))) {
        ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < h; y++) {
                uint32_t sy = (uint32_t)(((float)((uint64_t)y * s->h / h) + 0.5f) * g_hle.scale);
                if (sy >= s->th) sy = s->th - 1;
                const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)sy * m.RowPitch);
                for (x = 0; x < w; x++) {
                    uint32_t lx = (uint32_t)((uint64_t)x * s->w / w);
                    uint32_t sx = (uint32_t)hle_map_x(s, (float)lx + 0.5f);
                    if (sx >= s->tw) sx = s->tw - 1;
                    MEM32(bits + y * pitch + 4 * x) = row[sx];
                }
            }
            ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)staging, 0);
            g_hle.readbacks++;
            if (g_hle.log || g_hle.readbacks <= 4)
                fprintf(stderr, "[HLE-D3D11] LockRect %08X fmt %02X %ux%u pitch %u: native picture written back\n",
                        data, fmt, w, h, pitch);
        }
        ID3D11Texture2D_Release(staging);
    }
    LeaveCriticalSection(&g_hle.lock);
}
