/*
 * Cutscene skip: A on host pad 0, or F10 on the keyboard.
 *
 * BLACK plays its cutscenes as pre-rendered .xmv movies. Every one of them, front-end intro/attract FMVs and the
 * in-game story movies, is stepped by the same function (0x000C4570, "decode the next frames of this movie"), and
 * that function sets byte 0x62 ("end of stream") or 0x64 ("end of file") on the movie object when the XMV decoder
 * reports the end. The front-end page (0x00209D40, state 0x1E) waits for 0x62 and the in-game movie loop (0x000D370F)
 * waits for 0x64; both then run their normal teardown (stop XMV, go to the next page/mission).
 *
 * So the skip is simply "pretend the stream ended": native_d3d/black_native_copies.c copies 0x000C4570 with a check
 * for black_cutscene_skip() at the top, and raises both flags. Nothing else is bypassed -- the game releases the
 * decoder and advances exactly as at the end of the movie. The original game has no skip on these movies; this port
 * adds one.
 *
 * black_cutscene_skip() samples the host pad directly (usb_gamepad_report, the same source as the XInput
 * replacement) instead of the guest's pad state, so it also works when the game is not polling input (the in-game
 * movie loop does not). black_skip_hides_a() is used by black_input.c to keep the A press that triggered the skip
 * out of the guest until it is released, so the same press cannot fire a shot or pick a menu entry afterwards.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "black_native.h"

int usb_gamepad_report(int pad, uint8_t *out, int max);

#define PAD_BUTTON_A 4           /* XINPUT_GAMEPAD.bAnalogButtons[0] = A (report byte 4, 0xFF when pressed) */
#define SKIP_KEY     VK_F10
#define REGAP_MS     250         /* no movie step for this long: forget the previous sample */

static DWORD s_last_tick;
static int s_prev_a, s_prev_key;
static int s_hide_a;

/* The guest's XInputGetState hides A while this returns 1: the press was spent on a skip. */
int black_skip_hides_a(void)
{
    return s_hide_a;
}

/* 1 on a fresh A press (pad 0) or F10 press. Called once per movie frame from the 0x000C4570 native copy. */
int black_cutscene_skip(void)
{
    uint8_t rep[20];
    DWORD now = GetTickCount();
    int a, key, edge;

    /* BLACK_SKIP_MOVIES=1 (test runs): every movie ends at its first step. */
    static int s_auto = -1;
    if (s_auto < 0) {
        const char *e = getenv("BLACK_SKIP_MOVIES");
        s_auto = e && *e && *e != '0';
    }
    if (s_auto)
        return 1;
    usb_gamepad_report(0, rep, 20);
    a = rep[PAD_BUTTON_A] != 0;
    key = (GetAsyncKeyState(SKIP_KEY) & 0x8000) != 0;
    if (s_hide_a && !a)
        s_hide_a = 0;
    /* Movies are stepped every frame, so a real gap means a new movie: the first sample must not look like a press. */
    edge = s_last_tick && now - s_last_tick <= REGAP_MS
        && ((a && !s_prev_a) || (key && !s_prev_key));
    s_prev_a = a;
    s_prev_key = key;
    s_last_tick = now ? now : 1;
    if (edge && a)
        s_hide_a = 1;
    return edge;
}
