// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/* In-game options (F1 / Back+Start): settings kept in black-pc.ini next to
 * the executable. See hle_options.c. */
#ifndef HLE_OPTIONS_H
#define HLE_OPTIONS_H

#include "hle_d3d11_internal.h"
#include <stdint.h>

typedef struct HleOptions {
    int display;      /* 0 window, 1 borderless fullscreen, 2 exclusive fullscreen */
    int monitor;      /* index into the monitor list (0 = primary) */
    int refresh;      /* Hz for exclusive fullscreen, 0 = the monitor's current */
    int vsync;        /* present on vblank */
    int fps;          /* frame limiter: 60 (the title's), 30, 0 = off */
    int scale;        /* internal resolution preset, k_res[] (0 = the monitor's) (restart) */
    int aspect;       /* 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9      (restart) */
    int aa;           /* 0 off, 1 FXAA */
    int aniso;        /* 0 the game's filtering, else forced anisotropy 2..16 */
    int api;          /* 0 Direct3D 11, 1 Vulkan (DXVK)                (restart) */
    int adapter;      /* -1 system default, else DXGI adapter index  (restart) */
    int language;     /* EEPROM language: 1 English, 5 Spanish        (restart) */
    int volume;       /* 0..100 */
    int ui_language;  /* 0 Portuguese, 1 English (applies immediately) */
} HleOptions;

extern HleOptions g_opt;

/* WinMain, before anything reads the environment: black-pc.ini supplies
 * RECOMP_HLE_SCALE/ASPECT/FPS/ADAPTER and RECOMP_LANGUAGE unless already set. */
void hle_options_init(void);
/* Window thread, for every key message. Returns 1 when the menu took it. */
int hle_options_key(UINT msg, WPARAM vk, LPARAM lp);
/* Window thread, mouse messages (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_RBUTTONDOWN,
 * WM_MOUSEWHEEL; x, y in client pixels of a cw x ch client area). Returns 1
 * when the menu took it (it is open). */
int hle_options_mouse(UINT msg, int x, int y, int wheel, int cw, int ch);
/* Window thread, WM_SETCURSOR over the client area: 1 = hide the cursor. */
int hle_options_hide_cursor(void);
/* Game thread, after each Swap: opens the menu and holds the game there. */
void hle_options_frame(void);
/* Host-only startup menu: bypass the in-game boot guard. */
void hle_options_standalone(void);
/* Present path: apply a pending display-mode change (window/fullscreen,
 * monitor, refresh) and draw the menu over the back buffer. */
void hle_options_apply_display(uint32_t picture_w, uint32_t picture_h);
void hle_options_draw_overlay(ID3D11RenderTargetView *rtv, UINT w, UINT h);
/* On the game's main, options and pause menus: the "(BACK) settings"
 * button in the game's own button-hint style. */
void hle_options_draw_button(ID3D11RenderTargetView *rtv);
/* RECOMP_OPTIONS_AUTOTEST: a back-buffer capture tag wanted at this present
 * (consumed), or NULL. */
const char *hle_options_capture_tag(void);
/* Open the options screen at the next Swap (the game asked for it). */
void hle_options_request(const char *why);
/* Initial window placement for the window thread. */
#ifdef _WIN32
void hle_options_initial_window(DWORD *style, RECT *r);
#else
void hle_options_initial_window_sdl(int *x, int *y, int *w, int *h, unsigned *sdl_flags);
#endif
/* Internal scale that makes a 480-line game as tall as the chosen monitor. */
float hle_options_native_scale(void);

#endif
