/*
 * Modern controls for BLACK: the layout of recent Call of Duty / Battlefield games on a controller, plus keyboard
 * and mouse, translated to the buttons the game itself knows (its default "Custom Setup").
 *
 * The game (Xbox default)          Modern controller (in gameplay)       Keyboard and mouse
 *   R trigger   primary fire         RT  fire                              left button
 *   R3          aim (toggle)         LT  aim (held)                        right button
 *   L trigger   grenade              RB  grenade                           G
 *   D-pad down  use health pack      LB  health pack (also D-pad down)     Q / H / 4
 *   A           reload               X   reload (hold X: pick up weapon)   R (hold R: pick up)
 *   X           pick up weapon       A   pick up weapon / interact         F / E
 *   L3          crouch               B   crouch                            C / Left Ctrl
 *   B           melee                R3  melee                             V / middle button
 *   D-pad right next weapon          Y   next weapon                       2 / wheel down / mouse 5
 *   D-pad left  previous weapon      D-pad left  previous weapon           1 / wheel up / mouse 4
 *   D-pad up    firing mode          D-pad up    firing mode               X / B
 *   Y           suppressor           D-pad right suppressor                T
 *   White       next music track     L3  next music track                  M
 *   Start/Back  pause / back         Start / Back                          Esc / Tab
 *   sticks      move / look          sticks                                W A S D / mouse
 *
 * Only in gameplay: on any of the game's own pages (title, menus, pause, briefings) the pad is passed as it is,
 * so A confirms and B goes back as the game's prompts say, and the keyboard drives the menus (arrows or W A S D,
 * Enter or Space = A, Esc = B; Backspace, Back or F1 open the PC settings).
 * "In gameplay" is the game's own page pointer at 0x2F36EC being null:
 * it points to the active front-end/pause page and is cleared while the level runs (measured over menu, loading,
 * gameplay and pause snapshots). In-level tutorial prompts ("PRESS A TO CONTINUE") dim the screen through the
 * overlay alpha at 0x2E2D3C (about 0 while playing, 0.5-0.9 behind a prompt); they get the game's own pad too.
 *
 * Aim: the game's R3 toggles aiming. LT gives "hold to aim" by pressing R3 when LT goes down and again when it
 * comes up. ini [controles] mira=segurar passes LT straight to R3 instead; esquema=classico turns all of this off.
 * Settings (black-pc.ini next to the executable, [controles]): esquema=moderno|classico, mira=alternar|segurar,
 * sensibilidade_mouse=1.0, inverter_mouse=0. Environment overrides: BLACK_CONTROLS, BLACK_AIM, BLACK_MOUSE_SENS.
 */
#include "black_win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "black_native.h"
#include "native_gpu/black_gpu.h"

int xbox_FramebufferKeyDown(int vk);

#ifndef _WIN32
#include <unistd.h>
#include <ctype.h>
#define VK_LCONTROL 0xA2
#define WHEEL_DELTA 120
/* The [controles] section of black-pc.ini, read the way GetPrivateProfileString does (key=value lines). */
static void exe_dir_file(char *out, size_t cap, const char *name)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    char *slash;
    out[n > 0 ? n : 0] = 0;
    slash = strrchr(out, '/');
    snprintf(slash ? slash + 1 : out, cap - (size_t)(slash ? slash + 1 - out : 0), "%s", name);
}
static void ini_string(const char *section, const char *key, const char *def, char *out, size_t cap, const char *path)
{
    char line[256], cur[64] = "";
    FILE *f = fopen(path, "r");
    snprintf(out, cap, "%s", def);
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *e;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '[') {
            snprintf(cur, sizeof cur, "%s", p + 1);
            if ((e = strchr(cur, ']')) != NULL) *e = 0;
            continue;
        }
        if (strcasecmp(cur, section) || !(eq = strchr(p, '=')))
            continue;
        *eq = 0;
        for (e = eq - 1; e >= p && isspace((unsigned char)*e); e--) *e = 0;
        if (strcasecmp(p, key))
            continue;
        for (p = eq + 1; isspace((unsigned char)*p); p++) ;
        for (e = p + strlen(p) - 1; e >= p && isspace((unsigned char)*e); e--) *e = 0;
        snprintf(out, cap, "%s", p);
    }
    fclose(f);
}
#define GetPrivateProfileStringA(s, k, d, o, n, p) ini_string((s), (k), (d), (o), (n), (p))
static int GetPrivateProfileIntA(const char *s, const char *k, int d, const char *p)
{
    char v[32], dv[16];
    snprintf(dv, sizeof dv, "%d", d);
    ini_string(s, k, dv, v, sizeof v, p);
    return atoi(v);
}
#endif

#define PAGE_PTR      0x002F36ECu       /* active front-end/pause page; 0 in gameplay */
#define DIM_ALPHA     0x002E2D3Cu       /* screen dimming behind in-game prompts ("PRESS A TO CONTINUE"); ~0 in play */

