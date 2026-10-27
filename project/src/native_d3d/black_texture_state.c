/* Black XDK 5849 D3DDevice_SetTextureStageStateNotInline (0x26E860).
 * The recovered block at 0x26E897 starts with JNE and loses the flags from
 * CMP type,12 in its caller. Consequently every type >= 12 was treated as
 * COLOROP, leaving COLORARG/ALPHAARG at zero. Keep this dispatch together.
 * Layout/dirty masks follow recomp_0104.c and recomp_recovered_funcs.c.
 */
#include "black_native.h"

void sub_0026E860(void)
{
    uint32_t stage = black_arg(0), type = black_arg(1), value = black_arg(2);
    uint32_t dirty;
    if (stage >= 4 || type >= 32) {
        g_esp += 16;
        return;
    }
    if (type < 22) {
        if (type < 12)
            dirty = 1u << stage;
        else if (type == 12)
            dirty = value < 25 ? 0x800u : 0x480Fu;
        else
            dirty = MEM32(0x28BA18u + 4u * type);
        MEM32(0x27C000u) |= dirty;
        MEM32(0x279948u + 128u * stage + 4u * type) = value;
    } else if (type <= 27) {
        uint32_t args[] = {stage, type, value};
        black_guest_call(0x26E750u, 3, args);
    } else if (type <= 30) {
        static const uint32_t setter[] = {0x26E640u, 0x26E7C0u, 0x26E810u};
        uint32_t args[] = {stage, value};
        black_guest_call(setter[type - 28], 2, args);
    }
    g_esp += 16; /* stdcall: return address + three arguments */
}
