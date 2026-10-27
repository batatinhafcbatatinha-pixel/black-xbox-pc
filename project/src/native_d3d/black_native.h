/* Native replacements of Black's D3D8 (XDK 5849, LTCG) hardware layer.
 *
 * The translated library keeps its CPU-side work (device structure, pushbuffer ring, fences, resources); what it
 * did with the NV2A -- register writes, instance memory, FIFO user registers, interrupts -- is replaced here, as in
 * FIFA Street 1's NO_HW mode (street1/project/src/hle_device.c, hle_sync.c). The structures have the same layout
 * in both titles (same library source); only the addresses and, with LTCG, the register conventions differ.
 */
#pragma once
#include <stdint.h>
#include "recomp/recomp_types.h"
#include "gen/recomp_funcs.h"

#define BLACK_DEVICE      0x279B50u            /* the static CDevice (Street 1 0x261240) */
#define BLACK_MINIPORT    (BLACK_DEVICE + 0x1C28u)   /* g_Miniport inside the device (Street 1 0x262E68) */
#define BLACK_PDEVICE     0x27BFF8u            /* D3D_g_pDevice */
#define BLACK_REGISTERS   0xFD000000u          /* kept as a value: nothing native dereferences it */

#define DEV32(off)  MEM32(BLACK_DEVICE + (off))
#define MP32(off)   MEM32(BLACK_MINIPORT + (off))

/* Stack argument i of the current (not yet returned) guest call: [esp] is the return address. */
static inline uint32_t black_arg(int i) { return MEM32(g_esp + 4u + 4u * (uint32_t)i); }

/* Return from a guest call: eax = value, pop the return address and `args` stack dwords (ret 4*args). */
static inline void black_ret(uint32_t args, uint32_t value)
{
    g_eax = value;
    g_esp += 4u + 4u * args;
}

/* Call guest code (translated function or kernel thunk target) with stack arguments, as a call instruction
 * would; the callee pops what its convention pops. Returns eax. */
uint32_t black_guest_call(uint32_t va, int count, const uint32_t *args);

/* The game's contiguous allocator used by the library (stdcall size, flags) -- Street 1 0x1609A7. */
uint32_t black_alloc(uint32_t bytes, uint32_t flags);