/* XID report: bytes 2-3 digital buttons, 4-11 analog A B X Y Black White LT RT, 12-19 LX LY RX RY. */
enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8, D_START = 0x10, D_BACK = 0x20, D_L3 = 0x40, D_R3 = 0x80 };
enum { A_A, A_B, A_X, A_Y, A_BLACK, A_WHITE, A_LT, A_RT };

typedef struct {
    uint16_t d;
    uint8_t a[8];
    int16_t ax[4];
} Pad;

static void unpack(const uint8_t *r, Pad *p)
{
    p->d = (uint16_t)(r[2] | r[3] << 8);
    memcpy(p->a, r + 4, 8);
    memcpy(p->ax, r + 12, 8);
}

static void pack(const Pad *p, uint8_t *r)
{
    r[2] = (uint8_t)p->d;
    r[3] = (uint8_t)(p->d >> 8);
    memcpy(r + 4, p->a, 8);
    memcpy(r + 12, p->ax, 8);
}

static struct {
    int loaded, modern, aim_hold, invert;
    float sens;
} cfg;

static void config(void)
{
    char ini[MAX_PATH], v[64], *slash;
    const char *e;
    if (cfg.loaded)
        return;
    cfg.loaded = 1;
#ifdef _WIN32
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    strcpy(slash ? slash + 1 : ini, "black-pc.ini");
#else
    (void)slash;
    exe_dir_file(ini, sizeof ini, "black-pc.ini");
#endif
    GetPrivateProfileStringA("controles", "esquema", "moderno", v, sizeof v, ini);
    cfg.modern = _stricmp(v, "classico") != 0;
    GetPrivateProfileStringA("controles", "mira", "alternar", v, sizeof v, ini);
    cfg.aim_hold = _stricmp(v, "segurar") == 0;
    GetPrivateProfileStringA("controles", "sensibilidade_mouse", "1.0", v, sizeof v, ini);
    cfg.sens = (float)atof(v);
    cfg.invert = GetPrivateProfileIntA("controles", "inverter_mouse", 0, ini) != 0;
    if ((e = getenv("BLACK_CONTROLS")) && *e) cfg.modern = _stricmp(e, "classic") != 0 && _stricmp(e, "classico") != 0;
    if ((e = getenv("BLACK_AIM")) && *e) cfg.aim_hold = _stricmp(e, "hold") == 0 || _stricmp(e, "segurar") == 0;
    if ((e = getenv("BLACK_MOUSE_SENS")) && *e) cfg.sens = (float)atof(e);
    if (cfg.sens <= 0.0f || cfg.sens > 20.0f) cfg.sens = 1.0f;
    fprintf(stderr, "[BLACK-CONTROLS] %s controls, aim %s, mouse sensitivity %.2f%s (%s)\n",
            cfg.modern ? "modern" : "classic", cfg.aim_hold ? "held" : "toggled", cfg.sens,
            cfg.invert ? ", inverted" : "", ini);
}

static int key(int vk) { return xbox_FramebufferKeyDown(vk) != 0; }

/* A press of `ms` milliseconds started now (or kept going). */
typedef struct { ULONGLONG until; } Pulse;
static void pulse(Pulse *p, ULONGLONG now, unsigned ms) { p->until = now + ms; }
static int pulsing(const Pulse *p, ULONGLONG now) { return now < p->until; }

static int16_t stick_from_mouse(float v)
{
    /* Counts per millisecond to a stick deflection: past the game's dead zone at once, then proportional. */
    const float dz = 0.24f;
    float a = fabsf(v), s;
    if (a < 1e-4f)
        return 0;
    s = dz + 0.30f * cfg.sens * a;
    if (s > 1.0f) s = 1.0f;
    return (int16_t)(v < 0 ? -s * 32767.0f : s * 32767.0f);
}

