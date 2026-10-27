/*
 * Direct3D_CreateDevice without the NV2A: the miniport's hardware bring-up.
 *
 * CDevice::Init (0x2718F0) and CMiniport::InitHardware (0x272F35) are CPU work plus a hardware layer that
 * programs the GPU, reads its configuration and builds instance-memory objects (DMA contexts, graphics classes,
 * the FIFO channel). With the native renderer nothing executes the pushbuffer, so only the CPU-side state the
 * bring-up leaves behind matters. The hardware layer, with its Street 1 equivalent (hle_device.c):
 *
 *   0x273095  register base, interrupt enables      (0x259D01)  this = eax
 *   0x2730C0  PCI id, revision, memory size         (0x259D2C)  this = eax
 *   0x273103  clocks, instance memory layout        (0x259D6D)  this = eax
 *   0x273184  PGRAPH/PFIFO initialisation           (0x259DF0)  this = edi
 *   0x2732A4  DMA context object                    (0x259EE2)  this = eax, out = esi, 4 stack args
 *   0x27336D  FIFO channel 0                        (0x259C6A)  this = edx, 2 stack args
 *   0x275A71  Kelvin graphics context               (0x259FB1 for class 0x97, instance part)  this = ecx
 *
 * The values are the ones the original computes (Street 1 RECOMP_HLE_DEVLOG, same XDK); the miniport layout is
 * the same in both titles.
 */
#include <stdio.h>
#include <string.h>
#include "black_native.h"

uint32_t black_guest_call(uint32_t va, int count, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        fprintf(stderr, "Black native: no code at guest call target %08X\n", va);
        abort();
    }
    for (int i = count - 1; i >= 0; --i)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0u);                 /* return address: the callee only pops it */
    fn();
    return g_eax;
}

uint32_t black_alloc(uint32_t bytes, uint32_t flags)
{
    uint32_t a[2] = { bytes, flags };
    return black_guest_call(0x00025AB1u, 2, a);
}

/* Register base: kept as a value so that any leftover hardware access faults instead of landing in RAM. */
void sub_00273095(void)
{
    MEM32(g_eax) = BLACK_REGISTERS;
    black_ret(0, 1);
}

/* PCI id/revision (+0xA8, +0xC0) and memory size (+0xAC) stay 0; only the constant at +0xBC is set. */
void sub_002730C0(void)
{
    MEM32(g_eax + 0xBCu) = 0xFE502Au;
    black_ret(0, 1);
}

/* Clock PLLs, instance memory layout, gamma ramps. The instance memory itself is still claimed from the kernel
 * (MmClaimGpuInstanceMemory, thunk 0x283184), as the original does, so the memory map is the same. */
void sub_00273103(void)
{
    uint32_t mp = g_eax, pad, top;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
    g_esp -= 4;
    MEM32(g_esp) = 0;
    {
        uint32_t a[2] = { 0x5000u, g_esp };
        top = black_guest_call(MEM32(0x283184u), 2, a);
    }
    pad = MEM32(g_esp);
    g_esp += 4;
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
    MEM32(mp + 0x04 * 4) = top - 0x5000u;      /* instance memory base (VA) */
    MEM32(mp + 0x2C * 4) = 3;
    MEM32(mp + 0x2D * 4) = 1;
    MEM32(mp + 0x36 * 4) = 1;                  /* MPLL/VPLL/NVPLL dividers */
    MEM32(mp + 0x3A * 4) = 1;
    MEM32(mp + 0x3E * 4) = 1;
    MEM32(mp + 0x47 * 4) = 0x01111111u;
    MEM32(mp + 0x48 * 4) = 0xFF;
    MEM32(mp + 0x49 * 4) = 0x800000u;
    MEM32(mp + 0x4C * 4) = 0x700000u + pad;    /* RAMIN layout */
    MEM32(mp + 0x4A * 4) = MEM32(mp + 0x4C * 4) + 0x1000u;
    MEM32(mp + 0x4B * 4) = MEM32(mp + 0x4A * 4) + 0x80u;
    MEM32(mp + 0x4D * 4) = 2;
    MEM32(mp + 0x50 * 4) = 0x10A;
    MEM32(mp + 0x58 * 4) = 0x112;              /* next free instance block */
    for (uint32_t i = 0; i < 0x300; i++)       /* identity gamma ramps R, G, B */
        MEM8(mp + 0x1DC + i) = (uint8_t)i;
    MEM32(mp + 0xB4) = 1;                      /* written by the original between its register writes */
    MEM32(mp + 0x134) = 2;
    black_ret(0, 1);
}

