/*
 * In-game options.
 *
 * F1 on the keyboard, or Back+Start on a controller, opens a menu drawn over
 * the game by the native renderer. While it is open the game is held at its
 * Swap (the game thread waits here with the guest CPU released, the sound is
 * muted) and every key goes to the menu instead of the pad stand-in.
 *
 * Settings live in black-pc.ini next to the executable ([opcoes]). Display
 * mode, monitor, refresh rate, vsync, frame limiter, FXAA, anisotropic
 * filtering and volume apply at once. Internal resolution, aspect ratio and GPU are read once at boot (RECOMP_HLE_SCALE/ASPECT/
 * ADAPTER, RECOMP_LANGUAGE): the menu marks them and can restart the game.
 * An environment variable that is already set wins over the ini, so test
 * runs stay reproducible; RECOMP_OPTIONS=0 ignores the ini altogether.
 *
 * Text is drawn with GDI (Segoe UI, so accents and arrows render) into a DIB
 * that becomes a texture only when the menu changes.
 */
#include "hle_options.h"
#include "hle_gpu_api.h"
#include "hle_d3d11_internal.h"
#include "../recomp/recomp_types.h"
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#ifdef _WIN32
#include <xinput.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#else
/* Linux/macOS: GDI over a DIB (hle_gdi_posix.c), SDL2 for monitors, display
 * modes, fullscreen and controllers. */
#include "hle_gdi_posix.h"
#include <SDL2/SDL.h>
#define CCHDEVICENAME 32
#define ERROR_SUCCESS 0
#define VK_UP 0x26
#define VK_DOWN 0x28
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_BACK 0x08
#define VK_F1 0x70
#define WM_KEYDOWN 0x0100
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;

/* The menu's view of controller i, as XInput would give it. */
static DWORD XInputGetState(DWORD i, XINPUT_STATE *st)
{
    static SDL_GameController *pads[4];
    SDL_GameController *c;
    int k, n = 0;
    if (i >= 4)
        return 1;
    if (!pads[i] || !SDL_GameControllerGetAttached(pads[i])) {
        pads[i] = NULL;
        for (k = 0; k < SDL_NumJoysticks(); k++)
            if (SDL_IsGameController(k) && n++ == (int)i) {
                pads[i] = SDL_GameControllerOpen(k);
                break;
            }
        if (!pads[i])
            return 1;
    }
    c = pads[i];
    SDL_GameControllerUpdate();
    memset(st, 0, sizeof *st);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) st->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    st->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    st->Gamepad.sThumbLY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    return ERROR_SUCCESS;
}
#endif

extern void xbox_FramebufferKeyEvent(int vk, int down);
void black_options_volume(float volume);
void black_options_timing(void);
int guest_cpu_suspend(void);
void guest_cpu_resume(int token);
void hle_native_represent(void);

HleOptions g_opt = { 1, 0, 0, 1, 0, 0, 0, 0, 16, 0, -1, 1, 100, 1 };
static const wchar_t *ui_text(const wchar_t *pt, const wchar_t *en)
{
    return g_opt.ui_language ? en : pt;
}

static HleOptions s_boot;               /* what this process was started with */
static wchar_t s_ini[MAX_PATH];
static int s_use_ini = 1;

/* "21:9" is what ultrawide monitors really are: 3440x1440, 43:18. */
static const wchar_t *const k_aspect_name[] = { L"4:3 (original)", L"16:9", L"16:10", L"21:9 (ultrawide)", L"32:9" };
static const char *const k_aspect_env[] = { "", "16:9", "16:10", "43:18", "32:9" };
static const float k_aspect_value[] = { 4.0f / 3.0f, 16.0f / 9.0f, 1.6f, 43.0f / 18.0f, 32.0f / 9.0f };
#define N_ASPECT 5
/* Internal resolution presets: lines drawn (the game is 480) and the aspect
 * the preset implies (-1: the monitor's). Picking one also sets the aspect;
 * the aspect item can still change it afterwards. */
static const struct { int h, aspect; const wchar_t *name; } k_res[] = {
    { 0, -1, L"Resolução do monitor" },
    { 480, 0, L"Original" },
    { 720, 1, L"720p HD" },
    { 1080, 1, L"1080p Full HD" },
    { 1080, 3, L"1080p ultrawide" },
    { 1440, 1, L"1440p (2K)" },
    { 1440, 3, L"1440p ultrawide" },
    { 2160, 1, L"2160p (4K)" },
    { 4320, 1, L"4320p (8K)" },
};
#define N_RES ((int)(sizeof k_res / sizeof k_res[0]))
static const int k_fps[] = { 60, 30, 0 };
static const int k_aniso[] = { 0, 2, 4, 8, 16 };
static const int k_msaa[] = { 0, -1, 2, 4, 8 };

/* ------------------------------------------------------------- ini file */

static int ini_get(const wchar_t *key, int def)
{
    wchar_t b[32];
    if (!s_use_ini)
        return def;
    GetPrivateProfileStringW(L"opcoes", key, L"", b, 32, s_ini);
    if (!b[0])
        return def;
    if (!_wcsicmp(b, L"auto"))
        return -1;
    return _wtoi(b);
}

static void ini_put(const wchar_t *key, int v)
{
    wchar_t b[32];
    if (v < 0 && !wcscmp(key, L"gpu"))
        wcscpy(b, L"auto");
    else
        swprintf(b, 32, L"%d", v);
    WritePrivateProfileStringW(L"opcoes", key, b, s_ini);
}

static void save_ini(void)
{
    if (!s_use_ini)
        return;
    ini_put(L"modo_tela", g_opt.display);
    ini_put(L"monitor", g_opt.monitor);
    ini_put(L"taxa_hz", g_opt.refresh);
    ini_put(L"vsync", g_opt.vsync);
    ini_put(L"limite_fps", g_opt.fps);
    ini_put(L"resolucao", g_opt.scale);
    ini_put(L"proporcao", g_opt.aspect);
    ini_put(L"fxaa", g_opt.aa);
    ini_put(L"msaa", g_opt.msaa);
    ini_put(L"anisotropico", g_opt.aniso);
    ini_put(L"api_grafica", g_opt.api);
    ini_put(L"gpu", g_opt.adapter);
    ini_put(L"idioma", g_opt.language);
    ini_put(L"volume", g_opt.volume);
    ini_put(L"idioma_menu", g_opt.ui_language);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Boot-time settings travel to the rest of the runtime as environment
 * variables, read where they always were. */
/* Black USA: no FIFA language resource probing. */
#define N_LANGUAGES 1
static int installed_languages(int *list) { list[0] = 1; return 1; }
static const wchar_t *language_name(int id) { (void)id; return L"English (USA)"; }

static void env_sync(const char *name, int *v, int lo, int hi, int unset_value)
{
    const char *e = getenv(name);
    char b[16];
    if (e && *e) {
        int x = atoi(e);
        if (x >= lo && x <= hi)
            *v = x;
        return;
    }
    if (*v == unset_value)
        return;
    snprintf(b, sizeof b, "%d", *v);
    _putenv_s(name, b);
}

static void scale_publish(void)
{
    char b[16];
    if (k_res[g_opt.scale].h)
        snprintf(b, sizeof b, "%.4f", k_res[g_opt.scale].h / 480.0);
    else
        strcpy(b, "native");
    _putenv_s("RECOMP_HLE_SCALE", b);
    snprintf(b, sizeof b, "%d", g_opt.scale);
    _putenv_s("RECOMP_HLE_RES", b);
}

static void env_publish(void)
{
    char b[16];
    scale_publish();
    _putenv_s("RECOMP_HLE_ASPECT", g_opt.scale == 0 ? "" : k_aspect_env[g_opt.aspect]);
    _putenv_s("RECOMP_XBOX_WIDESCREEN", g_opt.scale != 0 && g_opt.aspect ? "1" : "0");
    snprintf(b, sizeof b, "%d", g_opt.language);
    _putenv_s("RECOMP_LANGUAGE", b);
    if (g_opt.adapter >= 0)
        snprintf(b, sizeof b, "%d", g_opt.adapter);
    else
        b[0] = 0;
    _putenv_s("RECOMP_HLE_ADAPTER", b);
    _putenv_s("RECOMP_HLE_API", g_opt.api ? "vulkan" : "d3d11");
    snprintf(b, sizeof b, "%d", g_opt.fps);
    _putenv_s("RECOMP_HLE_FPS", b);
    snprintf(b, sizeof b, "%d", g_opt.msaa);
    _putenv_s("RECOMP_HLE_MSAA", b);
}

static void native_monitor_defaults(void);

void hle_options_init(void)
{
    const char *e = getenv("RECOMP_OPTIONS");
    wchar_t *slash;
    s_use_ini = !(e && *e == '0');
    GetModuleFileNameW(NULL, s_ini, MAX_PATH);
    slash = wcsrchr(s_ini, L'\\');
    wcscpy(slash ? slash + 1 : s_ini, L"black-pc.ini");

    g_opt.display = clampi(ini_get(L"modo_tela", 1), 0, 2);
    g_opt.monitor = clampi(ini_get(L"monitor", 0), 0, 15);
    g_opt.refresh = clampi(ini_get(L"taxa_hz", 0), 0, 1000);
    g_opt.vsync = ini_get(L"vsync", 1) != 0;          /* on unless chosen off: no tearing */
    g_opt.fps = clampi(ini_get(L"limite_fps", 0), 0, 1000);
    g_opt.scale = clampi(ini_get(L"resolucao", 0), 0, N_RES - 1);
    g_opt.aspect = clampi(ini_get(L"proporcao", 0), 0, N_ASPECT - 1);
    g_opt.aa = clampi(ini_get(L"fxaa", 0), 0, 1);
    g_opt.msaa = ini_get(L"msaa", 0);
    g_opt.aniso = clampi(ini_get(L"anisotropico", 16), 0, 16);
    g_opt.api = clampi(ini_get(L"api_grafica", 0), 0, 1);
    g_opt.adapter = clampi(ini_get(L"gpu", -1), -1, 15);
    {
        int langs[N_LANGUAGES], nl = installed_languages(langs), want = ini_get(L"idioma", 1), i;
        g_opt.language = langs[0];
        for (i = 0; i < nl; i++)
            if (langs[i] == want)
                g_opt.language = want;
    }
    g_opt.volume = clampi(ini_get(L"volume", 100), 0, 100);
    g_opt.ui_language = clampi(ini_get(L"idioma_menu", 1), 0, 1);

    /* RECOMP_HLE_RES: the preset a restart from the menu asked for. A bare
     * RECOMP_HLE_SCALE (test runs) is left for the renderer as it is. */
    e = getenv("RECOMP_HLE_RES");
    if (e && *e)
        g_opt.scale = clampi(atoi(e), 0, N_RES - 1);
    e = getenv("RECOMP_HLE_SCALE");
    if (!(e && *e))
        scale_publish();
    env_sync("RECOMP_LANGUAGE", &g_opt.language, 1, 9, -99);
    env_sync("RECOMP_HLE_ADAPTER", &g_opt.adapter, -1, 15, -1);
    env_sync("RECOMP_HLE_FPS", &g_opt.fps, 0, 1000, -99);
    env_sync("RECOMP_HLE_MSAA", &g_opt.msaa, -1, 8, -99);
    if (g_opt.msaa != -1 && g_opt.msaa != 2 && g_opt.msaa != 4 && g_opt.msaa != 8)
        g_opt.msaa = 0;
    e = getenv("RECOMP_HLE_VSYNC");                  /* test runs; not published */
    if (e && *e)
        g_opt.vsync = atoi(e) != 0;
    e = getenv("RECOMP_HLE_MONITOR");                /* test runs: which monitor (0 = primary) */
    if (e && *e)
        g_opt.monitor = clampi(atoi(e), 0, 15);
    /* RECOMP_HLE_API (vulkan|d3d11) wins over the ini, like the others. */
    e = getenv("RECOMP_HLE_API");
    if (e && *e)
        g_opt.api = *e == 'v' || *e == 'V' || *e == '1';
    else if (g_opt.api)
        _putenv_s("RECOMP_HLE_API", "vulkan");
    if (!hle_gpu_d3d11_available())
        g_opt.api = 1;                          /* Linux/macOS: Vulkan (DXVK native) only */
    e = getenv("RECOMP_HLE_ASPECT");
    if (e && *e) {
        float a = 0.0f, b = 0.0f;
        int i, best = 0;
        if (sscanf(e, "%f:%f", &a, &b) == 2 && b > 0.0f)
            a /= b;
        for (i = 1; i < N_ASPECT; i++)
            if (a > 1.0f && fabsf(k_aspect_value[i] - a) < fabsf(k_aspect_value[best] - a))
                best = i;
        g_opt.aspect = best;
    } else if (g_opt.aspect && g_opt.scale != 0) {
        _putenv_s("RECOMP_HLE_ASPECT", g_opt.scale == 0 ? "" : k_aspect_env[g_opt.aspect]);
    }
    native_monitor_defaults();
    s_boot = g_opt;
    save_ini();
    fprintf(stderr, "[OPTIONS] %ls%s: display %d, monitor %d, %d Hz, vsync %d, fps %d, scale %d, aspect %d, "
            "fxaa %d, aniso %d, api %d, gpu %d, language %d, volume %d\n",
            s_ini, s_use_ini ? "" : " (ignored, RECOMP_OPTIONS=0)", g_opt.display, g_opt.monitor,
            g_opt.refresh, g_opt.vsync, g_opt.fps, g_opt.scale, g_opt.aspect, g_opt.aa, g_opt.aniso,
            g_opt.api, g_opt.adapter, g_opt.language, g_opt.volume);
}

/* ---------------------------------------------------- monitors and GPUs */

typedef struct Monitor {
    RECT rc, work;
    wchar_t device[CCHDEVICENAME];
    wchar_t name[96];
    int sdl_index;                      /* Linux/macOS: SDL display */
} Monitor;

static Monitor s_mon[8];
static int s_nmon;
static int s_rates[24], s_nrates;
static wchar_t s_gpu_name[8][128];
static int s_gpu_index[8], s_ngpu = -1;

#ifdef _WIN32
static BOOL CALLBACK monitor_cb(HMONITOR h, HDC dc, LPRECT r, LPARAM lp)
{
    MONITORINFOEXW mi;
    DISPLAY_DEVICEW dd;
    Monitor m;
    (void)dc; (void)r; (void)lp;
    if (s_nmon >= 8)
        return FALSE;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi))
        return TRUE;
    memset(&m, 0, sizeof m);
    m.rc = mi.rcMonitor;
    m.work = mi.rcWork;
    wcsncpy(m.device, mi.szDevice, CCHDEVICENAME - 1);
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    swprintf(m.name, 96, L"%ldx%ld", (long)(m.rc.right - m.rc.left), (long)(m.rc.bottom - m.rc.top));
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0]) {
        wchar_t t[96];
        swprintf(t, 96, L"%ls (%ls)", m.name, dd.DeviceString);
        wcscpy(m.name, t);
    }
    if (mi.dwFlags & MONITORINFOF_PRIMARY) {      /* the primary monitor is number 1 */
        memmove(&s_mon[1], &s_mon[0], sizeof(Monitor) * (size_t)s_nmon);
        s_mon[0] = m;
    } else {
        s_mon[s_nmon] = m;
    }
    s_nmon++;
    return TRUE;
}

