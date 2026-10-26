/* Native equivalents for output and sampling state that used to be lost
 * when CPU-only D3D8 setters replaced the library's GPU method writes. */
#include "hle_d3d11_internal.h"
#include "../recomp/recomp_types.h"
#include "hle_options.h"
#include <limits.h>
#include <string.h>
#ifndef D3D11_MIP_LOD_BIAS_MIN
#define D3D11_MIP_LOD_BIAS_MIN (-16.0f)
#define D3D11_MIP_LOD_BIAS_MAX 15.99f
#endif

#define QRS(i) MEM32(0x27C008u + 4u * (i))
#define QTSS(stage, i) MEM32(0x279948u + 128u * (stage) + 4u * (i))

static uint64_t descriptor_key(const void *value, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)value;
    uint64_t key = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < bytes; ++i)
        key = (key ^ p[i]) * 0x100000001B3ull;
    return key;
}

static D3D11_TEXTURE_ADDRESS_MODE texture_address(uint32_t mode)
{
    switch (mode & 7u) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

/* Descriptor is zero-initialized so cache keys include every meaningful
 * input, including border colour and floating-point LOD bias. */
uint64_t hle_quality_sampler(uint32_t stage, D3D11_SAMPLER_DESC *out)
{
    uint32_t mag = QTSS(stage, 3u) & 7u, min = QTSS(stage, 4u) & 7u;
    uint32_t mip = QTSS(stage, 5u) & 7u, anisotropy = QTSS(stage, 8u);
    uint32_t border = QTSS(stage, 29u);
    memset(out, 0, sizeof *out);
    out->Filter = (D3D11_FILTER)((min >= 2u ? 0x10u : 0u) |
                                  (mag >= 2u ? 4u : 0u) | (mip >= 2u ? 1u : 0u));
    if ((min == 3u || mag == 3u) && anisotropy > 1u)
        out->Filter = D3D11_FILTER_ANISOTROPIC;
    out->AddressU = texture_address(QTSS(stage, 0u));
    out->AddressV = texture_address(QTSS(stage, 1u));
    out->AddressW = texture_address(QTSS(stage, 2u));
    memcpy(&out->MipLODBias, (const void *)XBOX_PTR(0x279948u + 128u * stage + 24u), 4);
    if (out->MipLODBias < D3D11_MIP_LOD_BIAS_MIN) out->MipLODBias = D3D11_MIP_LOD_BIAS_MIN;
    if (out->MipLODBias > D3D11_MIP_LOD_BIAS_MAX) out->MipLODBias = D3D11_MIP_LOD_BIAS_MAX;
    out->MaxAnisotropy = anisotropy < 1u ? 1u : anisotropy > 16u ? 16u : anisotropy;
    /* Options: forced anisotropy on every filtered, mipmapped sampler. */
    if (g_opt.aniso > 1 && min >= 2u && mip && (unsigned)g_opt.aniso > out->MaxAnisotropy) {
        out->Filter = D3D11_FILTER_ANISOTROPIC;
        out->MaxAnisotropy = (UINT)g_opt.aniso;
    }
    out->ComparisonFunc = D3D11_COMPARISON_NEVER;
    out->BorderColor[0] = ((border >> 16) & 255u) / 255.0f;
    out->BorderColor[1] = ((border >> 8) & 255u) / 255.0f;
    out->BorderColor[2] = (border & 255u) / 255.0f;
    out->BorderColor[3] = (border >> 24) / 255.0f;
    out->MinLOD = mip ? (float)QTSS(stage, 7u) : 0.0f;
    out->MaxLOD = mip ? D3D11_FLOAT32_MAX : 0.0f;
    return descriptor_key(out, sizeof *out);
}

uint64_t hle_quality_raster(int has_depth, D3D11_RASTERIZER_DESC *out)
{
    uint32_t fill = QRS(139u), cull = QRS(147u), front = QRS(146u);
    uint32_t zbias = QRS(149u);
    memset(out, 0, sizeof *out);
    out->FillMode = fill == 0x1B01u || fill == 0x1B00u ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    out->CullMode = cull == 0 ? D3D11_CULL_NONE : cull == front ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
    out->FrontCounterClockwise = front == 0x901u;
    out->DepthClipEnable = has_depth && ((QRS(0x52u) >> 4) & 15u) != 1u;
    if (QRS(0x4Fu) || QRS(0x50u) || QRS(0x51u)) {
        out->DepthBias = zbias > INT_MAX ? INT_MIN : -(int)zbias;
        out->SlopeScaledDepthBias = MEMF(0x27C008u + 0x4Du * 4u);
    }
    out->MultisampleEnable = QRS(152u) != 0;
    out->AntialiasedLineEnable = QRS(151u) != 0;
    return descriptor_key(out, sizeof *out);
}

/* Simple render states use this table both in 0x28D7F0 and in NV2A.
 * Call on mutation rather than every draw so explicit BeginPush methods
 * remain authoritative until the corresponding CPU state is changed. */
void hle_quality_simple_state(uint32_t *nv, uint32_t state, uint32_t value)
{
    if (state < 0x5Cu) {
        uint32_t method = MEM32(0x2AE4F8u + 4u * state) & 0x1FFCu;
        nv[method >> 2] = value;
    }
}

/* These flags name real remaining differences; enabling their CPU setters
 * alone does not implement their GPU behavior. No rendering is suppressed. */
uint32_t hle_quality_gaps(uint32_t primitive)
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR_VA), gaps = 0;
    if ((MEM32(dev + 8u) & 0x8000u) && QRS(152u)) gaps |= 1u; /* sample coverage */
    if (primitive >= 2u && primitive <= 4u && MEMF(0x29E944u) != 1.0f)
        gaps |= 2u; /* wide lines require expanded geometry */
    if (QRS(139u) == 0x1B00u) gaps |= 4u; /* point polygon mode */
    if (QRS(141u) && QRS(140u) != QRS(139u)) gaps |= 8u; /* distinct back fill */
    if (QRS(141u)) gaps |= 16u; /* two-sided lighting */
    if (QRS(150u) && QRS(150u) != 0x1503u) gaps |= 32u; /* logic op other than copy */
    if (QRS(158u)) gaps |= 1024u; /* alpha-to-coverage/alpha-to-one */
    if (MEM32(dev + 0x784u)) {
        uint32_t modes = MEM32(dev + 0x790u);
        for (uint32_t stage = 0; stage < 4u; ++stage) {
            uint32_t mode = (modes >> (5u * stage)) & 31u;
            if (mode == 2u || mode == 3u || (mode >= 8u && mode <= 14u) || mode >= 17u)
                gaps |= 2048u; /* 3D/cube and dependent dot texture shader modes */
        }
    }
    for (uint32_t stage = 0; stage < 4u; ++stage) {
        if (QTSS(stage, 3u) >= 4u || QTSS(stage, 4u) >= 4u) gaps |= 128u; /* convolution filter */
        if (QTSS(stage, 10u)) gaps |= 256u; /* signed filtering needs SNORM source textures */
    }
    return gaps;
}