void black_controls_apply(uint8_t rep[20])
{
    static Pulse aim_pulse, reload_pulse, prev_pulse, next_pulse;
    static int lt_was, x_was, wheel_acc;
    static ULONGLONG x_down_at, mouse_at;
    static int mouse_acc_x, mouse_acc_y;
    static int16_t mouse_rx, mouse_ry;
    ULONGLONG now = GetTickCount64();
    int in_game, dx, dy, wheel, lt, x;
    unsigned mb;
    Pad m, o;

    config();
    in_game = cfg.modern && MEM32(PAGE_PTR) == 0 && !(MEMF(DIM_ALPHA) > 0.05f);
    BlackGpuMouseCapture(in_game);
    BlackGpuMouseRead(&dx, &dy, &mb, &wheel);
    unpack(rep, &m);

    if (!in_game) {
        /* Menus: the keyboard as the game's own pad. */
        if (key(VK_UP) || key('W')) m.d |= D_UP;
        if (key(VK_DOWN) || key('S')) m.d |= D_DOWN;
        if (key(VK_LEFT) || key('A')) m.d |= D_LEFT;
        if (key(VK_RIGHT) || key('D')) m.d |= D_RIGHT;
        if (key(VK_RETURN) || key(VK_SPACE)) { m.a[A_A] = 255; m.d |= D_START; }
        if (key(VK_ESCAPE)) m.a[A_B] = 255;
        if (key(VK_TAB)) m.d |= D_BACK;
        pack(&m, rep);
        lt_was = 0;
        mouse_acc_x = mouse_acc_y = 0;
        return;
    }

    /* Keyboard and mouse onto the modern pad (host LB/RB arrive as Black/White). */
    if (key('W')) m.ax[1] = 32767;
    if (key('S')) m.ax[1] = -32767;
    if (key('A')) m.ax[0] = -32767;
    if (key('D')) m.ax[0] = 32767;
    if (key(VK_UP)) m.d |= D_UP;
    if (key(VK_DOWN)) m.d |= D_DOWN;
    if (key(VK_LEFT)) m.d |= D_LEFT;
    if (key(VK_RIGHT)) m.d |= D_RIGHT;
    if (mb & BLACK_MOUSE_LEFT) m.a[A_RT] = 255;
    if (mb & BLACK_MOUSE_RIGHT) m.a[A_LT] = 255;
    if (key('G')) m.a[A_WHITE] = 255;
    if (key('Q') || key('H') || key('4')) m.a[A_BLACK] = 255;
    if (key('F') || key('E')) m.a[A_A] = 255;
    if (key('C') || key(VK_CONTROL) || key(VK_LCONTROL)) m.a[A_B] = 255;
    if (key('R')) m.a[A_X] = 255;
    if (key('V') || (mb & BLACK_MOUSE_MIDDLE)) m.d |= D_R3;
    if (key('2') || (mb & BLACK_MOUSE_X2)) m.a[A_Y] = 255;
    if (key('1') || (mb & BLACK_MOUSE_X1)) m.d |= D_LEFT;
    if (key('X') || key('B')) m.d |= D_UP;
    if (key('T')) m.d |= D_RIGHT;
    if (key('M')) m.d |= D_L3;
    if (key(VK_ESCAPE)) m.d |= D_START;
    if (key(VK_TAB)) m.d |= D_BACK;
    wheel_acc += wheel;
    if (wheel_acc >= WHEEL_DELTA) { wheel_acc = 0; pulse(&prev_pulse, now, 90); }
    if (wheel_acc <= -WHEEL_DELTA) { wheel_acc = 0; pulse(&next_pulse, now, 90); }

    /* Mouse look: velocity over the time since the last conversion, as a right-stick deflection. */
    mouse_acc_x += dx;
    mouse_acc_y += dy;
    if (!mouse_at) mouse_at = now;
    if (now - mouse_at >= 8) {
        float dt = (float)(now - mouse_at);
        mouse_rx = stick_from_mouse(mouse_acc_x / dt);
        mouse_ry = stick_from_mouse((cfg.invert ? mouse_acc_y : -mouse_acc_y) / dt);
        mouse_acc_x = mouse_acc_y = 0;
        mouse_at = now;
    }
    if (mouse_rx) m.ax[2] = mouse_rx;
    if (mouse_ry) m.ax[3] = mouse_ry;

    /* Modern pad -> the game's buttons. */
    memset(&o, 0, sizeof o);
    memcpy(o.ax, m.ax, sizeof o.ax);
    o.d = m.d & (D_START | D_BACK | D_UP);
    o.a[A_RT] = m.a[A_RT];                               /* fire */
    o.a[A_LT] = m.a[A_WHITE];                            /* grenade */
    if (m.a[A_BLACK] || (m.d & D_DOWN)) o.d |= D_DOWN;    /* health pack */
    if (m.a[A_B]) o.d |= D_L3;                            /* crouch */
    if (m.d & D_R3) o.a[A_B] = 255;                       /* melee */
    if (m.d & D_L3) o.a[A_WHITE] = 255;                   /* next music track */
    if (m.a[A_Y] || pulsing(&next_pulse, now)) o.d |= D_RIGHT;   /* next weapon */
    if ((m.d & D_LEFT) || pulsing(&prev_pulse, now)) o.d |= D_LEFT;
    if (m.d & D_RIGHT) o.a[A_Y] = 255;                    /* suppressor */
    if (m.a[A_A]) o.a[A_X] = 255;                         /* pick up / interact */

    /* X: a tap reloads, holding it picks up the weapon on the floor. */
    x = m.a[A_X] > 64;
    if (x && !x_was) x_down_at = now;
    if (!x && x_was && now - x_down_at < 300) pulse(&reload_pulse, now, 100);
    if (x && now - x_down_at >= 300) o.a[A_X] = 255;
    x_was = x;
    if (pulsing(&reload_pulse, now)) o.a[A_A] = 255;

    /* LT: aim while held. */
    lt = m.a[A_LT] > 64;
    if (cfg.aim_hold) {
        if (lt) o.d |= D_R3;
    } else if (lt != lt_was) {
        pulse(&aim_pulse, now, 90);
    }
    lt_was = lt;
    if (pulsing(&aim_pulse, now)) o.d |= D_R3;

    pack(&o, rep);
}