static void monitors_refresh(void)
{
    s_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_cb, 0);
    if (!s_nmon) {
        memset(&s_mon[0], 0, sizeof s_mon[0]);
        SetRect(&s_mon[0].rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        s_mon[0].work = s_mon[0].rc;
        wcscpy(s_mon[0].name, ui_text(L"Principal", L"Primary"));
        s_nmon = 1;
    }
}

#else
static void monitors_refresh(void)
{
    int i, n = SDL_GetNumVideoDisplays();
    s_nmon = 0;
    for (i = 0; i < n && s_nmon < 8; i++) {
        SDL_Rect r, u;
        const char *nm = SDL_GetDisplayName(i);
        Monitor *m = &s_mon[s_nmon];
        if (SDL_GetDisplayBounds(i, &r) != 0)
            continue;
        if (SDL_GetDisplayUsableBounds(i, &u) != 0)
            u = r;
        memset(m, 0, sizeof *m);
        SetRect(&m->rc, r.x, r.y, r.x + r.w, r.y + r.h);
        SetRect(&m->work, u.x, u.y, u.x + u.w, u.y + u.h);
        m->sdl_index = i;
        swprintf(m->name, 96, L"%dx%d (%s)", r.w, r.h, nm ? nm : "?");
        s_nmon++;
    }
    if (!s_nmon) {
        memset(&s_mon[0], 0, sizeof s_mon[0]);
        SetRect(&s_mon[0].rc, 0, 0, 1280, 720);
        s_mon[0].work = s_mon[0].rc;
        wcscpy(s_mon[0].name, ui_text(L"Principal", L"Primary"));
        s_nmon = 1;
    }
}
#endif

static Monitor *monitor_current(void)
{
    if (!s_nmon)
        monitors_refresh();
    return &s_mon[g_opt.monitor < s_nmon ? g_opt.monitor : 0];
}

/* The native preset publishes both dimensions, not just a 480-line scale.
 * Explicit environment overrides still win for reproducible diagnostics. */
static void native_monitor_defaults(void)
{
    const char *aspect = getenv("RECOMP_HLE_ASPECT");
    const char *scale = getenv("RECOMP_HLE_SCALE");
    if (g_opt.scale == 0 && scale && !strcmp(scale, "native") && !(aspect && *aspect)) {
        Monitor *m = monitor_current();
        int w = m->rc.right - m->rc.left, h = m->rc.bottom - m->rc.top;
        char value[48];
        snprintf(value, sizeof value, "%d:%d", w, h);
        _putenv_s("RECOMP_HLE_ASPECT", value);
        /* Keep the exact monitor aspect; the renderer otherwise clamps
         * anamorphic output to 16:9. No FSR or post-process upscaler. */
        if (!getenv("RECOMP_XBOX_WIDESCREEN") || !*getenv("RECOMP_XBOX_WIDESCREEN"))
            _putenv_s("RECOMP_XBOX_WIDESCREEN", "0");
        for (int i = 0; i < N_ASPECT; ++i)
            if (fabsf(k_aspect_value[i] - (float)w / h) <
                fabsf(k_aspect_value[g_opt.aspect] - (float)w / h)) g_opt.aspect = i;
    }
}

float hle_options_native_scale(void)
{
    Monitor *m = monitor_current();
    float k = (float)(m->rc.bottom - m->rc.top) / 480.0f;
    return k < 1.0f ? 1.0f : k > 9.0f ? 9.0f : k;
}

#ifdef _WIN32
/* Refresh rates the monitor offers at its current resolution. */
static void rates_refresh(void)
{
    Monitor *m = monitor_current();
    DEVMODEW cur, dm;
    DWORD i;
    s_nrates = 0;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(m->device[0] ? m->device : NULL, ENUM_CURRENT_SETTINGS, &cur))
        return;
    for (i = 0; s_nrates < 24; i++) {
        int j, f;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(m->device[0] ? m->device : NULL, i, &dm))
            break;
        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight || dm.dmDisplayFrequency <= 1)
            continue;
        f = (int)dm.dmDisplayFrequency;
        for (j = 0; j < s_nrates && s_rates[j] != f; j++)
            ;
        if (j < s_nrates)
            continue;
        for (j = s_nrates; j > 0 && s_rates[j - 1] > f; j--)
            s_rates[j] = s_rates[j - 1];
        s_rates[j] = f;
        s_nrates++;
    }
}

static int monitor_current_hz(void)
{
    Monitor *m = monitor_current();
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (EnumDisplaySettingsW(m->device[0] ? m->device : NULL, ENUM_CURRENT_SETTINGS, &cur))
        return (int)cur.dmDisplayFrequency;
    return 0;
}

#else
static void rates_refresh(void)
{
    Monitor *m = monitor_current();
    SDL_DisplayMode cur, dm;
    int i, j, n;
    s_nrates = 0;
    if (SDL_GetDesktopDisplayMode(m->sdl_index, &cur) != 0)
        return;
    n = SDL_GetNumDisplayModes(m->sdl_index);
    for (i = 0; i < n && s_nrates < 24; i++) {
        if (SDL_GetDisplayMode(m->sdl_index, i, &dm) != 0 || dm.w != cur.w || dm.h != cur.h || dm.refresh_rate <= 1)
            continue;
        for (j = 0; j < s_nrates && s_rates[j] != dm.refresh_rate; j++)
            ;
        if (j < s_nrates)
            continue;
        for (j = s_nrates; j > 0 && s_rates[j - 1] > dm.refresh_rate; j--)
            s_rates[j] = s_rates[j - 1];
        s_rates[j] = dm.refresh_rate;
        s_nrates++;
    }
}

static int monitor_current_hz(void)
{
    SDL_DisplayMode cur;
    if (SDL_GetDesktopDisplayMode(monitor_current()->sdl_index, &cur) == 0)
        return cur.refresh_rate;
    return 0;
}
#endif

