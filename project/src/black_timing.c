/* Black USA animation clock, 0x82210.
 * The shipped routine takes the low 32 bits of RDTSC, subtracts its last
 * sample (0x2C02A4), divides by 733333333 and truncates to milliseconds.
 * Losing a fraction on EVERY update slows animations at unlocked rates.
 * Retain the original routine/ABI, but carry the fractional ticks between
 * calls. No wait, FPS cap, clock acceleration or frame-count-based tick.
 */
#include "recomp_types.h"
#include <stdint.h>

void black_animation_clock_original(void);

void sub_00082210(void)
{
    static uint64_t fraction;
    uint32_t previous_ticks = MEM32(0x2C02A4u);
    uint32_t previous_ms = MEM32(0x2C02A0u);
    /* Original code preserves ECX/ESI, updates the sample, and returns with
     * the guest stack/FP stack in their proper state. */
    black_animation_clock_original();
    uint32_t elapsed_ticks = MEM32(0x2C02A4u) - previous_ticks;
    uint64_t numerator = fraction + (uint64_t)elapsed_ticks * 1000u;
    uint32_t elapsed_ms = (uint32_t)(numerator / 733333333u);
    fraction = numerator % 733333333u;
    MEM32(0x2C02A0u) = previous_ms + elapsed_ms;
    g_eax = elapsed_ms;
}
