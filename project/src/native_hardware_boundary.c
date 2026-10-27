/* The native host never dispatches an emulated NV2A command stream.
 * Keep a fatal boundary if the kernel's disabled hardware path is reached. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static void unsupported(const char *name) {
    fprintf(stderr, "Black native host: unsupported hardware path %s\n", name);
    abort();
}
void nv2a_pb_scan(uint32_t start, uint32_t end) {
    (void)start; (void)end; unsupported("nv2a_pb_scan");
}
void nv2a_pb_scan_report(void) { unsupported("nv2a_pb_scan_report"); }
uint32_t nv2a_pb_run(uint32_t start, uint32_t end) {
    (void)start; (void)end; unsupported("nv2a_pb_run"); return 0;
}
void xbox_OhciReport(void) { fprintf(stderr, "Black native host: no OHCI controller attached\n"); }
void xbox_OhciServicePendingInterrupts(void) {
    /* No OHCI device is attached in SetNoHardware mode, hence no queued
     * completions to deliver. This does not synthesize guest USB interrupts
     * or claim that a controller is connected. Guest MMIO stays unmapped. */
    static LONG reported;
    if (InterlockedCompareExchange(&reported, 1, 0) == 0)
        fprintf(stderr, "Black native host: OHCI queue empty; native input not integrated\n");
}
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int count) {
    char title[512];
    int length = 0;
    while (length < count && length < 127 && name[length]) ++length;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, (const WCHAR *)name,
                                   length, title, sizeof(title)-1, NULL, NULL);
    title[bytes > 0 ? bytes : 0] = 0;
    fprintf(stderr, "Black native host title: %s\n", title);
}
/* The scanout window belongs to the emulated framebuffer path; natively the D3D11 renderer presents at Swap, so
 * the kernel's display-mode notifications have nothing to drive. */
void xbox_FramebufferWindowSet(uint32_t address, uint32_t pitch) { (void)address; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_VideoIsPlaying(void) { unsupported("xbox_VideoIsPlaying"); return 0; }
int xbox_VideoPlayFile(const char *path) { (void)path; unsupported("xbox_VideoPlayFile"); return 0; }

/* Keyboard state from the native renderer's window, for pad 0 (xboxrecomp usb_gamepad.c / xinput_device.c). */
static volatile unsigned char s_key_down[256];
void black_key_event(int key, int down)
{
    if (key < 0) { memset((void *)s_key_down, 0, sizeof s_key_down); return; }   /* focus lost */
    if (key < 256) s_key_down[key] = (unsigned char)(down != 0);
}
int xbox_FramebufferKeyDown(int vk) { return (unsigned)vk <= 255 && s_key_down[vk] != 0; }