static void gpus_refresh(void)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL;
    UINT i;
    s_ngpu = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)))
        return;
    for (i = 0; s_ngpu < 8 && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            wcsncpy(s_gpu_name[s_ngpu], d.Description, 127);
            s_gpu_name[s_ngpu][127] = 0;
            s_gpu_index[s_ngpu++] = (int)i;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

/* -------------------------------------------------------- display mode */

static volatile LONG s_display_dirty = 1;
static volatile LONG s_toggle_fullscreen;
static unsigned s_fs_check;

#ifdef _WIN32
void hle_options_initial_window(DWORD *style, RECT *r)
{
    Monitor *m = monitor_current();
    if (g_opt.display) {
        *style = WS_POPUP | WS_VISIBLE;
        *r = m->rc;
    } else {
        RECT a = *r;
        int w, h;
        AdjustWindowRect(&a, WS_OVERLAPPEDWINDOW, FALSE);
        w = a.right - a.left;
        h = a.bottom - a.top;
        *style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        r->left = m->work.left + ((m->work.right - m->work.left) - w) / 2;
        r->top = m->work.top + ((m->work.bottom - m->work.top) - h) / 2;
        if (r->top < m->work.top)
            r->top = m->work.top;
        r->right = r->left + w;
        r->bottom = r->top + h;
    }
}

static int s_exclusive_failed;          /* 1 refused, 2 waiting for the focus */
static int s_dirty;                     /* the menu needs drawing again */

static void exclusive_enter(Monitor *m)
{
    DXGI_MODE_DESC md;
    HRESULT hr;
    memset(&md, 0, sizeof md);
    md.Width = (UINT)(m->rc.right - m->rc.left);
    md.Height = (UINT)(m->rc.bottom - m->rc.top);
    md.RefreshRate.Numerator = (UINT)(g_opt.refresh ? g_opt.refresh : monitor_current_hz());
    md.RefreshRate.Denominator = md.RefreshRate.Numerator ? 1 : 0;
    md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    IDXGISwapChain1_ResizeTarget(g_hle.swap, &md);
    hr = IDXGISwapChain1_SetFullscreenState(g_hle.swap, TRUE, NULL);
    if (FAILED(hr)) {
        /* NOT_CURRENTLY_AVAILABLE: the window is not in front; the check in
         * hle_options_apply_display retries once it is. */
        s_exclusive_failed = hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? 2 : 1;
        s_dirty = 1;
        fprintf(stderr, "[OPTIONS] exclusive fullscreen failed 0x%08lX: staying borderless\n", (unsigned long)hr);
        return;
    }
    s_exclusive_failed = 0;
    s_dirty = 1;
    IDXGISwapChain1_ResizeTarget(g_hle.swap, &md);
    g_hle.swap_w = 0;                         /* buffers resized at the next present */
    fprintf(stderr, "[OPTIONS] exclusive fullscreen %ux%u @ %u Hz\n", md.Width, md.Height, md.RefreshRate.Numerator);
}

void hle_options_apply_display(uint32_t pw, uint32_t ph)
{
    Monitor *m;
    BOOL fs = FALSE;
    if (InterlockedExchange(&s_toggle_fullscreen, 0)) {
        g_opt.display = g_opt.display ? 0 : 1;
        s_display_dirty = 1;
        save_ini();
    }
    if (g_opt.display == 2 && g_hle.swap && ++s_fs_check % 60 == 0 &&
        SUCCEEDED(IDXGISwapChain1_GetFullscreenState(g_hle.swap, &fs, NULL)) && !fs &&
        GetForegroundWindow() == g_hle.hwnd)
        s_display_dirty = 1;                      /* came back from Alt+Tab */
    if (!InterlockedExchange(&s_display_dirty, 0) || !g_hle.hwnd)
        return;
    monitors_refresh();
    m = monitor_current();
    fprintf(stderr, "[OPTIONS] display=%d monitor=%ldx%ld@%dHz picture=%ux%u vsync=%d fps=%d\n",
            g_opt.display, m->rc.right-m->rc.left, m->rc.bottom-m->rc.top,
            monitor_current_hz(), pw, ph, g_opt.vsync, g_opt.fps);
    fs = FALSE;
    if (g_hle.swap && SUCCEEDED(IDXGISwapChain1_GetFullscreenState(g_hle.swap, &fs, NULL)) && fs && g_opt.display != 2) {
        IDXGISwapChain1_SetFullscreenState(g_hle.swap, FALSE, NULL);
        g_hle.swap_w = 0;
    }
    if (g_opt.display == 0) {
        int ww = (int)(pw ? pw : 640), wh = (int)(ph ? ph : 480);
        int aw = (m->work.right - m->work.left) * 9 / 10, ah = (m->work.bottom - m->work.top) * 9 / 10;
        RECT r;
        if (ww > aw) { wh = wh * aw / ww; ww = aw; }
        if (wh > ah) { ww = ww * ah / wh; wh = ah; }
        SetRect(&r, 0, 0, ww, wh);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowLongPtrW(g_hle.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hle.hwnd, HWND_NOTOPMOST,
                     m->work.left + ((m->work.right - m->work.left) - (r.right - r.left)) / 2,
                     m->work.top + ((m->work.bottom - m->work.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(g_hle.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hle.hwnd, HWND_TOP, m->rc.left, m->rc.top, m->rc.right - m->rc.left,
                     m->rc.bottom - m->rc.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (g_opt.display == 2) {
            if (g_hle.swap)
                exclusive_enter(m);
            else
                s_display_dirty = 1;              /* once the swap chain exists */
        }
    }
}

#else
void hle_options_initial_window_sdl(int *x, int *y, int *w, int *h, unsigned *flags)
{
    Monitor *m = monitor_current();
    if (g_opt.display) {
        *x = m->rc.left;
        *y = m->rc.top;
        *w = m->rc.right - m->rc.left;
        *h = m->rc.bottom - m->rc.top;
        *flags |= g_opt.display == 2 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else {
        *x = m->work.left + ((m->work.right - m->work.left) - *w) / 2;
        *y = m->work.top + ((m->work.bottom - m->work.top) - *h) / 2;
        if (*y < m->work.top)
            *y = m->work.top;
    }
}

static int s_exclusive_failed;          /* 1 refused, 2 waiting for the focus */
static int s_dirty;                     /* the menu needs drawing again */
static Uint32 s_display_event = (Uint32)-1;
static uint32_t s_pic_w, s_pic_h;

/* Runs on the window thread (SDL wants its window driven from there): the
 * present path only posts the request. */
static void display_apply_now(void)
{
    SDL_Window *win = (SDL_Window *)g_hle.hwnd;
    Monitor *m;
    monitors_refresh();
    m = monitor_current();
    if (g_opt.display == 0) {
        int ww = (int)(s_pic_w ? s_pic_w : 640), wh = (int)(s_pic_h ? s_pic_h : 480);
        int aw = (m->work.right - m->work.left) * 9 / 10, ah = (m->work.bottom - m->work.top) * 9 / 10;
        if (ww > aw) { wh = wh * aw / ww; ww = aw; }
        if (wh > ah) { ww = ww * ah / wh; wh = ah; }
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowBordered(win, SDL_TRUE);
        SDL_SetWindowSize(win, ww, wh);
        SDL_SetWindowPosition(win, m->work.left + ((m->work.right - m->work.left) - ww) / 2,
                              m->work.top + ((m->work.bottom - m->work.top) - wh) / 2);
    } else {
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowPosition(win, m->rc.left, m->rc.top);
        if (g_opt.display == 2) {
            SDL_DisplayMode want, got;
            memset(&want, 0, sizeof want);
            want.w = m->rc.right - m->rc.left;
            want.h = m->rc.bottom - m->rc.top;
            want.refresh_rate = g_opt.refresh ? g_opt.refresh : monitor_current_hz();
            if (SDL_GetClosestDisplayMode(m->sdl_index, &want, &got))
                SDL_SetWindowDisplayMode(win, &got);
            s_exclusive_failed = SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) != 0;
            if (s_exclusive_failed) {
                fprintf(stderr, "[OPTIONS] exclusive fullscreen failed (%s): staying borderless\n", SDL_GetError());
                SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else {
                fprintf(stderr, "[OPTIONS] exclusive fullscreen %dx%d @ %d Hz\n", got.w, got.h, got.refresh_rate);
            }
        } else {
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    s_dirty = 1;
}

void hle_window_sdl_events(SDL_Event *e)
{
    if (s_display_event != (Uint32)-1 && e->type == s_display_event)
        display_apply_now();
}

void hle_options_apply_display(uint32_t pw, uint32_t ph)
{
    SDL_Event e;
    if (InterlockedExchange(&s_toggle_fullscreen, 0)) {
        g_opt.display = g_opt.display ? 0 : 1;
        s_display_dirty = 1;
        save_ini();
    }
    if (!InterlockedExchange(&s_display_dirty, 0) || !g_hle.hwnd)
        return;
    if (s_display_event == (Uint32)-1)
        s_display_event = SDL_RegisterEvents(1);
    s_pic_w = pw;
    s_pic_h = ph;
    memset(&e, 0, sizeof e);
    e.type = s_display_event;
    SDL_PushEvent(&e);
}

#endif

/* ------------------------------------------------------------ the menu */

enum {
    IT_DISPLAY, IT_MONITOR, IT_REFRESH, IT_VSYNC, IT_FPS, IT_SCALE, IT_ASPECT, IT_AA, IT_MSAA, IT_ANISO,
    IT_API, IT_GPU, IT_UI_LANG, IT_LANG, IT_VOLUME, IT_RESUME, IT_RESTART, IT_QUIT, IT_COUNT
};

static volatile LONG s_request;             /* asked to open (window thread or pad) */
static volatile LONG s_open;
static int s_sel, s_dirty = 1;
static volatile LONG s_kq_w;
static LONG s_kq_r;
static volatile WPARAM s_kq[64];

static int restart_pending(void)
{
    return g_opt.scale != s_boot.scale || g_opt.aspect != s_boot.aspect ||
           g_opt.adapter != s_boot.adapter || g_opt.language != s_boot.language ||
           g_opt.api != s_boot.api || g_opt.msaa != s_boot.msaa;
}

static int item_restart(int it)
{
    switch (it) {
    case IT_SCALE: return g_opt.scale != s_boot.scale;
    case IT_ASPECT: return g_opt.aspect != s_boot.aspect;
    case IT_API: return g_opt.api != s_boot.api;
    case IT_GPU: return g_opt.adapter != s_boot.adapter;
    case IT_LANG: return g_opt.language != s_boot.language;
    case IT_MSAA: return g_opt.msaa != s_boot.msaa;
    }
    return 0;
}

/* The front-end screen on show (game_hooks.c reports each screen load). The
 * main menu, the options list and the pause menu carry the settings button. */
static char s_screen[48];

static int screen_has_button(void)
{
    /* Black's active front-end/pause page (0x2F36EC, null while a level plays): title, main menu, options and
     * pause show the "(BACK) PC settings" button; Back (or Backspace, or a click on it) opens this menu. */
    return g_hle.primary && MEM32(0x2F36ECu) != 0;
}

int hle_options_key(UINT msg, WPARAM vk, LPARAM lp)
{
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (vk == VK_RETURN && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && (lp & (1 << 29))) {
        if (msg == WM_SYSKEYDOWN && !(lp & (1 << 30)))
            InterlockedExchange(&s_toggle_fullscreen, 1);   /* Alt+Enter */
        return 1;
    }
    if (!s_open && !s_request) {
        if (down && (vk == VK_F1 || (vk == VK_BACK && screen_has_button()))) {
            InterlockedExchange(&s_request, 1);
            xbox_FramebufferKeyEvent(-1, 0);
            return 1;
        }
        return 0;
    }
    if (down) {
        LONG w = s_kq_w;
        if (w - s_kq_r < 64) {
            s_kq[w & 63] = vk;
            InterlockedExchange(&s_kq_w, w + 1);
        }
    }
    return 1;
}

/* The mouse works the menu: pointing selects a line, left click changes it
 * forward (or runs it; on the "<" end, backward), right click backward, the
 * wheel steps through the lines. Events ride the key queue as these codes. */
enum { MK_MOVE = 0x10001, MK_LEFT, MK_RIGHT, MK_WHEEL_UP, MK_WHEEL_DOWN };
static int s_pw, s_ph;                  /* the menu surface (defined with the drawing code) */
static volatile LONG s_mouse_x = -1, s_mouse_y = -1;
static RECT s_row[IT_COUNT], s_row_left[IT_COUNT], s_hint_a, s_hint_b;

int hle_options_mouse(UINT msg, int x, int y, int wheel, int cw, int ch)
{
    WPARAM code;
    LONG w;
    if (!s_open)
        return 0;
    if (cw > 0 && ch > 0 && s_pw > 0 && s_ph > 0) {     /* client -> menu pixels */
        x = (int)((long long)x * s_pw / cw);
        y = (int)((long long)y * s_ph / ch);
    }
    InterlockedExchange(&s_mouse_x, x);
    InterlockedExchange(&s_mouse_y, y);
    switch (msg) {
    case 0x0200: code = MK_MOVE; break;                       /* WM_MOUSEMOVE */
    case 0x0201: code = MK_LEFT; break;                       /* WM_LBUTTONDOWN */
    case 0x0204: code = MK_RIGHT; break;                      /* WM_RBUTTONDOWN */
    case 0x020A: code = wheel > 0 ? MK_WHEEL_UP : MK_WHEEL_DOWN; break;   /* WM_MOUSEWHEEL */
    default: return 1;
    }
    w = s_kq_w;
    if (code == MK_MOVE && w != s_kq_r && s_kq[(w - 1) & 63] == MK_MOVE)
        return 1;                                             /* coalesce moves */
    if (w - s_kq_r < 64) {
        s_kq[w & 63] = code;
        InterlockedExchange(&s_kq_w, w + 1);
    }
    return 1;
}

int hle_options_hide_cursor(void)
{
    return g_opt.display != 0 && !s_open;
}

static int cycle(const int *list, int n, int cur, int d)
{
    int i;
    for (i = 0; i < n && list[i] != cur; i++)
        ;
    if (i == n)
        return list[0];
    return list[(i + n + d) % n];
}

static void change(int it, int d)
{
    int list[32], n = 0, i;
    switch (it) {
    case IT_DISPLAY:
        g_opt.display = (g_opt.display + 3 + d) % 3;
        s_display_dirty = 1;
        break;
    case IT_MONITOR:
        monitors_refresh();
        g_opt.monitor = (g_opt.monitor + s_nmon + d) % s_nmon;
        rates_refresh();
        s_display_dirty = 1;
        break;
    case IT_REFRESH:
        list[n++] = 0;
        for (i = 0; i < s_nrates && n < 32; i++)
            list[n++] = s_rates[i];
        g_opt.refresh = cycle(list, n, g_opt.refresh, d);
        if (g_opt.display == 2)
            s_display_dirty = 1;
        break;
    case IT_VSYNC: g_opt.vsync = !g_opt.vsync; break;
    case IT_FPS: g_opt.fps = cycle(k_fps, 3, g_opt.fps, d); break;
    case IT_SCALE:
        g_opt.scale = (g_opt.scale + N_RES + d) % N_RES;
        if (k_res[g_opt.scale].aspect >= 0) {
            g_opt.aspect = k_res[g_opt.scale].aspect;
        } else {                                /* the monitor's own shape */
            Monitor *m = monitor_current();
            float a = (float)(m->rc.right - m->rc.left) / (float)(m->rc.bottom - m->rc.top);
            g_opt.aspect = 0;
            for (i = 1; i < N_ASPECT; i++)
                if (fabsf(k_aspect_value[i] - a) < fabsf(k_aspect_value[g_opt.aspect] - a))
                    g_opt.aspect = i;
        }
        break;
    case IT_ASPECT: g_opt.aspect = (g_opt.aspect + N_ASPECT + d) % N_ASPECT; break;
    case IT_AA: g_opt.aa = !g_opt.aa; break;
    case IT_MSAA:
        g_opt.msaa = cycle(k_msaa, 5, g_opt.msaa, d);
        if (g_opt.msaa < 0 && g_hle.game_msaa <= 1)   /* the game asks for none: no "game default" */
            g_opt.msaa = cycle(k_msaa, 5, g_opt.msaa, d);
        break;
    case IT_ANISO: g_opt.aniso = cycle(k_aniso, 5, g_opt.aniso, d); break;
    case IT_API:
        if (!hle_gpu_vulkan_available() || !hle_gpu_d3d11_available())
            break;
        g_opt.api = !g_opt.api;
        /* GPU numbers are the chosen API's (DXGI vs Vulkan devices). */
        g_opt.adapter = g_opt.api == s_boot.api ? s_boot.adapter : -1;
        break;
    case IT_GPU:
        if (g_opt.api != s_boot.api)
            break;                              /* the list is the running API's */
        list[n++] = -1;
        for (i = 0; i < s_ngpu; i++)
            list[n++] = s_gpu_index[i];
        g_opt.adapter = cycle(list, n, g_opt.adapter, d);
        break;
    case IT_UI_LANG: g_opt.ui_language = !g_opt.ui_language; break;
    case IT_LANG:
        n = installed_languages(list);
        g_opt.language = cycle(list, n, g_opt.language, d);
        break;
    case IT_VOLUME: g_opt.volume = clampi(g_opt.volume + 10 * d, 0, 100); break;
    }
    s_dirty = 1;
}

static void leave_fullscreen(void)
{
#ifdef _WIN32
    BOOL fs = FALSE;
    EnterCriticalSection(&g_hle.lock);
    if (g_hle.swap && SUCCEEDED(IDXGISwapChain1_GetFullscreenState(g_hle.swap, &fs, NULL)) && fs)
        IDXGISwapChain1_SetFullscreenState(g_hle.swap, FALSE, NULL);
    LeaveCriticalSection(&g_hle.lock);
#else
    if (g_hle.hwnd)
        SDL_SetWindowFullscreen((SDL_Window *)g_hle.hwnd, 0);
#endif
}

static void restart_game(void)
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
#endif
    save_ini();
    env_publish();                              /* the new choices win over the old boot values */
    _putenv_s("RECOMP_OPTIONS_AUTOTEST", "");     /* a scripted test restarts once, not for ever */
    leave_fullscreen();
#ifndef _WIN32
    {
        /* Same process image, same environment (the new choices are in it). */
        char exe[MAX_PATH];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        fprintf(stderr, "[OPTIONS] restarting to apply scale %d, aspect %d, api %d, gpu %d, language %d\n",
                g_opt.scale, g_opt.aspect, g_opt.api, g_opt.adapter, g_opt.language);
        fflush(stdout);
        fflush(stderr);
        if (n > 0) {
            exe[n] = 0;
            execl(exe, exe, (char *)NULL);
        }
        fprintf(stderr, "[OPTIONS] restart failed\n");
        return;
    }
#else
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fprintf(stderr, "[OPTIONS] restarting to apply scale %d, aspect %d, api %d, gpu %d, language %d\n",
            g_opt.scale, g_opt.aspect, g_opt.api, g_opt.adapter, g_opt.language);
    fflush(stdout);
    fflush(stderr);
    if (CreateProcessW(exe, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    fprintf(stderr, "[OPTIONS] restart failed (%lu)\n", (unsigned long)GetLastError());
#endif
}

static int row_at(int x, int y)
{
    POINT p;
    int it;
    p.x = x;
    p.y = y;
    for (it = 0; it < IT_COUNT; it++)
        if (PtInRect(&s_row[it], p))
            return it;
    return -1;
}

static void menu_key(int vk);

static void menu_mouse(int code)
{
    POINT p;
    int row;
    p.x = s_mouse_x;
    p.y = s_mouse_y;
    row = row_at(p.x, p.y);
    switch (code) {
    case MK_MOVE:
        if (row >= 0 && row != s_sel) {
            s_sel = row;
            s_dirty = 1;
        }
        break;
    case MK_LEFT: case MK_RIGHT:
        if (PtInRect(&s_hint_b, p)) { menu_key(VK_ESCAPE); break; }
        if (PtInRect(&s_hint_a, p)) { menu_key(VK_RETURN); break; }
        if (row < 0) {
            if (code == MK_RIGHT)
                menu_key(VK_ESCAPE);                         /* right click outside: back */
            break;
        }
        s_sel = row;
        s_dirty = 1;
        if (code == MK_RIGHT || PtInRect(&s_row_left[row], p))
            menu_key(VK_LEFT);
        else
            menu_key(VK_RETURN);
        break;
    case MK_WHEEL_UP: menu_key(VK_UP); break;
    case MK_WHEEL_DOWN: menu_key(VK_DOWN); break;
    }
}

static void menu_key(int vk)
{
    if (vk >= MK_MOVE) {
        menu_mouse(vk);
        return;
    }
    switch (vk) {
    case VK_UP:
        do s_sel = (s_sel + IT_COUNT - 1) % IT_COUNT; while (s_sel == IT_RESTART && !restart_pending());
        s_dirty = 1;
        break;
    case VK_DOWN:
        do s_sel = (s_sel + 1) % IT_COUNT; while (s_sel == IT_RESTART && !restart_pending());
        s_dirty = 1;
        break;
    case VK_LEFT: change(s_sel, -1); break;
    case VK_RIGHT: change(s_sel, 1); break;
    case VK_RETURN: case VK_SPACE:
        if (s_sel == IT_RESUME)
            s_open = 0;
        else if (s_sel == IT_RESTART)
            restart_game();
        else if (s_sel == IT_QUIT) {
            save_ini();
            leave_fullscreen();
            fflush(stderr);
            ExitProcess(0);
        } else
            change(s_sel, 1);
        break;
    case VK_ESCAPE: case VK_F1: case VK_BACK:
        s_open = 0;
        break;
    }
}

/* Every connected pad's buttons OR'd, the left stick as a d-pad. A slot
 * without a pad is probed only now and then: XInputGetState on an empty
 * slot is slow. */
static int s_pad_seen[4];
static unsigned s_pad_probe;

static WORD pads_read(int probe_all)
{
    WORD b = 0;
    int i;
    s_pad_probe++;
    for (i = 0; i < 4; i++) {
        XINPUT_STATE st;
        if (!s_pad_seen[i] && !probe_all && (s_pad_probe % 240u) != (unsigned)i * 60u)
            continue;
        if (XInputGetState((DWORD)i, &st) != ERROR_SUCCESS) {
            s_pad_seen[i] = 0;
            continue;
        }
        if (!s_pad_seen[i])
            fprintf(stderr, "[OPTIONS] controller %d connected\n", i);
        s_pad_seen[i] = 1;
        b |= st.Gamepad.wButtons;
        /* RECOMP_OPTIONS_PADLOG: each change of the buttons the menu sees. */
        {
            static int padlog = -1;
            static WORD last[4];
            if (padlog < 0)
                padlog = getenv("RECOMP_OPTIONS_PADLOG") != NULL;
            if (padlog && st.Gamepad.wButtons != last[i])
                fprintf(stderr, "[OPTIONS] pad %d buttons %04X (screen %s)\n", i, st.Gamepad.wButtons, s_screen);
            last[i] = st.Gamepad.wButtons;
        }
        if (st.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    return b;
}

/* RECOMP_OPTIONS_AUTOTEST=<swap>[:<script>]: at that Swap the menu opens by
 * itself and plays the script -- evidence for a run nobody is at the keyboard
 * for. Swap 0: nothing opens it, the first session opened any other way (the
 * game's Options > Video) plays the script. Script letters, one every 6
 * menu frames: U D L R arrows, E Enter, X Esc, C capture, W wait 20 frames.
 * Default "WCUUUUUURWCX": capture, FXAA on, capture, close. Captures go next
 * to the frame dumps (RECOMP_FB_DUMP) as <prefix>-options-c<N>.bmp. */
static int s_autotest = -1, s_auto_step, s_auto_wait, s_auto_shots;
static char s_auto_script[128] = "WCUUUUUURWCX";
static const char *volatile s_capture;
static char s_capture_tag[16];

const char *hle_options_capture_tag(void)
{
    /* RECOMP_OPTIONS_SHOT_EVERY=<swaps>: the presented frame (menu button
     * included) every so many swaps, as <prefix>-options-s<swap>.bmp. */
    static int every = -1;
    static char tag[24];
    const char *t = s_capture;
    s_capture = NULL;
    if (every < 0) {
        const char *e = getenv("RECOMP_OPTIONS_SHOT_EVERY");
        every = e ? atoi(e) : 0;
    }
    if (!t && every > 0 && !s_open && g_hle.swaps % (uint64_t)every == 0) {
        snprintf(tag, sizeof tag, "s%06llu", (unsigned long long)g_hle.swaps);
        t = tag;
    }
    return t;
}

static void autotest_step(void)
{
    char c;
    if (s_auto_wait > 0) {
        s_auto_wait--;
        return;
    }
    c = s_auto_script[s_auto_step];
    if (!c)
        return;
    s_auto_step++;
    s_auto_wait = c == 'W' ? 20 : 6;
    switch (c) {
    case 'U': menu_key(VK_UP); break;
    case 'D': menu_key(VK_DOWN); break;
    case 'L': menu_key(VK_LEFT); break;
    case 'R': menu_key(VK_RIGHT); break;
    case 'E': menu_key(VK_RETURN); break;
    case 'X': menu_key(VK_ESCAPE); break;
    case 'C':
        snprintf(s_capture_tag, sizeof s_capture_tag, "c%d", ++s_auto_shots);
        s_capture = s_capture_tag;
        break;
    }
}

static void menu_loop(void)
{
    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                      XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD prev, b, rep_b = 0;
    DWORD rep_t = 0;
    int token, i;

    s_kq_r = s_kq_w;
    if (s_autotest == -2)
        s_autotest = 0;
    s_auto_step = s_auto_wait = 0;
    InterlockedExchange(&s_open, 1);
    InterlockedExchange(&s_request, 0);
    xbox_FramebufferKeyEvent(-1, 0);
    monitors_refresh();
    rates_refresh();
    if (s_ngpu < 0)
        gpus_refresh();
    s_sel = IT_RESUME;
    s_dirty = 1;
    fprintf(stderr, "[OPTIONS] menu open: game paused\n");
    fflush(stderr);
    token = guest_cpu_suspend();
    black_options_volume(0.0f);
    prev = pads_read(1);                     /* the buttons that opened it do not count */
    while (s_open) {
        WORD edge;
        DWORD now = GetTickCount();
        LONG w = s_kq_w;
        while (s_kq_r != w) {
            menu_key((int)s_kq[s_kq_r & 63]);
            s_kq_r++;
        }
        b = pads_read(0);
        edge = b & (WORD)~prev;
        if (edge & dirs) {
            rep_b = edge & dirs;
            rep_t = now + 350;
        } else if ((b & dirs) && (b & dirs) == rep_b && (LONG)(now - rep_t) >= 0) {
            edge |= rep_b;
            rep_t = now + 110;
        }
        if (!(b & dirs))
            rep_b = 0;
        prev = b;
        if (edge & XINPUT_GAMEPAD_DPAD_UP) menu_key(VK_UP);
        if (edge & XINPUT_GAMEPAD_DPAD_DOWN) menu_key(VK_DOWN);
        if (edge & XINPUT_GAMEPAD_DPAD_LEFT) menu_key(VK_LEFT);
        if (edge & XINPUT_GAMEPAD_DPAD_RIGHT) menu_key(VK_RIGHT);
        if (edge & XINPUT_GAMEPAD_A) menu_key(VK_RETURN);
        if (edge & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) menu_key(VK_ESCAPE);
        if (s_autotest == 0)
            autotest_step();
        hle_native_represent();
        Sleep(15);
    }
    /* A button still held would reach the game as a fresh press. */
    for (i = 0; i < 100 && pads_read(0); i++)
        Sleep(15);
    save_ini();
    black_options_volume((float)g_opt.volume / 100.0f);
    s_dirty = 1;
    if (s_autotest == 0)
        s_autotest = 0x7FFFFFFF;
    guest_cpu_resume(token);
    fprintf(stderr, "[OPTIONS] menu closed: display %d, vsync %d, fps %d, fxaa %d, msaa %d, aniso %d, volume %d%s\n",
            g_opt.display, g_opt.vsync, g_opt.fps, g_opt.aa, g_opt.msaa, g_opt.aniso, g_opt.volume,
            restart_pending() ? " (restart pending)" : "");
}

void hle_options_frame(void)
{
    black_options_timing();
    static WORD prev;
    static int volume_set = -1;
    static unsigned frames;
    WORD b;
    if (s_autotest < 0) {
        const char *e = getenv("RECOMP_OPTIONS_AUTOTEST");
        const char *colon = e ? strchr(e, ':') : NULL;
        s_autotest = e && *e ? atoi(e) : 0x7FFFFFFF;
        if (colon && colon[1])
            snprintf(s_auto_script, sizeof s_auto_script, "%s", colon + 1);
        if (s_autotest == 0)
            s_autotest = -2;                          /* script the first session */
    }
    if (s_autotest != 0x7FFFFFFF && s_autotest > 0 && ++frames >= (unsigned)s_autotest) {
        s_autotest = 0;                               /* this session is scripted */
        InterlockedExchange(&s_request, 1);
    }
    if (volume_set != g_opt.volume) {
        volume_set = g_opt.volume;
        black_options_volume((float)g_opt.volume / 100.0f);
    }
    b = pads_read(0);
    if ((b & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START) &&
        (prev & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) != (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))
        InterlockedExchange(&s_request, 1);
    /* Back alone on a screen showing the button. */
    if (screen_has_button() && (b & XINPUT_GAMEPAD_BACK) && !(b & XINPUT_GAMEPAD_START) &&
        !(prev & XINPUT_GAMEPAD_BACK))
        InterlockedExchange(&s_request, 1);
    prev = b;
    if (s_request && !s_open)
        menu_loop();
}

/* -------------------------------------------------------------- drawing */

static HDC s_dc;
static HBITMAP s_bmp;
static uint32_t *s_bits;
static HFONT s_font, s_font_bold, s_font_small;
static int s_pw, s_ph, s_lh;
static ID3D11Texture2D *s_tex;
static ID3D11ShaderResourceView *s_srv;
static ID3D11VertexShader *s_vs;
static ID3D11PixelShader *s_ps;
static ID3D11BlendState *s_blend;
static ID3D11SamplerState *s_samp;

#define BG_RGB      RGB(10, 16, 30)
#define BG_PIXEL    0x0A101Eu                 /* the same colour as a DIB pixel */

static int overlay_resources(void)
{
    static const char src[] =
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0);"
        "struct V{float4 p:SV_Position; float2 uv:TEXCOORD0;};"
        "V vs(uint id:SV_VertexID){V o; float2 q=float2(id==1?2:0,id==2?2:0);"
        "o.p=float4(q.x*2-1,1-q.y*2,0,1); o.uv=q; return o;}"
        "float4 ps(V i):SV_Target{return t.Sample(s,i.uv);}";
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    HRESULT hr;
    if (s_vs && s_ps && s_blend && s_samp)
        return 1;
    hr = hle_d3d_compile(src, sizeof src - 1, "options", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err);
    if (err) { fprintf(stderr, "[OPTIONS] %s\n", (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); err = NULL; }
    if (SUCCEEDED(hr))
        hr = hle_d3d_compile(src, sizeof src - 1, "options", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (err) { fprintf(stderr, "[OPTIONS] %s\n", (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(g_hle.dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &s_ps);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (SUCCEEDED(hr)) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(g_hle.dev, &bd, &s_blend);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(g_hle.dev, &sd, &s_samp);
    }
    if (FAILED(hr))
        fprintf(stderr, "[OPTIONS] overlay setup failed 0x%08lX\n", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static HFONT s_font_title;

/* Native Windows fonts: no game-specific assets are required. */
#define FACE_TITLE L"Segoe UI"
#define FACE_BODY L"Segoe UI"

static HFONT make_face(const wchar_t *face, int px, int weight, int italic)
{
    return CreateFontW(-px, 0, 0, 0, weight, (DWORD)italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static HFONT make_font(int px, int weight, int italic)
{
    return make_face(FACE_BODY, px, weight, italic);
}

/* The screen covers the whole window: a DIB of the window's size. */
static void surface_resize(int pw, int ph, int lh)
{
    BITMAPINFO bi;
    D3D11_TEXTURE2D_DESC td;
    if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); s_srv = NULL; }
    if (s_tex) { ID3D11Texture2D_Release(s_tex); s_tex = NULL; }
    if (s_bmp) { DeleteObject(s_bmp); s_bmp = NULL; s_bits = NULL; }
    if (s_font) {
        DeleteObject(s_font); DeleteObject(s_font_bold); DeleteObject(s_font_small); DeleteObject(s_font_title);
        s_font = NULL;
    }
    if (!s_dc)
        s_dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
    if (!s_bmp)
        return;
    SelectObject(s_dc, s_bmp);
    s_font = make_font(lh * 74 / 100, FW_BOLD, 0);
    s_font_bold = make_font(lh * 74 / 100, FW_BOLD, 0);
    s_font_small = make_font(lh * 54 / 100, FW_BOLD, 0);
    s_font_title = make_face(FACE_TITLE, lh * 155 / 100, FW_BOLD, 0);
    memset(&td, 0, sizeof td);
    td.Width = (UINT)pw;
    td.Height = (UINT)ph;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &s_tex)))
        ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)s_tex, NULL, &s_srv);
    s_pw = pw;
    s_ph = ph;
    s_lh = lh;
    s_dirty = 1;
}

static void text(const wchar_t *s, int x0, int y0, int x1, int y1, COLORREF c, HFONT f, UINT align)
{
    RECT r;
    SetRect(&r, x0, y0, x1, y1);
    SelectObject(s_dc, f);
    SetTextColor(s_dc, c);
    DrawTextW(s_dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void fill(int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    SetRect(&r, x0, y0, x1, y1);
    FillRect(s_dc, &r, b);
    DeleteObject(b);
}

/* The game's menu text: light lines carry a dark drop shadow. */
static void text_sh(const wchar_t *s, int x0, int y0, int x1, int y1, COLORREF c, HFONT f, UINT align)
{
    int o = s_lh / 14 + 1;
    text(s, x0 + o, y0 + o, x1 + o, y1 + o, RGB(6, 10, 18), f, align);
    text(s, x0, y0, x1, y1, c, f, align);
}

static int text_width(const wchar_t *s, HFONT f)
{
    SIZE sz;
    SelectObject(s_dc, f);
    GetTextExtentPoint32W(s_dc, s, (int)wcslen(s), &sz);
    return sz.cx;
}

/* Menu lines are capitals, as on the game's screens; "1366x768" keeps its x. */
static void upper(wchar_t *s)
{
    int i;
    for (i = 0; s[i]; i++)
        if ((s[i] >= L'a' && s[i] <= L'z' && !(s[i] == L'x' && i && s[i - 1] >= L'0' && s[i - 1] <= L'9')) ||
            (s[i] >= 0xE0 && s[i] <= 0xFE && s[i] != 0xF7))
            s[i] = (wchar_t)(s[i] - 0x20);
}

static void poly(const POINT *p, int n, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    SelectObject(s_dc, b);
    SelectObject(s_dc, pen);
    Polygon(s_dc, p, n);
    DeleteObject(b);
    DeleteObject(pen);
}

/* A triangle pointing left (dir < 0) or right, tip at (x, y). */
static void arrow(int x, int y, int size, int dir, COLORREF c)
{
    POINT p[3];
    p[0].x = x; p[0].y = y;
    p[1].x = x - dir * size * 9 / 10; p[1].y = y - size;
    p[2].x = x - dir * size * 9 / 10; p[2].y = y + size;
    poly(p, 3, c);
}

/* Left-to-right colour ramp. */
static void hramp(int x0, int x1, int y0, int y1, COLORREF a, COLORREF b)
{
    int x, n = x1 - x0;
    for (x = x0; x < x1; x += 2) {
        int t = n > 0 ? (x - x0) * 256 / n : 256;
        fill(x, y0, x + 2, y1, RGB((GetRValue(a) * (256 - t) + GetRValue(b) * t) >> 8,
                                   (GetGValue(a) * (256 - t) + GetGValue(b) * t) >> 8,
                                   (GetBValue(a) * (256 - t) + GetBValue(b) * t) >> 8));
    }
}

/* A rough paint stroke, like the white brush lines framing the game's
 * screens: short quads of uneven width with a few gaps. */
static void brush(int ax, int ay, int bx, int by, int th, COLORREF c, unsigned seed)
{
    double dx = bx - ax, dy = by - ay, len = sqrt(dx * dx + dy * dy), nx, ny;
    int i, steps;
    if (len < 1.0)
        return;
    nx = -dy / len;
    ny = dx / len;
    steps = (int)(len / (th * 0.7)) + 1;
    for (i = 0; i < steps; i++) {
        double t0 = (double)i / steps, t1 = (double)(i + 1) / steps, h0, h1;
        POINT p[4];
        seed = seed * 1103515245u + 12345u;
        if (((seed >> 16) & 63) == 0 && i && i + 1 < steps)
            continue;                                        /* a dry gap */
        h0 = th * (0.36 + ((seed >> 8) & 63) / 384.0);
        seed = seed * 1103515245u + 12345u;
        h1 = th * (0.36 + ((seed >> 8) & 63) / 384.0);
        p[0].x = (LONG)(ax + dx * t0 + nx * h0); p[0].y = (LONG)(ay + dy * t0 + ny * h0);
        p[1].x = (LONG)(ax + dx * t1 + nx * h1 + dx / len); p[1].y = (LONG)(ay + dy * t1 + ny * h1 + dy / len);
        p[2].x = (LONG)(ax + dx * t1 - nx * h1 + dx / len); p[2].y = (LONG)(ay + dy * t1 - ny * h1 + dy / len);
        p[3].x = (LONG)(ax + dx * t0 - nx * h0); p[3].y = (LONG)(ay + dy * t0 - ny * h0);
        poly(p, 4, c);
    }
}

/* A controller button as the game draws it: a coloured disc with a white
 * letter, then the caption in capitals. Returns the x after it. */
static int button_hint(int x, int y, int lh, wchar_t letter, COLORREF c, const wchar_t *caption)
{
    wchar_t l[2] = { letter, 0 }, cap[64];
    int d = lh * 72 / 100, w;
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
    SelectObject(s_dc, b);
    SelectObject(s_dc, pen);
    Ellipse(s_dc, x, y + (lh - d) / 2, x + d, y + (lh - d) / 2 + d);
    DeleteObject(b);
    DeleteObject(pen);
    text(l, x, y, x + d, y + lh, RGB(255, 255, 255), s_font_small, DT_CENTER);
    swprintf(cap, 64, L"%ls", caption);
    upper(cap);
    w = text_width(cap, s_font_small);
    text_sh(cap, x + d + lh / 4, y, x + d + lh / 4 + w + 4, y + lh, RGB(240, 240, 240), s_font_small, DT_LEFT);
    return x + d + lh / 4 + w + lh;
}

static void item_text(int it, wchar_t *label, wchar_t *value, int *greyed)
{
    uint32_t bw = g_hle.pp_w ? g_hle.pp_w : 640, bh = g_hle.pp_h ? g_hle.pp_h : 480;
    int i;
    *greyed = 0;
    value[0] = 0;
    switch (it) {
    case IT_DISPLAY:
        wcscpy(label, ui_text(L"Modo de tela", L"Display mode"));
        wcscpy(value, g_opt.display == 0 ? ui_text(L"Janela", L"Windowed") : g_opt.display == 1 ? ui_text(L"Tela cheia (sem bordas)", L"Borderless fullscreen") :
               s_exclusive_failed == 2 ? ui_text(L"Tela cheia exclusiva (ao focar a janela)", L"Exclusive fullscreen (when focused)") :
               s_exclusive_failed ? ui_text(L"Exclusiva recusada pelo Windows: sem bordas", L"Exclusive unavailable: borderless") : ui_text(L"Tela cheia exclusiva", L"Exclusive fullscreen"));
        break;
    case IT_MONITOR:
        wcscpy(label, L"Monitor");
        swprintf(value, 128, L"%d: %ls", (g_opt.monitor < s_nmon ? g_opt.monitor : 0) + 1, monitor_current()->name);
        break;
    case IT_REFRESH:
        wcscpy(label, ui_text(L"Taxa de atualização", L"Refresh rate"));
        if (g_opt.refresh)
            swprintf(value, 128, L"%d Hz", g_opt.refresh);
        else
            swprintf(value, 128, ui_text(L"A do monitor (%d Hz)", L"Monitor refresh rate (%d Hz)"), monitor_current_hz());
        *greyed = g_opt.display != 2;
        break;
    case IT_VSYNC:
        wcscpy(label, ui_text(L"Sincronia vertical (VSync)", L"Vertical sync (VSync)"));
        wcscpy(value, g_opt.vsync ? ui_text(L"Ligada", L"On") : ui_text(L"Desligada", L"Off"));
        break;
    case IT_FPS:
        wcscpy(label, ui_text(L"Limite de quadros", L"Frame limit"));
        if (g_opt.fps == 60) wcscpy(value, L"60 fps (original)");
        else if (g_opt.fps) swprintf(value, 128, L"%d fps", g_opt.fps);
        else wcscpy(value, ui_text(L"Sem limite", L"Unlimited"));
        break;
    case IT_SCALE:
        wcscpy(label, ui_text(L"Resolução interna", L"Internal resolution"));
    {
        unsigned ih = k_res[g_opt.scale].h ? (unsigned)k_res[g_opt.scale].h
                                           : (unsigned)((float)bh * hle_options_native_scale() + 0.5f);
        unsigned iw = (unsigned)((float)ih * (g_opt.aspect ? k_aspect_value[g_opt.aspect] : (float)bw / (float)bh) + 0.5f);
        if (g_opt.scale == 0) {
            Monitor *m = monitor_current();
            iw = m->rc.right - m->rc.left;
            ih = m->rc.bottom - m->rc.top;
        }
        swprintf(value, 128, L"%ls (%ux%u)", (g_opt.scale == 0 ? ui_text(L"Resolução do monitor", L"Monitor resolution") : k_res[g_opt.scale].name), (iw + 1) & ~1u, ih);
    }
        break;
    case IT_ASPECT:
        wcscpy(label, ui_text(L"Proporção da imagem", L"Aspect ratio"));
        wcscpy(value, k_aspect_name[g_opt.aspect]);
        break;
    case IT_AA:
        wcscpy(label, ui_text(L"Antisserrilhamento", L"Anti-aliasing"));
        wcscpy(value, g_opt.aa ? L"FXAA" : ui_text(L"Desligado", L"Off"));
        break;
    case IT_MSAA:
        wcscpy(label, ui_text(L"Multiamostragem (MSAA)", L"Multisampling (MSAA)"));
        if (g_opt.msaa > 0)
            swprintf(value, 128, L"%dx", g_opt.msaa);
        else if (g_opt.msaa < 0 && g_hle.game_msaa > 1)
            swprintf(value, 128, ui_text(L"O do jogo (%ux)", L"Game default (%ux)"), g_hle.game_msaa);
        else if (g_opt.msaa < 0)
            wcscpy(value, ui_text(L"O do jogo (nenhum)", L"Game default (none)"));
        else
            wcscpy(value, ui_text(L"Desligada", L"Off"));
        break;
    case IT_ANISO:
        wcscpy(label, ui_text(L"Filtro anisotrópico", L"Anisotropic filtering"));
        if (g_opt.aniso) swprintf(value, 128, L"%dx", g_opt.aniso);
        else wcscpy(value, ui_text(L"O do jogo", L"Game default"));
        break;
    case IT_API:
        wcscpy(label, ui_text(L"API gráfica", L"Graphics API"));
        wcscpy(value, g_opt.api ? L"Vulkan" : L"DirectX 11");
        *greyed = !hle_gpu_vulkan_available() || !hle_gpu_d3d11_available();
        break;
    case IT_GPU:
        wcscpy(label, ui_text(L"Placa de vídeo", L"Graphics card"));
#ifdef _WIN32
        wcscpy(value, ui_text(L"Automática (padrão do Windows)", L"Automatic (Windows default)"));
#else
        wcscpy(value, ui_text(L"Automática (padrão do sistema)", L"Automatic (system default)"));
#endif
        for (i = 0; i < s_ngpu && g_opt.api == s_boot.api; i++)
            if (s_gpu_index[i] == g_opt.adapter)
                wcscpy(value, s_gpu_name[i]);
        break;
    case IT_UI_LANG:
        wcscpy(label, ui_text(L"Idioma do menu", L"Menu language"));
        wcscpy(value, g_opt.ui_language ? L"English" : L"Português");
        break;
    case IT_LANG:
        *greyed = 1;
        wcscpy(label, ui_text(L"Idioma do jogo", L"Game language"));
        wcscpy(value, language_name(g_opt.language));
        break;
    case IT_VOLUME:
        wcscpy(label, L"Volume");
        swprintf(value, 128, L"%d%%", g_opt.volume);
        break;
    case IT_RESUME: wcscpy(label, ui_text(L"Voltar", L"Back")); break;
    case IT_RESTART: wcscpy(label, ui_text(L"Reiniciar o jogo para aplicar *", L"Restart game to apply *")); *greyed = !restart_pending(); break;
    case IT_QUIT: wcscpy(label, ui_text(L"Sair do jogo", L"Quit game")); break;
    }
}

static const wchar_t *item_help(int it)
{
    switch (it) {
    case IT_DISPLAY: return ui_text(L"Janela, tela cheia sem bordas (troca rápida com Alt+Tab) ou tela cheia exclusiva.", L"Windowed, borderless fullscreen (quick Alt+Tab) or exclusive fullscreen.");
    case IT_MONITOR: return ui_text(L"Em qual monitor o jogo aparece.", L"Monitor used to display the game.");
    case IT_REFRESH: return ui_text(L"Frequência do monitor na tela cheia exclusiva.", L"Monitor refresh rate in exclusive fullscreen.");
    case IT_VSYNC: return ui_text(L"Espera o monitor para mostrar cada quadro: sem cortes na imagem, um pouco mais de atraso.", L"Synchronizes frames with the monitor to prevent tearing, with slightly more latency.");
    case IT_FPS: return ui_text(L"Limite opcional de apresentação. VSync é controlado separadamente.", L"Optional presentation limit. VSync is controlled separately.");
    case IT_SCALE: return ui_text(L"Resolução em que o jogo é desenhado: maior = imagem mais nítida, mais trabalho para a placa. Também ajusta a proporção.", L"Higher rendering resolutions give a sharper image and increase GPU load. Also adjusts aspect ratio.");
    case IT_ASPECT: return ui_text(L"4:3 é o original. As outras alargam o campo de visão para telas largas.", L"4:3 is the original. Other ratios widen the field of view for widescreen displays.");
    case IT_AA: return ui_text(L"FXAA suaviza as bordas serrilhadas com custo quase nulo.", L"FXAA smooths jagged edges with minimal performance cost.");
    case IT_MSAA: return ui_text(L"Suaviza as bordas da cena 3D com várias amostras por pixel. \"O do jogo\" usa o que o Xbox usava. Reinicia o jogo.", L"Smooths the edges of the 3D scene with several samples per pixel. \"Game default\" uses what the Xbox used. Restarts the game.");
    case IT_ANISO: return ui_text(L"Deixa nítidas as texturas vistas em ângulo, como o chão.", L"Sharpens textures viewed at an angle, such as the ground.");
    case IT_API: return !hle_gpu_d3d11_available() ? ui_text(L"Neste sistema o jogo desenha com Vulkan (DXVK).", L"This system renders the game with Vulkan (DXVK).")
                      : hle_gpu_vulkan_available()
                        ? ui_text(L"DirectX 11 ou Vulkan (pela DXVK). A imagem é a mesma; use a que rodar melhor na sua placa.", L"DirectX 11 or Vulkan (DXVK). Choose whichever performs better on your GPU.")
                        : ui_text(L"Vulkan precisa do driver Vulkan da placa e da pasta vulkan ao lado do jogo.", L"Vulkan requires a Vulkan driver and the vulkan folder next to the game.");
    case IT_GPU: return ui_text(L"Placa de vídeo usada para desenhar o jogo.", L"Graphics card used to render the game.");
    case IT_UI_LANG: return ui_text(L"Idioma deste menu de configurações. A mudança é imediata.", L"Language of this settings menu. Changes apply immediately.");
    case IT_LANG: return ui_text(L"Idioma dos textos e narração do jogo.", L"Game language supported by the installed game resources.");
    case IT_VOLUME: return ui_text(L"Volume geral do jogo.", L"Overall game volume.");
    case IT_RESUME: return ui_text(L"Salva e volta para onde você estava.", L"Saves settings and returns to the game.");
    case IT_RESTART: return ui_text(L"Fecha e abre o jogo de novo com as opções marcadas com *.", L"Restarts the game to apply settings marked with *.");
    case IT_QUIT: return ui_text(L"Salva as configurações e fecha o jogo.", L"Saves settings and quits the game.");
    }
    return L"";
}

/* The game's Options look: an orange FreeStyle title on a white brush
 * frame, a dark see-through panel, white Euromode capitals; the chosen line
 * on a yellow paint band with "::", its value in a dark box between orange
 * arrows (as in Options > Gameplay); (A)/(B) hints under a white line. */
#define BG_ALPHA    0xC8000000u
#define PANEL_RGB   RGB(18, 30, 52)
#define PANEL_PIXEL 0x121E34u
#define PANEL_ALPHA 0xEC000000u
#define S1_ORANGE   RGB(243, 146, 30)
#define S1_YELLOW   RGB(226, 232, 48)
#define S1_WHITE    RGB(238, 238, 232)

static void render_menu(void)
{
    const int lh = s_lh, w = s_pw, h = s_ph;
    const int cw = w * 78 / 100 < lh * 40 ? w * 78 / 100 : lh * 40;
    const int x0 = (w - cw) / 2, x1 = x0 + cw;
    const int xm = x0 + cw * 44 / 100;                       /* labels end here */
    const int va0 = xm + lh * 3 / 2, va1 = x1 - lh * 2;      /* the value box */
    const int top = lh * 4, foot = h - lh * 2;
    int y, it, i, n = s_pw * s_ph, bx, pb;
    wchar_t sub[160];
    POINT p[4];
    SelectObject(s_dc, s_bmp);
    SetBkMode(s_dc, TRANSPARENT);
    fill(0, 0, w, h, BG_RGB);
    /* The rows first (to know where the panel ends), drawn over it after. */
    y = top + lh / 3;
    for (it = 0; it < IT_COUNT; it++) {
        if (it == IT_DISPLAY || it == IT_UI_LANG)
            y += lh * 3 / 4;
        if (it == IT_RESUME)
            y += lh / 3;
        if (it == IT_RESTART && !restart_pending())
            continue;
        y += lh;
    }
    pb = y + lh / 3;
    p[0].x = x0 - lh * 2; p[0].y = top;
    p[1].x = x1 + lh; p[1].y = top;
    p[2].x = x1 + lh; p[2].y = pb;
    p[3].x = x0 - lh; p[3].y = pb;
    poly(p, 4, PANEL_RGB);
    /* The white brush frame: down from the top left, under the title, then
     * along the panel's right side. */
    brush(x0 - lh * 5, -lh, x0 - lh * 3 / 2, top - lh / 2, lh / 2, S1_WHITE, 7u);
    brush(x0 - lh * 3 / 2, top - lh / 2, x1 + lh * 3 / 2, top - lh / 2, lh / 2, S1_WHITE, 11u);
    brush(x1 + lh * 3 / 2, top - lh / 2, x1 + lh * 2, top + lh / 2, lh / 2, S1_WHITE, 13u);
    brush(x1 + lh * 2, top + lh / 2, x1 + lh * 2, pb - lh, lh / 2, S1_WHITE, 17u);
    brush(x1 + lh * 2, pb - lh, x1 + lh * 3, pb + lh, lh / 2, S1_WHITE, 19u);
    /* Title, lower case as in the game ("options", "gameplay"). */
    text_sh(ui_text(L"BLACK — configurações", L"BLACK — PC Settings"), x0, 0, x1, top - lh * 3 / 4, S1_ORANGE, s_font_title, DT_LEFT);
    swprintf(sub, 160, ui_text(L"PC  •  jogo pausado", L"PC  •  game paused"));
    upper(sub);
    text_sh(sub, x0, top - lh * 2, x1, top - lh, RGB(175, 192, 215), s_font_small, DT_RIGHT);
    y = top + lh / 3;
    for (it = 0; it < IT_COUNT; it++) {
        wchar_t label[128], value[160], v[200];
        int greyed, sel = it == s_sel, ym, a = lh / 2;
        COLORREF lc;
        if (it == IT_DISPLAY || it == IT_UI_LANG) {
            text_sh(it == IT_DISPLAY ? ui_text(L"VÍDEO", L"VIDEO") : ui_text(L"SISTEMA", L"SYSTEM"), x0 - lh, y, xm, y + lh * 3 / 4, S1_ORANGE, s_font_small, DT_LEFT);
            y += lh * 3 / 4;
        }
        if (it == IT_RESUME)
            y += lh / 3;
        if (it == IT_RESTART && !restart_pending())
            continue;
        item_text(it, label, value, &greyed);
        upper(label);
        upper(value);
        ym = y + lh / 2;
        SetRect(&s_row[it], x0 - lh, y, x1 + lh, y + lh);
        SetRectEmpty(&s_row_left[it]);
        if (sel) {
            /* The yellow paint band, ragged at its left end, then "::". */
            int bl = xm - text_width(label, s_font_bold) - lh * 2;
            if (bl > x0 - lh) bl = x0 - lh;
            hramp(bl, bl + lh * 3, y + lh / 8, y + lh - lh / 8, PANEL_RGB, S1_YELLOW);
            fill(bl + lh * 3, y + lh / 8, xm + lh, y + lh - lh / 8, S1_YELLOW);
            for (i = 0; i < 4; i++)
                fill(xm + lh / 3 + (i & 1) * (lh / 6), ym - lh / 8 + (i >> 1) * (lh / 6), xm + lh / 3 + (i & 1) * (lh / 6) + lh / 10 + 1,
                     ym - lh / 8 + (i >> 1) * (lh / 6) + lh / 10 + 1, RGB(30, 30, 30));
            if (!value[0])                                   /* a line that acts: the (A) disc */
                button_hint(xm - text_width(label, s_font_bold) - lh * 6 / 5, y, lh, L'A', RGB(60, 170, 60), L"");
        }
        lc = sel ? RGB(20, 20, 20) : greyed ? RGB(120, 130, 145) : RGB(240, 240, 240);
        if (sel)
            text(label, x0 - lh, y, xm, y + lh, lc, s_font_bold, DT_RIGHT);
        else
            text_sh(label, x0 - lh, y, xm, y + lh, lc, s_font, DT_RIGHT);
        if (value[0]) {
            int arrows = sel && (it != IT_API || (hle_gpu_vulkan_available() && hle_gpu_d3d11_available()));
            swprintf(v, 200, L"%ls%ls", value, item_restart(it) ? L" *" : L"");
            if (sel) {
                fill(va0, y + lh / 8, va1, y + lh - lh / 8, RGB(40, 40, 42));
                if (arrows) {
                    arrow(va0 - a, ym, a, -1, S1_ORANGE);
                    arrow(va1 + a, ym, a, 1, S1_ORANGE);
                }
                text(v, va0 + lh / 2, y, va1 - lh / 2, y + lh, greyed ? RGB(150, 120, 90) : S1_ORANGE, s_font_bold, DT_CENTER);
            } else {
                text_sh(v, va0 + lh / 2, y, va1 - lh / 2, y + lh, greyed ? RGB(120, 130, 145) : RGB(240, 240, 240), s_font, DT_CENTER);
            }
            SetRect(&s_row_left[it], va0 - lh * 3 / 2, y, va0 + lh, y + lh);
        }
        y += lh;
    }
    /* Help for the selected line, then the white base line and the hints. */
    text_sh(item_help(s_sel), x0 - lh, pb + lh / 6, x1 + lh, pb + lh + lh / 6, RGB(205, 215, 230), s_font_small, DT_LEFT);
    if (restart_pending())
        text_sh(ui_text(L"* vale depois de reiniciar", L"* applies after restarting"), x0 - lh, foot - lh, x1 + lh, foot, RGB(255, 190, 110), s_font_small, DT_RIGHT);
    fill(0, foot - 1, x0 + cw / 2, foot + 1, S1_WHITE);
    p[0].x = x0 + cw / 2; p[0].y = foot - 1;
    p[1].x = x0 + cw / 2 + 2; p[1].y = foot - 1;
    p[2].x = x0 + cw / 2 + lh * 2 + 2; p[2].y = h;
    p[3].x = x0 + cw / 2 + lh * 2; p[3].y = h;
    poly(p, 4, S1_WHITE);
    bx = x0;
    SetRect(&s_hint_a, bx, foot + lh / 4, 0, h - lh / 4);
    bx = button_hint(bx, foot + lh / 4, lh, L'A', RGB(60, 170, 60), ui_text(L"Selecionar", L"Select"));
    s_hint_a.right = bx - lh;
    SetRect(&s_hint_b, bx, foot + lh / 4, 0, h - lh / 4);
    bx = button_hint(bx, foot + lh / 4, lh, L'B', RGB(200, 40, 40), ui_text(L"Voltar", L"Back"));
    s_hint_b.right = bx - lh;
    bx = x0 + cw / 2 + lh * 3;
    arrow(bx, foot + lh * 3 / 4, lh / 4, -1, S1_ORANGE);
    arrow(bx + lh * 3 / 4, foot + lh * 3 / 4, lh / 4, 1, S1_ORANGE);
    swprintf(sub, 160, ui_text(L"alterar      Alt+Enter  tela cheia      F1  fechar", L"change      Alt+Enter  fullscreen      F1  close"));
    upper(sub);
    text_sh(sub, bx + lh, foot + lh / 4, w, h - lh / 4, RGB(200, 208, 220), s_font_small, DT_LEFT);
    GdiFlush();
    for (i = 0; i < n; i++) {
        uint32_t c = s_bits[i] & 0xFFFFFFu;
        s_bits[i] = c | (c == BG_PIXEL ? BG_ALPHA : c == PANEL_PIXEL ? PANEL_ALPHA : 0xFF000000u);
    }
    ID3D11DeviceContext_UpdateSubresource(g_hle.ctx, (ID3D11Resource *)s_tex, 0, NULL, s_bits, (UINT)s_pw * 4u, 0);
}

void hle_options_request(const char *why)
{
    /* The front end also runs its screens' code while it loads; nothing a
     * player chose can happen in the first seconds after boot. */
    if (g_hle.swaps < 600) {
        fprintf(stderr, "[OPTIONS] %s at swap %llu: during boot, ignored\n", why, (unsigned long long)g_hle.swaps);
        return;
    }
    if (!s_open && !InterlockedExchange(&s_request, 1))
        fprintf(stderr, "[OPTIONS] opened by %s (swap %llu)\n", why, (unsigned long long)g_hle.swaps);
}

void hle_options_draw_overlay(ID3D11RenderTargetView *rtv, UINT w, UINT h)
{
    int lh;
    D3D11_VIEWPORT vp;
    if (!s_open || !rtv || w < 64 || h < 64 || !overlay_resources())
        return;
    lh = (int)h / 26;
    if (lh < 12) lh = 12;
    if ((int)w != s_pw || (int)h != s_ph || lh != s_lh || !s_srv)
        surface_resize((int)w, (int)h, lh);
    if (!s_srv || !s_bits)
        return;
    if (s_dirty) {
        render_menu();
        s_dirty = 0;
    }
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_hle.ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, s_blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_hle.ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_hle.ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(g_hle.ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_Draw(g_hle.ctx, 3, 0);
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &none);
    }
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 0, NULL, NULL);
}

/* ------------------------------------------------- the button on menus */

static HBITMAP s_btn_bmp;
static uint32_t *s_btn_bits;
static ID3D11Texture2D *s_btn_tex;
static ID3D11ShaderResourceView *s_btn_srv;
static ID3D11BlendState *s_blend_pre;
static int s_btn_w, s_btn_h, s_btn_language = -1;

/* The capsule and caption, drawn twice: white-on-black for coverage, then
 * in colour on black, which is the colour premultiplied by that coverage.
 * The caption is the game's Euromode capitals with their drop shadow, like
 * its own "(B) BACK" hint. */
static void button_paint(int w, int h, int mask)
{
    HFONT big = make_font(h * 56 / 100, FW_BOLD, 0), fsmall = make_font(h * 36 / 100, FW_BOLD, 0);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    RECT r;
    int cap = h * 17 / 10, o = h / 16 + 1;
    SetRect(&r, 0, 0, w, h);
    FillRect(s_dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(s_dc, white);
    SelectObject(s_dc, pen);
    RoundRect(s_dc, 1, h / 5, cap, h - h / 5, h / 2, h / 2);
    SetBkMode(s_dc, TRANSPARENT);
    text(L"BACK", 1, h / 5, cap, h - h / 5, mask ? RGB(255, 255, 255) : RGB(20, 20, 20), fsmall, DT_CENTER);
    text(ui_text(L"CONFIGURAÇÕES DO PC", L"PC SETTINGS"), cap + h / 3 + o, o, w, h + o, mask ? RGB(255, 255, 255) : RGB(6, 10, 18), big, DT_LEFT);
    text(ui_text(L"CONFIGURAÇÕES DO PC", L"PC SETTINGS"), cap + h / 3, 0, w, h, RGB(240, 240, 240), big, DT_LEFT);
    DeleteObject(white);
    DeleteObject(pen);
    DeleteObject(big);
    DeleteObject(fsmall);
    GdiFlush();
}

static int button_resources(int h)
{
    BITMAPINFO bi;
    D3D11_TEXTURE2D_DESC td;
    uint32_t *mask;
    int w = h * 12, i;
    if (s_btn_srv && s_btn_h == h && s_btn_language == g_opt.ui_language)
        return 1;
    if (s_btn_srv) { ID3D11ShaderResourceView_Release(s_btn_srv); s_btn_srv = NULL; }
    if (s_btn_tex) { ID3D11Texture2D_Release(s_btn_tex); s_btn_tex = NULL; }
    if (s_btn_bmp) { DeleteObject(s_btn_bmp); s_btn_bmp = NULL; }
    if (!s_blend_pre) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(g_hle.dev, &bd, &s_blend_pre)))
            return 0;
    }
    if (!s_dc)
        s_dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_btn_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_btn_bits, NULL, 0);
    if (!s_btn_bmp)
        return 0;
    SelectObject(s_dc, s_btn_bmp);
    mask = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!mask)
        return 0;
    button_paint(w, h, 1);
    memcpy(mask, s_btn_bits, (size_t)w * (size_t)h * 4u);
    button_paint(w, h, 0);
    for (i = 0; i < w * h; i++) {
        uint32_t m = (mask[i] >> 8) & 0xFFu;                  /* coverage (green) */
        s_btn_bits[i] = (s_btn_bits[i] & 0xFFFFFFu) | (m << 24);
    }
    free(mask);
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    {
        D3D11_SUBRESOURCE_DATA sd;
        sd.pSysMem = s_btn_bits;
        sd.SysMemPitch = (UINT)w * 4u;
        sd.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, &sd, &s_btn_tex)))
            return 0;
    }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)s_btn_tex, NULL, &s_btn_srv)))
        return 0;
    if (s_bmp)
        SelectObject(s_dc, s_bmp);                            /* the screen's DIB back */
    s_btn_w = w;
    s_btn_h = h;
    s_btn_language = g_opt.ui_language;
    return 1;
}

