/*
 * Mouse for Black's modern keyboard-and-mouse controls (project/src/native_d3d/black_controls.c reads it).
 *
 * Raw input on the game window gives relative motion with no acceleration and no dependence on where the cursor
 * is, so looking around never stops at a screen edge. The window procedure (hle_d3d11.c) passes every message
 * here first; WM_INPUT is accumulated and consumed by BlackGpuMouseRead() once per controller poll.
 *
 * In gameplay the controls ask for capture: the cursor is confined to the client area and hidden. On the game's
 * menus, in the PC settings and whenever the window loses the focus it is released, so the mouse works the
 * options menu and the desktop normally. Legacy mouse messages are not suppressed (the options menu uses them).
 */
#include "black_gpu.h"
#ifdef _WIN32
#include <windows.h>

static volatile LONG s_dx, s_dy, s_wheel, s_buttons;
static volatile LONG s_capture_wanted, s_captured;
static HWND s_hwnd;

static void apply_capture(HWND h)
{
    int want = s_capture_wanted && h && GetForegroundWindow() == h;
    if (want) {
        RECT r;
        POINT a, b;
        GetClientRect(h, &r);
        a.x = r.left; a.y = r.top; b.x = r.right; b.y = r.bottom;
        ClientToScreen(h, &a);
        ClientToScreen(h, &b);
        r.left = a.x; r.top = a.y; r.right = b.x; r.bottom = b.y;
        ClipCursor(&r);
    } else if (s_captured) {
        ClipCursor(NULL);
    }
    InterlockedExchange(&s_captured, want);
}

/* 1 when the message was the mouse's own (still passed on to DefWindowProc by the caller). */
int black_mouse_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (!s_hwnd) {
        RAWINPUTDEVICE rid;
        s_hwnd = h;
        rid.usUsagePage = 0x01;           /* generic desktop */
        rid.usUsage = 0x02;               /* mouse */
        rid.dwFlags = 0;                  /* foreground only; legacy messages kept */
        rid.hwndTarget = h;
        RegisterRawInputDevices(&rid, 1, sizeof rid);
    }
    switch (m) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            USHORT f = ri.data.mouse.usButtonFlags;
            LONG b = s_buttons;
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&s_dx, ri.data.mouse.lLastX);
                InterlockedExchangeAdd(&s_dy, ri.data.mouse.lLastY);
            }
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) b |= BLACK_MOUSE_LEFT;
            if (f & RI_MOUSE_LEFT_BUTTON_UP) b &= ~BLACK_MOUSE_LEFT;
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) b |= BLACK_MOUSE_RIGHT;
            if (f & RI_MOUSE_RIGHT_BUTTON_UP) b &= ~BLACK_MOUSE_RIGHT;
            if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) b |= BLACK_MOUSE_MIDDLE;
            if (f & RI_MOUSE_MIDDLE_BUTTON_UP) b &= ~BLACK_MOUSE_MIDDLE;
            if (f & RI_MOUSE_BUTTON_4_DOWN) b |= BLACK_MOUSE_X1;
            if (f & RI_MOUSE_BUTTON_4_UP) b &= ~BLACK_MOUSE_X1;
            if (f & RI_MOUSE_BUTTON_5_DOWN) b |= BLACK_MOUSE_X2;
            if (f & RI_MOUSE_BUTTON_5_UP) b &= ~BLACK_MOUSE_X2;
            InterlockedExchange(&s_buttons, b);
            if (f & RI_MOUSE_WHEEL)
                InterlockedExchangeAdd(&s_wheel, (SHORT)ri.data.mouse.usButtonData);
        }
        return 1;
    }
    case WM_KILLFOCUS:
        InterlockedExchange(&s_buttons, 0);
        ClipCursor(NULL);
        InterlockedExchange(&s_captured, 0);
        return 1;
    case WM_ACTIVATE: case WM_SIZE: case WM_MOVE:
        apply_capture(h);
        return 1;
    case WM_SETCURSOR:
        if (s_captured && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return 2;                     /* handled: the caller returns TRUE */
        }
        break;
    }
    return 0;
}