/* PGRAPH/PFIFO initialisation: the engine defaults kept in the miniport. */
void sub_00273184(void)
{
    uint32_t mp = g_edi;
    MEM32(mp + 0x1FC * 4) = 0x00118700u;
    MEM32(mp + 0x1FD * 4) = 0x0028C3FFu;
    MEM32(mp + 0x1FE * 4) = 0xF3DE0479u;
    MEM32(mp + 0x200 * 4) = 4;
    MEM32(mp + 0x201 * 4) = 0x45EAD10Eu;
    MEM32(mp + 0x203 * 4) = 0x78;
    MEM32(mp + 0x204 * 4) = 0x40;
    MEM32(mp + 0x208 * 4) = 0;
    MEM32(mp + 0x209 * 4) = 0;
    MEM32(mp + 0x210 * 4) = 0xFFFFFFFFu;
    MEM32(mp + 0x211 * 4) = 0xFFFFFFFFu;
    black_ret(0, 1);
}

/* DMA context (handle, class, base, limit; out object in esi): an instance number, no instance memory. */
void sub_002732A4(void)
{
    uint32_t mp = g_eax, out = g_esi, inst = MEM32(g_eax + 0x160);
    MEM32(mp + 0x160) = inst + 1;
    MEM32(out + 0) = black_arg(0);
    MEM32(out + 4) = 0;
    MEM32(out + 8) = black_arg(1);
    MEM32(out + 12) = inst;
    black_ret(4, 1);
}

/* FIFO channel 0. The out pointer (second stack argument) is where D3D finds the channel's user registers
 * (+0x40 PUT, +0x44 GET): a block of ordinary guest memory, read and written only by the native KickOff and
 * the library's own GET reads. The miniport fields are the CPU part of 0x27336D and 0x2758D7. */
void sub_0027336D(void)
{
    uint32_t mp = g_edx, out = black_arg(1), first = MEM32(g_edx + 0x160), block;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, ecx = g_ecx, edx = g_edx;
    MEM32(mp + 0x100) = 0;                     /* channel 0 */
    MEM32(mp + 0x104) = 0;
    MEM32(mp + 0x110) = 0;
    MEM32(mp + 0x118) = 0;
    MEM32(mp + 0x160) = first + 0x37F;
    MEM32(mp + 0x10C) = first;
    MEM32(mp + 0x138) = first;                 /* context instance of channel 0 */
    MEM32(mp + 0x104) |= 1u;
    MEM32(mp + 0x108) |= 1u;
    block = black_alloc(0x100, 0xAC800000u);
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp; g_ecx = ecx; g_edx = edx;
    if (block)
        memset((void *)XBOX_PTR(block), 0, 0x100);
    MEM32(out) = block;
    fprintf(stderr, "[BLACK-DEV] FIFO channel 0 user registers at %08X (guest memory)\n", block);
    black_ret(2, 1);
}

/* Kelvin graphics context: only instance memory, which does not exist without the NV2A. */
void sub_00275A71(void)
{
    black_ret(0, g_eax);
}

/* CMiniport::SetTile (this and 6 more on the stack, index in eax): PFB/PGRAPH tile registers. The CPU sees linear
 * memory either way; the library's tile table at device+0x1AC0 is kept by the caller, D3D_SetTileNoWait. */
void sub_0027357F(void)
{
    black_ret(7, 1);
}
