// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/* The last step of a frame: the game's picture onto the window's back buffer.
 *
 * The picture keeps its shape (black bars when the window's proportions
 * differ) and is filtered to the window's size, through the game's gamma
 * ramp (D3DDevice_SetGammaRamp; identity until the game sets one) and, with
 * the FXAA option, an FXAA pass in the same draw. */
#include "hle_d3d11_internal.h"
#include "hle_gpu_api.h"
#include "../recomp/recomp_types.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>

static uint8_t ramp[256][4];
static int gamma_dirty = 1, ramp_set;
static ID3D11VertexShader *present_vs;
static ID3D11PixelShader *present_ps, *present_ps_fxaa;
static ID3D11Texture1D *gamma_tex;
static ID3D11ShaderResourceView *gamma_srv;
static ID3D11SamplerState *present_sampler;
/* Where the last frame's picture landed in the back buffer (the options
 * draw the menu button relative to it). */
D3D11_VIEWPORT g_present_picture;

void hle_native_gamma_changed(uint32_t table)
{
    EnterCriticalSection(&g_hle.lock);
    for (uint32_t i = 0; i < 256; i++) {
        for (uint32_t c = 0; c < 3; c++)
            ramp[i][c] = MEM8(table + c * 256 + i);
        ramp[i][3] = 255;
    }
    ramp_set = 1;
    gamma_dirty = 1;
    LeaveCriticalSection(&g_hle.lock);
}

static ID3DBlob *compile(const char *src, size_t len, const char *entry, const char *profile)
{
    ID3DBlob *code = NULL, *error = NULL;
    HRESULT hr = hle_d3d_compile(src, len, "present", NULL, NULL, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) {
        fprintf(stderr, "[HLE-PRESENT] %s: %s\n", entry, (const char *)ID3D10Blob_GetBufferPointer(error));
        ID3D10Blob_Release(error);
    }
    return SUCCEEDED(hr) ? code : NULL;
}

static int present_shaders(void)
{
    static const char source[] =
        "Texture2D<float4> pic:register(t0); Texture1D<float4> ramp:register(t1);"
        "SamplerState lin:register(s0);"
        "struct V{float4 p:SV_Position; float2 uv:TEXCOORD0;};"
        "V vs(uint id:SV_VertexID){V o; float2 q=float2(id==1?2:0,id==2?2:0);"
        "o.p=float4(q.x*2-1,1-q.y*2,0,1); o.uv=q; return o;}"
        "float3 S(float2 uv){return pic.SampleLevel(lin,uv,0).rgb;}"
        "float3 grade(float3 c){uint3 i=(uint3)round(saturate(c)*255);"
        "return float3(ramp.Load(int2(i.r,0)).r,ramp.Load(int2(i.g,0)).g,ramp.Load(int2(i.b,0)).b);}"
        "float4 ps(V i):SV_Target{return float4(grade(S(i.uv)),1);}"
        /* FXAA (Lottes' original console variant): blend along the edge
         * direction, only where the result stays within the local range. */
        "float4 ps_fxaa(V i):SV_Target{"
        "float w,h; pic.GetDimensions(w,h); float2 rc=1/float2(w,h);"
        "float3 nw=S(i.uv+float2(-1,-1)*rc),ne=S(i.uv+float2(1,-1)*rc),"
        "sw=S(i.uv+float2(-1,1)*rc),se=S(i.uv+float2(1,1)*rc),m=S(i.uv);"
        "const float3 L=float3(0.299,0.587,0.114);"
        "float lnw=dot(nw,L),lne=dot(ne,L),lsw=dot(sw,L),lse=dot(se,L),lm=dot(m,L);"
        "float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));"
        "float2 d=float2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));"
        "float red=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0);"
        "d=clamp(d/(min(abs(d.x),abs(d.y))+red),-8.0,8.0)*rc;"
        "float3 a=0.5*(S(i.uv+d*(1.0/3.0-0.5))+S(i.uv+d*(2.0/3.0-0.5)));"
        "float3 b=a*0.5+0.25*(S(i.uv-d*0.5)+S(i.uv+d*0.5));"
        "float lb=dot(b,L);"
        "return float4(grade((lb<lmin||lb>lmax)?a:b),1);}";
    HRESULT hr = S_OK;
    if (present_vs && present_ps && present_ps_fxaa && gamma_srv && present_sampler)
        return 1;
    if (!present_vs) {
        ID3DBlob *b = compile(source, sizeof source - 1, "vs", "vs_5_0");
        hr = b ? ID3D11Device_CreateVertexShader(g_hle.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &present_vs) : E_FAIL;
        if (b) ID3D10Blob_Release(b);
    }
    if (SUCCEEDED(hr) && !present_ps) {
        ID3DBlob *b = compile(source, sizeof source - 1, "ps", "ps_5_0");
        hr = b ? ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &present_ps) : E_FAIL;
        if (b) ID3D10Blob_Release(b);
    }
    if (SUCCEEDED(hr) && !present_ps_fxaa) {
        ID3DBlob *b = compile(source, sizeof source - 1, "ps_fxaa", "ps_5_0");
        hr = b ? ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &present_ps_fxaa) : E_FAIL;
        if (b) ID3D10Blob_Release(b);
    }
    if (SUCCEEDED(hr) && !gamma_tex) {
        D3D11_TEXTURE1D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = 256; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture1D(g_hle.dev, &td, NULL, &gamma_tex);
    }
    if (SUCCEEDED(hr) && !gamma_srv)
        hr = ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)gamma_tex, NULL, &gamma_srv);
    if (SUCCEEDED(hr) && !present_sampler) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(g_hle.dev, &sd, &present_sampler);
    }
    if (FAILED(hr)) fprintf(stderr, "[HLE-PRESENT] setup failed %08X\n", (unsigned)hr);
    return SUCCEEDED(hr);
}