void hle_options_draw_button(ID3D11RenderTargetView *rtv)
{
    extern D3D11_VIEWPORT g_present_picture;
    D3D11_VIEWPORT pic = g_present_picture, vp;
    float w43, x43;
    int h;
    if (s_open || !rtv || !screen_has_button() || pic.Height < 120.0f || !overlay_resources())
        return;
    h = (int)(pic.Height * 0.06f + 0.5f);
    if (h < 12)
        h = 12;
    if (!button_resources(h))
        return;
    /* In the 4:3 picture's coordinates, on the line of the game's own
     * button hints (y 0.93), right of where they sit. */
    w43 = pic.Height * 4.0f / 3.0f;
    if (w43 > pic.Width)
        w43 = pic.Width;
    x43 = pic.TopLeftX + (pic.Width - w43) * 0.5f;
    vp.TopLeftX = (float)(int)(x43 + w43 * 0.30f);
    vp.TopLeftY = (float)(int)(pic.TopLeftY + pic.Height * 0.933f - (float)h * 0.5f);
    vp.Width = (float)s_btn_w;
    vp.Height = (float)s_btn_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_hle.ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, s_blend_pre, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_hle.ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_hle.ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &s_btn_srv);
    ID3D11DeviceContext_PSSetSamplers(g_hle.ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_Draw(g_hle.ctx, 3, 0);
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 1, &none);
    }
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 0, NULL, NULL);
}

void hle_options_standalone(void) { menu_loop(); }
