// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
#ifndef HLE_VSH_H
#define HLE_VSH_H
#include <stddef.h>
#include <stdint.h>

/* Xbox vertex shader microcode (count instructions, 4 dwords each) -> HLSL
 * vs_5_0 source. *clip_space tells whether the XDK screen-space suffix was
 * found (position is clip space) or the shader writes screen space itself.
 * Returns the source length, or 0 with a message in error. */
size_t hle_vsh_hlsl(const uint32_t (*program)[4], unsigned count,
                    char *out, size_t cap, int *clip_space, char *error, size_t ecap);

#endif