/* source -> target (bw x bh): returns 1 when drawn, -1 on failure. */
int hle_present_to(HleSurface *source, ID3D11RenderTargetView *target, UINT bw, UINT bh, int fxaa)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float sa, da, vw, vh;
    D3D11_VIEWPORT viewport;
    ID3D11ShaderResourceView *resources[2];
    if (!present_shaders()) return -1;
    if (gamma_dirty) {
        if (!ramp_set)
            for (uint32_t i = 0; i < 256; i++)
                ramp[i][0] = ramp[i][1] = ramp[i][2] = (uint8_t)i, ramp[i][3] = 255;
        ID3D11DeviceContext_UpdateSubresource(g_hle.ctx, (ID3D11Resource *)gamma_tex, 0, NULL, ramp, sizeof ramp, sizeof ramp);
        gamma_dirty = 0;
    }
    resources[0] = hle_surface_srv(source->data);
    resources[1] = gamma_srv;
    if (!resources[0]) return -1;
    ID3D11DeviceContext_ClearRenderTargetView(g_hle.ctx, target, black);
    /* Same shape as the picture, as large as fits, centred. */
    sa = (float)source->tw / (float)source->th;
    da = (float)bw / (float)bh;
    if (da > sa) { vh = (float)bh; vw = (float)(int)(vh * sa + 0.5f); }
    else { vw = (float)bw; vh = (float)(int)(vw / sa + 0.5f); }
    viewport.TopLeftX = (float)(int)(((float)bw - vw) * 0.5f);
    viewport.TopLeftY = (float)(int)(((float)bh - vh) * 0.5f);
    viewport.Width = vw;
    viewport.Height = vh;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_present_picture = viewport;
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 1, &target, NULL);
    ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &viewport);
    ID3D11DeviceContext_RSSetState(g_hle.ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_hle.ctx, present_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_hle.ctx, fxaa ? present_ps_fxaa : present_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 2, resources);
    ID3D11DeviceContext_PSSetSamplers(g_hle.ctx, 0, 1, &present_sampler);
    ID3D11DeviceContext_Draw(g_hle.ctx, 3, 0);
    resources[0] = resources[1] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 2, resources);
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 0, NULL, NULL);
    return 1;
}
