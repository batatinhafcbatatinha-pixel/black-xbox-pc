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
#include <windows.h>
#include "black_gpu.h"

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
