#pragma once
#include <stdint.h>
extern uint32_t timing_sample, timing_ms, g_eax;
#define MEM32(a) (*((a) == 0x2C02A4u ? &timing_sample : &timing_ms))