BLACK_GPU_API void BlackGpuMouseRead(int *dx, int *dy, unsigned *buttons, int *wheel)
{
    *dx = (int)InterlockedExchange(&s_dx, 0);
    *dy = (int)InterlockedExchange(&s_dy, 0);
    *wheel = (int)InterlockedExchange(&s_wheel, 0);
    *buttons = s_captured || !s_capture_wanted ? (unsigned)s_buttons : 0u;
}

BLACK_GPU_API void BlackGpuMouseCapture(int on)
{
    int focused = s_hwnd && GetForegroundWindow() == s_hwnd;
    if (on != s_capture_wanted || (on && focused != s_captured)) {
        InterlockedExchange(&s_capture_wanted, on);
        apply_capture(s_hwnd);
    }
}
#else
/* Linux/macOS: the renderer's SDL window. Relative mouse mode in gameplay (the cursor hidden and held, motion as
 * deltas); the window thread applies a change of mode when the user event below wakes it. */
#include <SDL2/SDL.h>

static SDL_atomic_t s_dx, s_dy, s_wheel, s_buttons, s_capture_wanted, s_captured;
static Uint32 s_wake_event;

void black_mouse_sdl_event(const SDL_Event *e)
{
    int want = SDL_AtomicGet(&s_capture_wanted);
    if (want != SDL_AtomicGet(&s_captured)) {
        SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
        SDL_AtomicSet(&s_captured, want);
    }
    switch (e->type) {
    case SDL_MOUSEMOTION:
        if (SDL_AtomicGet(&s_captured)) {
            SDL_AtomicAdd(&s_dx, e->motion.xrel);
            SDL_AtomicAdd(&s_dy, e->motion.yrel);
        }
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        unsigned bit = e->button.button == SDL_BUTTON_LEFT ? BLACK_MOUSE_LEFT
                     : e->button.button == SDL_BUTTON_RIGHT ? BLACK_MOUSE_RIGHT
                     : e->button.button == SDL_BUTTON_MIDDLE ? BLACK_MOUSE_MIDDLE
                     : e->button.button == SDL_BUTTON_X1 ? BLACK_MOUSE_X1
                     : e->button.button == SDL_BUTTON_X2 ? BLACK_MOUSE_X2 : 0u;
        int b = SDL_AtomicGet(&s_buttons);
        SDL_AtomicSet(&s_buttons, e->type == SDL_MOUSEBUTTONDOWN ? (int)(b | bit) : (int)(b & ~bit));
        break;
    }
    case SDL_MOUSEWHEEL:
        SDL_AtomicAdd(&s_wheel, e->wheel.y * 120);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            SDL_AtomicSet(&s_buttons, 0);
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_AtomicSet(&s_captured, 0);
        }
        break;
    }
}

BLACK_GPU_API void BlackGpuMouseRead(int *dx, int *dy, unsigned *buttons, int *wheel)
{
    *dx = SDL_AtomicSet(&s_dx, 0);
    *dy = SDL_AtomicSet(&s_dy, 0);
    *wheel = SDL_AtomicSet(&s_wheel, 0);
    *buttons = SDL_AtomicGet(&s_captured) || !SDL_AtomicGet(&s_capture_wanted) ? (unsigned)SDL_AtomicGet(&s_buttons) : 0u;
}

BLACK_GPU_API void BlackGpuMouseCapture(int on)
{
    if (SDL_AtomicSet(&s_capture_wanted, on) != on && SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_Event e;
        if (!s_wake_event)
            s_wake_event = SDL_RegisterEvents(1);
        SDL_zero(e);
        e.type = s_wake_event;
        SDL_PushEvent(&e);            /* the window thread applies the mode */
    }
}
#endif