void hle_quality_texgen(float worldview[16], float normalview[16],
                        uint32_t modes[4], float textransform[4][16])
{
    uint32_t dev = MEM32(D3D_DEVICE_PTR_VA), shader = MEM32(dev + DEV_VERTEX_SHADER);
    int active = !(MEM8(shader + 4u) & 0x12u) && MEM32(dev + 0x1928u) != 1u;
    for (uint32_t i = 0; i < 4u; ++i)
        for (uint32_t j = 0; j < 4u; ++j)
            worldview[i * 4u + j] = MEMF(dev + 0xDE0u + i * 16u) * MEMF(dev + 0xC60u + j * 4u) +
                MEMF(dev + 0xDE4u + i * 16u) * MEMF(dev + 0xC70u + j * 4u) +
                MEMF(dev + 0xDE8u + i * 16u) * MEMF(dev + 0xC80u + j * 4u) +
                MEMF(dev + 0xDECu + i * 16u) * MEMF(dev + 0xC90u + j * 4u);
    memset(normalview, 0, 16u * sizeof(float));
    float a = worldview[0], b = worldview[1], c = worldview[2];
    float d = worldview[4], e = worldview[5], f = worldview[6];
    float g = worldview[8], h = worldview[9], i = worldview[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (det != 0.0f) {
        normalview[0] = (e * i - f * h) / det;
        normalview[1] = (f * g - d * i) / det;
        normalview[2] = (d * h - e * g) / det;
        normalview[4] = (c * h - b * i) / det;
        normalview[5] = (a * i - c * g) / det;
        normalview[6] = (b * g - a * h) / det;
        normalview[8] = (b * f - c * e) / det;
        normalview[9] = (c * d - a * f) / det;
        normalview[10] = (a * e - b * d) / det;
    }
    normalview[15] = 1.0f;
    for (uint32_t stage = 0; stage < 4u; ++stage) {
        uint32_t tci = QTSS(stage, 28u), flags = QTSS(stage, 21u);
        modes[stage] = (active ? 0x80000000u : 0u) | (tci & 0xFFFF0003u) |
            ((flags & 0x1FFu) << 4) | (QRS(142u) ? 8u : 0u);
        for (uint32_t k = 0; k < 16u; ++k)
            textransform[stage][k] = MEMF(dev + 0xCE0u + stage * 64u + k * 4u);
    }
}
