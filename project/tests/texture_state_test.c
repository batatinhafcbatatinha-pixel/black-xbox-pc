/* Standalone regression test; no game assets or renderer needed.
 * Compile with src/native_d3d/black_texture_state.c and include paths
 * src, src/recomp, src/recomp/gen.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "native_d3d/black_native.h"

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_eax, g_esp;
static uint32_t called, argc_seen, args_seen[3];

uint32_t black_guest_call(uint32_t va, int count, const uint32_t *args)
{
    called = va;
    argc_seen = count;
    memcpy(args_seen, args, count * sizeof(*args));
    return 0;
}

static void set(uint32_t stage, uint32_t type, uint32_t value)
{
    g_esp = 0x1000;
    MEM32(g_esp) = 0x12345678;
    MEM32(g_esp + 4) = stage;
    MEM32(g_esp + 8) = type;
    MEM32(g_esp + 12) = value;
    called = 0;
    sub_0026E860();
    assert(g_esp == 0x1010);
    assert(MEM32(0x1000) == 0x12345678);
}

int main(void)
{
    void *mem = calloc(1, 0x300000);
    assert(mem);
    g_xbox_mem_offset = (ptrdiff_t)mem;
    /* COLORARG1 must not overwrite COLOROP: this failed in the split AOT. */
    MEM32(0x279978) = 4;
    MEM32(0x28BA18 + 4 * 14) = 0x800;
    set(0, 14, 2);
    assert(MEM32(0x279978) == 4);
    assert(MEM32(0x279980) == 2);
    assert(MEM32(0x27C000) == 0x800);
    /* All deferred slots and stages are independent. */
    for (uint32_t stage = 0; stage < 4; ++stage) {
        for (uint32_t type = 0; type < 22; ++type) {
            uint32_t address = 0x279948 + 128 * stage + 4 * type;
            set(stage, type, 0x100 + type);
            assert(MEM32(address) == 0x100 + type);
            assert(!called);
        }
    }
    MEM32(0x27C000) = 0;
    set(3, 0, 3);
    assert(MEM32(0x27C000) == 8);
    MEM32(0x27C000) = 0;
    set(0, 12, 24);
    assert(MEM32(0x27C000) == 0x800);
    MEM32(0x27C000) = 0;
    set(0, 12, 25);
    assert(MEM32(0x27C000) == 0x480F);
    for (uint32_t type = 22; type <= 30; ++type) {
        static const uint32_t special[] = {0x26E640, 0x26E7C0, 0x26E810};
        set(2, type, 0xABCDEF);
        assert(called == (type < 28 ? 0x26E750 : special[type - 28]));
        assert(argc_seen == (type < 28 ? 3 : 2));
        assert(args_seen[0] == 2);
        assert(args_seen[argc_seen - 1] == 0xABCDEF);
        if (type < 28) assert(args_seen[1] == type);
    }
    set(0, 31, 0); assert(!called);
    set(4, 12, 0); assert(!called);
    set(0, 32, 0); assert(!called);
    free(mem);
    puts("texture-state regression: passed");
    return 0;
}
