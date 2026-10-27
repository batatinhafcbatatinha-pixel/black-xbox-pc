/*
 * Controller input in C: the XDK XInput/XPP API without the USB stack (Street 1 hle_input.c).
 *
 * XAPI's XPP layer drives the console's OHCI USB host controller (0xFED00000) and an XID driver. These functions
 * replace the API itself, so XInitDevices never starts the USB driver and nothing touches the controller. Pad state
 * comes from usb_gamepad_report() (xboxrecomp/src/usb/usb_gamepad.c): host XInput pads, the keyboard on pad 0 and
 * the test scripts (RECOMP_PAD_SCRIPT / RECOMP_PAD_LIVE). Its 20-byte report is the XID report: bytes 2..19 are
 * XINPUT_GAMEPAD. Port 0 is always connected; ports 1-3 follow host pads 1-3. Memory units: none.
 *
 *   0x27D058 XInitDevices(count, types)           0x27DD29 XGetDeviceChanges(type, ins, rem)
 *   0x27DD96 XInputOpen(type, port, slot, poll)    0x27DDEC XInputClose(h)
 *   0x27DDF8 XInputGetState(h, state)              0x27DE6B XInputSetState(h, feedback)
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "black_native.h"

int usb_gamepad_report(int pad, uint8_t *out, int max);
void xbox_InputInit(void);
BOOL xbox_InputIsConnected(DWORD port);
typedef struct { WORD left, right; } HostVibration;
DWORD xbox_InputSetState(DWORD port, const void *vibration);
int black_skip_hides_a(void);            /* black_skip.c: A press spent on a cutscene skip */
void black_controls_apply(uint8_t rep[20]);

#define TYPE_GAMEPAD  0x0027CF4Cu        /* g_DeviceType_Gamepad (XDEVICE_TYPE_GAMEPAD) */
#define NPORTS        4
#define HANDLE_BASE   0x00C0DE00u        /* opaque; only these functions read it */
#define ERR_SUCCESS        0u
#define ERR_IO_PENDING     997u
#define ERR_NOT_CONNECTED  1167u

typedef struct {
    int open;
    uint32_t packet;
    uint8_t last[18];
    int have_last;
    WORD rumble_l, rumble_r;
} Port;

static Port s_port[NPORTS];
static int s_inited;
static uint32_t s_host_mask, s_reported;
static DWORD s_probe_tick;

static void input_init(void)
{
    if (s_inited)
        return;
    s_inited = 1;
    xbox_InputInit();
    fprintf(stderr, "[BLACK-INPUT] XInput API in C: no USB stack, port 0 always connected\n");
}

/* Host pads behind ports 1-3; an empty XInput slot is slow to ask, so probe at most twice a second. */
static uint32_t connected_mask(void)
{
    DWORD now = GetTickCount();
    if (!s_probe_tick || now - s_probe_tick >= 500) {
        uint32_t m = 0;
        uint8_t rep[20];
        s_probe_tick = now ? now : 1;
        for (int i = 1; i < NPORTS; i++) {
            usb_gamepad_report(i, rep, 20);
            if (xbox_InputIsConnected((DWORD)i))
                m |= 1u << i;
        }
        s_host_mask = m;
    }
    return 1u | s_host_mask;
}

static int handle_port(uint32_t h)
{
    uint32_t p = h - HANDLE_BASE - 1u;
    return p < NPORTS && s_port[p].open ? (int)p : -1;
}

void sub_0027D058(void)                  /* XInitDevices */
{
    input_init();
    s_reported = 0;
    black_ret(2, 0);
}

void sub_0027DD29(void)                  /* XGetDeviceChanges */
{
    uint32_t type = black_arg(0), pins = black_arg(1), prem = black_arg(2), cur, ins, rem;
    input_init();
    cur = type == TYPE_GAMEPAD ? connected_mask() : 0;
    ins = type == TYPE_GAMEPAD ? cur & ~s_reported : 0;
    rem = type == TYPE_GAMEPAD ? s_reported & ~cur : 0;
    if (type == TYPE_GAMEPAD)
        s_reported = cur;
    if (pins) MEM32(pins) = ins;
    if (prem) MEM32(prem) = rem;
    if (ins || rem)
        fprintf(stderr, "[BLACK-INPUT] XGetDeviceChanges: inserted %X removed %X\n", ins, rem);
    black_ret(3, (ins | rem) ? 1u : 0u);
}

void sub_0027DD96(void)                  /* XInputOpen */
{
    uint32_t type = black_arg(0), port = black_arg(1), slot = black_arg(2), h = 0;
    input_init();
    if (type == TYPE_GAMEPAD && port < NPORTS && slot == 0 && (connected_mask() >> port & 1u)) {
        memset(&s_port[port], 0, sizeof s_port[port]);
        s_port[port].open = 1;
        h = HANDLE_BASE + 1u + port;
    }
    fprintf(stderr, "[BLACK-INPUT] XInputOpen type %08X port %u slot %u -> %08X\n", type, port, slot, h);
    black_ret(4, h);
}

void sub_0027DDEC(void)                  /* XInputClose */
{
    int port = handle_port(black_arg(0));
    if (port >= 0) {
        HostVibration v = { 0, 0 };
        if (s_port[port].rumble_l || s_port[port].rumble_r)
            xbox_InputSetState((DWORD)port, &v);
        s_port[port].open = 0;
    }
    black_ret(1, 0);
}

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD = report bytes 2..19; the packet number moves only when the
 * state does, as on the console. */
void sub_0027DDF8(void)                  /* XInputGetState */
{
    uint8_t rep[20];
    uint32_t st = black_arg(1);
    int port = handle_port(black_arg(0));
    Port *p;
    if (port < 0 || !(connected_mask() >> port & 1u)) {
        black_ret(2, ERR_NOT_CONNECTED);
        return;
    }
    p = &s_port[port];
    usb_gamepad_report(port, rep, 20);
    if (port == 0 && black_skip_hides_a())
        rep[4] = 0;                      /* A that skipped a cutscene is not also a game action */
    if (port == 0)
        black_controls_apply(rep);       /* black_controls.c: modern controller / keyboard and mouse */
    if (!p->have_last || memcmp(p->last, rep + 2, 18)) {
        memcpy(p->last, rep + 2, 18);
        p->have_last = 1;
        p->packet++;
    }
    MEM32(st) = p->packet;
    for (int i = 0; i < 18; i++)
        MEM8(st + 4 + (uint32_t)i) = p->last[i];
    black_ret(2, ERR_SUCCESS);
}

/* XINPUT_FEEDBACK: header { dwStatus, hEvent, reserved[58] }, rumble at +66; completes at once. */
void sub_0027DE6B(void)                  /* XInputSetState */
{
    uint32_t fb = black_arg(1);
    int port = handle_port(black_arg(0));
    if (port < 0) {
        black_ret(2, ERR_NOT_CONNECTED);
        return;
    }
    {
        Port *p = &s_port[port];
        HostVibration v;
        v.left = MEM16(fb + 66);
        v.right = MEM16(fb + 68);
        if (v.left != p->rumble_l || v.right != p->rumble_r) {
            p->rumble_l = v.left;
            p->rumble_r = v.right;
            xbox_InputSetState((DWORD)port, &v);
        }
    }
    MEM32(fb) = ERR_SUCCESS;
    black_ret(2, ERR_IO_PENDING);
}
