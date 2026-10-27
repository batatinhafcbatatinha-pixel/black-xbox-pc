/* Test the production wrapper against a deliberately truncating original.
 * Elapsed time must be invariant across frame rates and a low-TSC wrap. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
uint32_t timing_sample, timing_ms, g_eax;
static uint32_t next_ticks;
void black_animation_clock_original(void) {
    uint32_t ticks = next_ticks - timing_sample;
    timing_sample = next_ticks;
    g_eax = (uint32_t)((uint64_t)ticks * 1000 / 733333333u);
    timing_ms += g_eax;
}
void sub_00082210(void);
static void check(unsigned frames, uint32_t start) {
    timing_sample = start; timing_ms = 0;
    for (unsigned i = 1; i <= frames; ++i) {
        next_ticks = start + (uint32_t)((uint64_t)733333333u * i / frames);
        sub_00082210();
    }
    assert(timing_ms == 1000);
    next_ticks = timing_sample;
    sub_00082210();
    assert(timing_ms == 1000 && g_eax == 0);
}
int main(void) {
    check(15, 0); check(60, 0); check(1000, 0); check(1000, 0xFFFFFFF0u);
    puts("PASS: 15/60/1000 updates per second, zero delta, 32-bit TSC wrap");
    return 0;
}
