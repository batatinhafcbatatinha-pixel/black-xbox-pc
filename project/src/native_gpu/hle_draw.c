// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/*
 * Draw path of the native D3D11 backend (shadow mode).
 *
 * State is mirrored from every routed call, whatever its depth -- the
 * library sets most of its defaults through its own calls (CreateDevice,
 * SetRenderTarget -> SetViewport, Swap restoring what its blit changed) --
 * while draws are only taken from the game's own calls (depth 0); the
 * library's internal blits are its business.
 *
 * Where the state comes from:
 *   simple render states   SetRenderState_Simple(ecx = NV097 method, edx =
 *                          value): an NV097 register shadow, GL enums
 *   complex render states  D3D_g_RenderState[136..] (ZENABLE, CULLMODE, ...)
 *                          read at draw time; XbSymbolDatabase names them
 *   texture stage states   D3D_g_DeferredTextureState[4][32], read at draw
 *   streams                D3D_g_Stream[16] = {stride, offset, vertex buffer}
 *   vertex shader          device->m_VertexShader (+0x794): object whose
 *                          attribute table is {stream, offset, format, tess}
 *                          at dword 5 + 4*reg; microcode captured from
 *                          CreateVertexShader (header 0x2078 | n << 16)
 *   pixel shader           SetPixelShader(object): {ref, owned, pPSDef};
 *                          D3DPIXELSHADERDEF holds the combiner registers
 *   textures/palettes      SetTexture/SetPalette objects {Common, Data, Lock,
 *                          Format, Size}; Data is physical (contiguous VA
 *                          0x80000000 | Data)
 *
 * Vertices are pulled by the vertex shader from a raw buffer (hle_vsh.c), so
 * every Xbox vertex format works with one code path. Pixels go through one
 * pixel shader that evaluates the register combiners from a constant buffer
 * (the logic of xboxrecomp's validated nv2a_gpu_combiner.hlsl), plus a small
 * fixed-function texture-stage evaluator for draws without a pixel shader.
 */
#include "hle_d3d11_internal.h"
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../recomp/recomp_types.h"
#include "hle_vsh.h"
#include "d3d8_swizzle.h"
#include "hle_gpu_api.h"

#define G_RENDER_STATE      0x27C008u
#define G_TEXTURE_STATE     0x279948u
#define G_STREAM            0x279758u
#define RS(i)               MEM32(G_RENDER_STATE + 4u * (i))
#define TSS(st, i)          MEM32(G_TEXTURE_STATE + 128u * (st) + 4u * (i))

enum { RS_FOGCOLOR = 138, RS_FILLMODE = 139, RS_ZENABLE = 143, RS_STENCILENABLE = 144,
       RS_CULLMODE = 147, RS_TEXTUREFACTOR = 148 };
enum { TSS_ADDRESSU, TSS_ADDRESSV, TSS_ADDRESSW, TSS_MAGFILTER, TSS_MINFILTER, TSS_MIPFILTER,
       TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL, TSS_MAXANISOTROPY, TSS_COLORKEYOP, TSS_COLORSIGN,
       TSS_ALPHAKILL, TSS_COLOROP, TSS_COLORARG0, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP,
       TSS_ALPHAARG0, TSS_ALPHAARG1, TSS_ALPHAARG2, TSS_RESULTARG };

#define NV(m)               d.nv[(m) >> 2]

static uint32_t contig(uint32_t data) { return data | 0x80000000u; }

/* ------------------------------------------------------------------ state */

typedef struct HleVs {
    uint32_t obj;                     /* handle & ~1 */
    unsigned n;
    uint32_t (*code)[4];
    ID3D11VertexShader *vs;
    int clip_space, failed;
} HleVs;

#define MAX_VS 512

typedef struct HleTex {
    uint32_t data, format, size, pal;
    uint64_t hash, checked;
    uint64_t shash, changed;      /* sampled hash; frame the contents last changed */
    uint32_t w, h;
    int linear;
    uint32_t levels, dxgi;            /* of the native texture, for in-place updates */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} HleTex;

#define TEX_SLOTS 4096

typedef struct { uint64_t key; void *obj; } StateEntry;
#define STATE_SLOTS 256

typedef struct {
    uint32_t attr[16][4];
    float imm[16][4];
    float ss[4];
    uint32_t flags[4];
    float texgen_worldview[16], texgen_normalview[16];
    uint32_t texgen_mode[4];
    float texture_transform[4][16];
} DrawCB;

typedef struct {
    uint32_t color_icw[8], color_ocw[8], alpha_icw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8];
    uint32_t final[4];                /* inputs ABCD, EFG, constant 0, constant 1 */
    uint32_t control[4];              /* combiner control, texture modes, alpha test, flags */
    float tex_scale[4][4];
    float fog[4];
    uint32_t ff[4][4];                /* fixed function: colorop|args, alphaop|args per stage */
    uint32_t tfactor[4];              /* x: texture factor, y: fog on, z: fog table mode */
    float fogparm[4];                 /* fog start, end, density */
    float bump[4][4];
    float bump_luminance[4][4];
} PsCB;

static struct {
    int ready;
    uint32_t nv[0x800];
    float vsc[192][4];
    int vsc_dirty;
    uint32_t tex[4], pal[4], ps;
    float psc[16][4];
    uint32_t psc_set;
    struct { uint32_t x, y, w, h; float minz, maxz; } vp;
    float attr[16][4];
    uint32_t imm_prim, imm_count, imm_cap;
    float (*imm)[16][4];
    uint32_t push_begin;

    HleVs vs[MAX_VS];
    HleTex texc[TEX_SLOTS];

    ID3D11Buffer *vb;
    ID3D11ShaderResourceView *vb_srv;
    uint32_t vb_pos, vb_size;
    int vb_no_overwrite;
    ID3D11Buffer *ib;
    uint32_t ib_pos, ib_size;
    /* Vulkan (DXVK): the rings rotate through RINGS buffers instead of
     * MAP_DISCARD (see ring_next). */
    int rings;
    ID3D11Buffer *vb_ring[3], *ib_ring[3];
    ID3D11ShaderResourceView *vb_ring_srv[3];
    ID3D11Query *vb_q[3], *ib_q[3];
    int vb_q_issued[3], ib_q_issued[3];
    int vb_cur, ib_cur;
    ID3D11Buffer *cb_vsc, *cb_draw, *cb_ps;
    ID3D11PixelShader *ps_main;
    StateEntry blend[STATE_SLOTS], depth[STATE_SLOTS], raster[STATE_SLOTS], sampler[STATE_SLOTS];

    uint64_t draws, skipped, tris;
    unsigned warned[16];
} d;

/* What the draw path last bound on the context. Present, the options
 * overlay and partial clears bind their own state and call
 * hle_draw_state_invalidate(); nothing else changes these bindings
 * (shader resources are not cached: binding a render target unbinds them).
 * RECOMP_HLE_NO_STATE_CACHE=1 binds everything on every draw. */
static struct {
    int valid, off;
    ID3D11BlendState *bs; float bf[4];
    ID3D11DepthStencilState *ds; UINT sref;
    ID3D11RasterizerState *rs;
    ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp;
    ID3D11Buffer *ib;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs;
    ID3D11ShaderResourceView *vb_srv;
    ID3D11PixelShader *ps;
    ID3D11SamplerState *smp[4];
    int gs_null;
} s_bind;

/* RECOMP_HLE_AB=1 (measurement): hle_d3d11.c's profiler turns the draw-path
 * optimisations off for every other 600-frame window. */
int hle_ab_off;

void hle_draw_state_invalidate(void)
{
    s_bind.valid = 0;
}

/* Constant buffer contents last uploaded (a Map is skipped when equal).
 * RECOMP_HLE_NO_CB_SKIP=1 uploads on every draw. */
static int s_cb_skip = -1, s_cb_draw_valid, s_cb_ps_valid;

static int warn(int which, unsigned limit)
{
    return g_hle.log && d.warned[which]++ < limit;
}

static void nv_defaults(void)
{
    NV(0x304) = 0;            NV(0x344) = 1;          NV(0x348) = 0;
    NV(0x350) = 0x8006;       NV(0x354) = 0x203;      NV(0x358) = 0x01010101;
    NV(0x35C) = 1;            NV(0x300) = 0;          NV(0x33C) = 0x207;
    NV(0x360) = 0xFF;         NV(0x364) = 0x207;      NV(0x36C) = 0xFF;
    NV(0x370) = 0x1E00;       NV(0x374) = 0x1E00;     NV(0x378) = 0x1E00;
}

void hle_draw_set_simple(uint32_t method, uint32_t value)
{
    if (!d.ready) {
        nv_defaults();
        for (uint32_t i = 0; i < 16; i++) d.attr[i][3] = 1.0f;
        d.attr[3][0] = d.attr[3][1] = d.attr[3][2] = 1.0f;
        d.ready = 1;
    }
    NV(method & 0x1FFCu) = value;
}

/* -------------------------------------------------------------- shaders */

static const char k_ps_source[] =
"#ifndef CB\n#define CB(x) x\n#define COMB_LOOP [loop]\n#endif\n"
"cbuffer Comb:register(b0){uint4 CB(rcColorIcw)[2];uint4 CB(rcColorOcw)[2];uint4 CB(rcAlphaIcw)[2];uint4 CB(rcAlphaOcw)[2];"
"uint4 rcFactor0[2];uint4 rcFactor1[2];uint4 rcFinal;uint4 CB(rcControl);float4 texScale[4];float4 fogColor;"
"uint4 CB(ff)[4];uint4 tfactor;float4 fogParm;float4 bumpMat[4];float4 bumpLum[4];}\n"
"Texture2D tx0:register(t0);Texture2D tx1:register(t1);Texture2D tx2:register(t2);Texture2D tx3:register(t3);\n"
"SamplerState sm0:register(s0);SamplerState sm1:register(s1);SamplerState sm2:register(s2);SamplerState sm3:register(s3);\n"
"struct VSOut{float4 pos:SV_Position;float4 d0:COLOR0;float4 d1:COLOR1;float4 fog:FOG;"
"float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
"float4 b0:COLOR2;float4 b1:COLOR3;float pointSize:PSIZE;};\n"
"float4 unpack(uint c){return float4((c>>16)&255,(c>>8)&255,c&255,c>>24)/255.0;}\n"
"float mapIn(float x,uint b){float p=max(x,0);switch((b>>5)&7){\n"
"case 0:return p;case 1:return 1-saturate(x);case 2:return 2*p-1;\n"
"case 3:return -2*p+1;case 4:return p-0.5;case 5:return -p+0.5;\n"
"case 6:return x;default:return -x;}}\n"
"float3 readRgb(float4 r[16],uint b){float4 v=r[b&15];float3 x=(b&16)?v.www:v.xyz;\n"
"return float3(mapIn(x.x,b),mapIn(x.y,b),mapIn(x.z,b));}\n"
"float readAlpha(float4 r[16],uint b){float4 v=r[b&15];return mapIn((b&16)?v.w:v.z,b);}\n"
"float4 mapOut(float4 x,uint m){switch(m){case 8:return x-0.5;case 16:return x*2;\n"
"case 24:return (x-0.5)*2;case 32:return x*4;case 48:return x*0.5;default:return x;}}\n"
"uint word(uint4 a[2],uint s){return a[s/4][s%4];}\n"
"static float4 T0,T1,T2,T3;float4 TS(uint i){return i==0?T0:i==1?T1:i==2?T2:T3;}\n"
"float4 evalCombiner(float4 v0,float4 v1,float4 fogIn){\n"
"float4 r[16];[unroll]for(uint k=0;k<16;k++)r[k]=0;\n"
"r[3]=fogIn;r[4]=v0;r[5]=v1;[unroll]for(uint k=0;k<1;k++){r[8]=T0;r[9]=T1;r[10]=T2;r[11]=T3;}\n"
"r[12].w=(rcControl.y&31)?T0.w:1;\n"
"uint flags=rcControl.x>>8,n=min(rcControl.x&255,8);\n"
"COMB_LOOP for(uint s=0;s<n;s++){\n"
"uint icw=word(rcColorIcw,s),ocw=word(rcColorOcw,s);\n"
"uint aicw=word(rcAlphaIcw,s),aocw=word(rcAlphaOcw,s),fl=ocw>>12,afl=aocw>>12;\n"
"r[1]=unpack(word(rcFactor0,(flags&16)?s:0));r[2]=unpack(word(rcFactor1,(flags&256)?s:0));\n"
"bool mux=(flags&1)?r[12].w>=0.5:(((int)(r[12].w*255)&1)!=0);\n"
"float3 A=readRgb(r,icw>>24),B=readRgb(r,icw>>16),C=readRgb(r,icw>>8),D=readRgb(r,icw);\n"
"float aa=readAlpha(r,aicw>>24),ab=readAlpha(r,aicw>>16),ac=readAlpha(r,aicw>>8),ad=readAlpha(r,aicw);\n"
"float3 pab=(fl&2)?dot(A,B).xxx:A*B,pcd=(fl&1)?dot(C,D).xxx:C*D;\n"
"float3 sum=(fl&4)?(mux?pcd:pab):pab+pcd;\n"
"float3 rab=clamp(mapOut(float4(pab,0),fl&56).xyz,-1,1);\n"
"float3 rcd=clamp(mapOut(float4(pcd,0),fl&56).xyz,-1,1);\n"
"float3 rms=clamp(mapOut(float4(sum,0),fl&56).xyz,-1,1);\n"
"float apab=aa*ab,apcd=ac*ad;\n"
"float asum=(afl&4)?(mux?apcd:apab):apab+apcd;\n"
"float aab=clamp(mapOut(apab.xxxx,afl&56).x,-1,1);\n"
"float acd=clamp(mapOut(apcd.xxxx,afl&56).x,-1,1);\n"
"float ams=clamp(mapOut(asum.xxxx,afl&56).x,-1,1);\n"
"uint dst=(ocw>>4)&15;if(dst){r[dst].xyz=rab;if(fl&128)r[dst].w=rab.z;}\n"
"dst=ocw&15;if(dst){r[dst].xyz=rcd;if(fl&64)r[dst].w=rcd.z;}\n"
"dst=(ocw>>8)&15;if(dst)r[dst].xyz=rms;\n"
"dst=(aocw>>4)&15;if(dst)r[dst].w=aab;dst=aocw&15;if(dst)r[dst].w=acd;\n"
"dst=(aocw>>8)&15;if(dst)r[dst].w=ams;r[0]=0;\n"
"}\n"
"if(rcFinal.x||rcFinal.y){uint f0=rcFinal.x,f1=rcFinal.y,ff=f1&255;\n"
"r[1]=unpack(rcFinal.z);r[2]=unpack(rcFinal.w);\n"
"float3 a=(ff&64)?1-r[5].xyz:r[5].xyz,b=(ff&32)?1-r[12].xyz:r[12].xyz;\n"
"r[14]=float4((ff&128)?saturate(a+b):a+b,0);\n"
"float3 E=readRgb(r,f1>>24),F=readRgb(r,f1>>16);r[15]=float4(E*F,0);\n"
"float3 A=readRgb(r,f0>>24),B=readRgb(r,f0>>16),C=readRgb(r,f0>>8),D=readRgb(r,f0);\n"
"return saturate(float4(D+C*(1-A)+B*A,readAlpha(r,f1>>8)));}\n"
"return saturate(r[12]);}\n"
/* D3D fixed-function texture stages (no pixel shader set). */
"float4 ffArg(uint a,float4 cur,float4 dif,float4 spec,float4 tex,float4 tmp){float4 v;\n"
"switch(a&15){case 0:v=dif;break;case 1:v=cur;break;case 2:v=tex;break;case 3:v=unpack(tfactor.x);break;\n"
"case 4:v=spec;break;default:v=tmp;break;}if(a&32)v=v.wwww;if(a&16)v=1-v;return v;}\n"
"float4 ffOp(uint op,float4 a1,float4 a2,float4 a0,float4 dif,float4 tex,float4 cur){switch(op){\n"
"case 2:return a1;case 3:return a2;case 4:return a1*a2;case 5:return a1*a2*2;case 6:return a1*a2*4;\n"
"case 7:return a1+a2;case 8:return a1+a2-0.5;case 9:return (a1+a2-0.5)*2;case 10:return a1-a2;\n"
"case 11:return a1+a2-a1*a2;case 12:return lerp(a2,a1,dif.w);case 13:return lerp(a2,a1,tex.w);\n"
"case 14:return lerp(a2,a1,unpack(tfactor.x).w);case 15:return lerp(a2,a1,cur.w);\n"
"case 16:return a1+a2*(1-tex.w);case 17:return a1;\n"
"case 18:return a1+a1.w*a2;case 19:return a1*a2+a1.w;\n"
"case 20:return a1+(1-a1.w)*a2;case 21:return (1-a1)*a2+a1.w;\n"
"case 22:return dot(a1.xyz*2-1,a2.xyz*2-1).xxxx;case 23:return a0+a1*a2;\n"
"case 24:return lerp(a2,a1,a0);default:return cur;}}\n"
"float4 evalFixed(float4 dif,float4 spec){float4 cur=dif,tmp=0;\n"
"[unroll]for(uint s=0;s<4;s++){uint4 f=ff[s];uint cop=f.x&255;if(cop<=1)break;\n"
"if(s>0&&(ff[max(s,1)-1].x&255)==17)cur*=TS(s);if(cop==25||cop==26)continue;\n"
"float4 c=ffOp(cop,ffArg(f.x>>8,cur,dif,spec,TS(s),tmp),ffArg(f.x>>16,cur,dif,spec,TS(s),tmp),ffArg(f.x>>24,cur,dif,spec,TS(s),tmp),dif,TS(s),cur);\n"
"uint aop=f.y&255;float4 a=aop<=1?cur:ffOp(aop,ffArg(f.y>>8,cur,dif,spec,TS(s),tmp),ffArg(f.y>>16,cur,dif,spec,TS(s),tmp),ffArg(f.y>>24,cur,dif,spec,TS(s),tmp),dif,TS(s),cur);\n"
"float4 res=saturate(float4(c.xyz,cop==22?c.x:a.w));if(f.z==5)tmp=res;else cur=res;}\n"
"return cur;}\n"
"float signedByte(float value){return value>=0.5?(value*255-256)/127:value*255/127;}\n"
"float4 dependentCoord(uint st,float4 tc,float4 src,inout float2 delta,inout float lum){uint mode=(rcControl.y>>(st*5))&31;\n"
"if(mode==6||mode==7){float2 dsdt=float2(signedByte(src.z),signedByte(src.y));\n"
"delta=mul(dsdt,float2x2(bumpMat[st].x,bumpMat[st].y,bumpMat[st].z,bumpMat[st].w));\n"
"lum=mode==7?src.x*bumpLum[st].x+bumpLum[st].y:1;}\n"
"if(mode==15)tc.xy=src.wx;if(mode==16)tc.xy=src.yz;return tc;}\n"
"float4 stage(uint st,float4 tc,float2 delta,float lum,Texture2D tx,SamplerState sm){uint mode=(rcControl.y>>(st*5))&31;\n"
"if(mode==0)return float4(0,0,0,1);if(mode==4)return tc;\n"
"if(mode==5){uint cmp=(ff[st].w>>16)&15;[unroll]for(uint k=0;k<4;k++){if((cmp&(1<<k))?tc[k]>=0:tc[k]<0)discard;}return 0;}\n"
"if(!((rcControl.w>>st)&1))return 1;\n"
"float2 uv=tc.xy;if((mode==1||mode==2)&&tc.w!=0)uv/=tc.w;\n"
"float4 s=tx.Sample(sm,(uv+delta)*texScale[st].xy);\n"
"if(texScale[st].z>0.5){uint D=(uint)round(saturate(s.r)*16777215.0);\n"
"s=float4(((D>>8)&255)/255.0,(D&255)/255.0,0,(D>>16)/255.0);}\n"
"s*=lum;uint ctl=ff[st].w;if((ctl&256)&&s.w==0)discard;uint ck=(ctl>>4)&3;\n"
"uint4 q=(uint4)round(saturate(s)*255);uint color=(q.w<<24)|(q.x<<16)|(q.y<<8)|q.z;\n"
"uint mask=(ctl&512)?0xffffff:0xffffffff;if(ck&&(color&mask)==(asuint(texScale[st].w)&mask)){\n"
"if(ck==3)discard;if(ck==1)s.w=0;else s=0;}\n"
"uint sign=(ctl>>12)&15;if(sign&2)s.x=s.x>=0.5?(s.x*255-256)/127:s.x*255/127;\n"
"if(sign&4)s.y=s.y>=0.5?(s.y*255-256)/127:s.y*255/127;\n"
"if(sign&8)s.z=s.z>=0.5?(s.z*255-256)/127:s.z*255/127;\n"
"if(sign&1)s.w=s.w>=0.5?(s.w*255-256)/127:s.w*255/127;\n"
"return s;}\n"
"float fogFactor(float f){if(!tfactor.y||f<-1e29)return 1;float d=abs(f);switch(tfactor.z){\n"
"case 1:return saturate(exp(-fogParm.z*d));case 2:return saturate(exp(-(fogParm.z*d)*(fogParm.z*d)));\n"
"case 3:return saturate((fogParm.y-d)/max(fogParm.y-fogParm.x,1e-6));default:return saturate(f);}}\n"
"bool alphaPass(float a){uint at=rcControl.z;if(!(at&1))return true;uint fn=(at>>8)&7;float ref=(at>>16)&255;\n"
"float v=round(saturate(a)*255);switch(fn){case 0:return false;case 1:return v<ref;case 2:return v==ref;\n"
"case 3:return v<=ref;case 4:return v>ref;case 5:return v!=ref;case 6:return v>=ref;default:return true;}}\n"
"float4 pickc(uint k,float4 a,float4 b,float4 c,float4 d){return k==0?a:k==1?b:k==2?c:d;}\n"
"float4 main(VSOut i):SV_Target{float4 tc[4]={i.t0,i.t1,i.t2,i.t3};float2 delta=0;float lum=1;\n"
"T0=stage(0,pickc(ff[0].w&3,tc[0],tc[1],tc[2],tc[3]),delta,lum,tx0,sm0);\n"
"uint op=ff[0].x&255;if((rcControl.w&256)&&(op==25||op==26)){delta=mul(T0.xy,float2x2(bumpMat[0].x,bumpMat[0].y,bumpMat[0].z,bumpMat[0].w));lum=op==26?saturate(T0.z*bumpLum[0].x+bumpLum[0].y):1;}\n"
"tc[1]=dependentCoord(1,tc[1],T0,delta,lum);T1=stage(1,pickc(ff[1].w&3,tc[0],tc[1],tc[2],tc[3]),delta,lum,tx1,sm1);delta=0;lum=1;\n"
"op=ff[1].x&255;if((rcControl.w&256)&&(op==25||op==26)){delta=mul(T1.xy,float2x2(bumpMat[1].x,bumpMat[1].y,bumpMat[1].z,bumpMat[1].w));lum=op==26?saturate(T1.z*bumpLum[1].x+bumpLum[1].y):1;}\n"
"tc[2]=dependentCoord(2,tc[2],TS(min((ff[2].w>>20)&3,1)),delta,lum);T2=stage(2,pickc(ff[2].w&3,tc[0],tc[1],tc[2],tc[3]),delta,lum,tx2,sm2);delta=0;lum=1;\n"
"op=ff[2].x&255;if((rcControl.w&256)&&(op==25||op==26)){delta=mul(T2.xy,float2x2(bumpMat[2].x,bumpMat[2].y,bumpMat[2].z,bumpMat[2].w));lum=op==26?saturate(T2.z*bumpLum[2].x+bumpLum[2].y):1;}\n"
"tc[3]=dependentCoord(3,tc[3],TS(min((ff[3].w>>20)&3,2)),delta,lum);T3=stage(3,pickc(ff[3].w&3,tc[0],tc[1],tc[2],tc[3]),delta,lum,tx3,sm3);\n"
"float ff=fogFactor(i.fog.x);\n"
"float4 c=(rcControl.w&256)?evalFixed(i.d0,i.d1):evalCombiner(i.d0,i.d1,float4(fogColor.xyz,ff));\n"
"if(rcControl.w&256)c.rgb=lerp(fogColor.rgb,c.rgb,ff);\n"
"if(tfactor.w==1)return float4(1,0,1,1);if(tfactor.w==2)return float4(T0.rgb,1);if(tfactor.w==3)return float4(i.d0.rgb,1);if(tfactor.w==4)return float4(c.rgb,1);if(tfactor.w==5)return float4(frac(i.t0.xy),0,1);\n"
"if(!alphaPass(c.w))discard;return c;}\n";

static ID3DBlob *compile(const char *src, size_t len, const char *profile, const char *what)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = hle_d3d_compile(src, len, what, NULL, NULL, "main", profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[HLE-D3D11] %s compile failed 0x%08lX: %s\n", what, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static HleVs *vs_find(uint32_t obj)
{
    int i;
    for (i = 0; i < MAX_VS; i++)
        if (d.vs[i].obj == obj && d.vs[i].code)
            return &d.vs[i];
    return NULL;
}

/* Compiled shaders by microcode: titles create and delete the same vertex
 * shader over and over (FIFA does it every frame on the team screens), and a
 * D3DCompile per creation took the frame rate down to 1 fps. Entries are
 * never released; the set of distinct programs a title uses is small. */
typedef struct { uint64_t hash; unsigned n; ID3D11VertexShader *vs; int clip_space; } VsCompiled;
#define VS_COMPILED 1024
static VsCompiled s_vs_compiled[VS_COMPILED];

static uint64_t vs_hash(const HleVs *v)
{
    uint64_t h = 0xCBF29CE484222325ull ^ v->n;
    unsigned i;
    for (i = 0; i < v->n * 4; i++)
        h = (h ^ v->code[i / 4][i % 4]) * 0x100000001B3ull;
    return h;
}

static void vs_drop(HleVs *v)
{
    free(v->code);
    memset(v, 0, sizeof *v);
}

static void vs_register(uint32_t handle, uint32_t func)
{
    uint32_t hdr, n, i, obj = handle & ~1u;
    HleVs *v;
    if (!(handle & 1) || !func)
        return;
    hdr = MEM32(func);
    n = hdr >> 16;
    if ((hdr & 0xFFFF) != 0x2078 || !n || n > 136) {
        if (warn(0, 16))
            fprintf(stderr, "[HLE-D3D11] vertex shader %08X: unexpected function header %08X\n", handle, hdr);
        return;
    }
    if ((v = vs_find(obj)) != NULL)
        vs_drop(v);
    for (i = 0; i < MAX_VS && d.vs[i].code; i++)
        ;
    if (i == MAX_VS) {
        if (warn(1, 4))
            fprintf(stderr, "[HLE-D3D11] vertex shader table full\n");
        return;
    }
    v = &d.vs[i];
    v->obj = obj;
    v->n = n;
    v->code = (uint32_t (*)[4])malloc(n * 16u);
    for (i = 0; i < n * 4; i++)
        v->code[i / 4][i % 4] = MEM32(func + 4 + 4 * i);
}

/* Per-frame cost of work that stalls a frame the first time it is needed:
 * vertex shader compiles and texture decodes (hle_draw_frame_costs). */
static double s_cost_vs_ms, s_cost_tex_ms;
static unsigned s_cost_vs_n, s_cost_tex_n;

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* RECOMP_HLE_RTLOG=1 (diagnostics): at each swap, the render targets the
 * frame drew into and the targets it sampled as textures, printed when that
 * set is new -- which scenes render off screen (blur, glow, depth of field). */
static int s_rtlog = -1;
void hle_trace_arm_next(void);
static struct { uint32_t rt, ps, vs; unsigned n; } s_rtf[8];
static unsigned s_nrtf;
static uint32_t s_rtread[8], s_rtread_ps[8], s_rtread_into[8], s_rtcur;
static unsigned s_nrtread;

static void rtlog_draw(uint32_t rt, uint32_t ps, uint32_t vs)
{
    unsigned i;
    for (i = 0; i < s_nrtf; i++)
        if (s_rtf[i].rt == rt && s_rtf[i].ps == ps) {
            s_rtf[i].n++;
            return;
        }
    if (s_nrtf < 8) {
        s_rtf[s_nrtf].rt = rt; s_rtf[s_nrtf].ps = ps; s_rtf[s_nrtf].vs = vs; s_rtf[s_nrtf].n = 1;
        s_nrtf++;
    }
}

static void rtlog_read(uint32_t data)
{
    unsigned i;
    if (s_rtlog <= 0)
        return;
    for (i = 0; i < s_nrtread; i++)
        if (s_rtread[i] == data && s_rtread_ps[i] == d.ps && s_rtread_into[i] == s_rtcur)
            return;
    if (s_nrtread < 8) {
        s_rtread_ps[s_nrtread] = d.ps;
        s_rtread_into[s_nrtread] = s_rtcur;
        s_rtread[s_nrtread++] = data;
    }
}

static void rtlog_frame(void)
{
    static uint64_t seen[512];
    static unsigned nseen;
    uint64_t sig = 0xCBF29CE484222325ull;
    unsigned i, k;
    if (s_rtlog <= 0)
        return;
    if (s_nrtf > 1 || s_nrtread) {
        uint32_t rts[8];
        unsigned nrt = 0;
        for (i = 0; i < s_nrtf; i++) {
            for (k = 0; k < nrt && rts[k] != s_rtf[i].rt; k++)
                ;
            if (k == nrt) rts[nrt++] = s_rtf[i].rt;
        }
        for (i = 0; i < nrt; i++) sig = (sig ^ rts[i]) * 0x100000001B3ull;
        for (i = 0; i < s_nrtread; i++) sig = (sig ^ s_rtread[i] ^ 0x5555u) * 0x100000001B3ull;
        for (k = 0; k < nseen && seen[k] != sig; k++)
            ;
        if (k == nseen && nseen < 512) {
            seen[nseen++] = sig;
            fprintf(stderr, "[HLE-RT] swap %llu:", (unsigned long long)g_hle.swaps);
            for (i = 0; i < s_nrtf; i++)
                fprintf(stderr, " rt %08X ps %08X vs %08X x%u;", s_rtf[i].rt, s_rtf[i].ps, s_rtf[i].vs, s_rtf[i].n);
            fprintf(stderr, " reads");
            for (i = 0; i < s_nrtread; i++)
                fprintf(stderr, " %08X(ps %08X -> rt %08X)", s_rtread[i], s_rtread_ps[i], s_rtread_into[i]);
            fprintf(stderr, "\n");
            {
                extern volatile int hle_dump_request;
                static int dumps;
                if (dumps++ < 24)
                    hle_dump_request = 1;     /* a picture of the next frame, when dumps are on */
                if (s_nrtread && dumps < 4)
                    hle_trace_arm_next();     /* RECOMP_HLE_TRACE=black: trace it too */
            }
        }
    }
    s_nrtf = s_nrtread = 0;
}

void hle_draw_frame_costs(double *vs_ms, unsigned *vs_n, double *tex_ms, unsigned *tex_n)
{
    rtlog_frame();
    *vs_ms = s_cost_vs_ms; *vs_n = s_cost_vs_n;
    *tex_ms = s_cost_tex_ms; *tex_n = s_cost_tex_n;
    s_cost_vs_ms = s_cost_tex_ms = 0.0;
    s_cost_vs_n = s_cost_tex_n = 0;
}

/* Compiled vertex shaders on disk (shadercache.bin next to the exe), keyed
 * by the generated HLSL: D3DCompile takes tens of milliseconds, so a shader
 * met for the first time in a match was a visible hitch -- now it is one
 * once per machine, and a later run creates it from the bytecode (<1 ms).
 * A change to the generator changes the HLSL and so the key; stale entries
 * just stop matching. */
typedef struct { uint64_t key; int clip_space; uint32_t size; void *code; } DiskVs;
static DiskVs *s_disk;
static unsigned s_ndisk, s_capdisk;
static FILE *s_disk_out;
#define DISK_MAGIC 0x31435346u                /* "FSC1" */

static void disk_path(wchar_t *p)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, p, MAX_PATH);
    slash = wcsrchr(p, L'\\');
    wcscpy(slash ? slash + 1 : p, L"shadercache.bin");
}

static void disk_add(uint64_t key, int clip_space, const void *code, uint32_t size)
{
    if (s_ndisk == s_capdisk) {
        unsigned cap = s_capdisk ? s_capdisk * 2 : 256;
        DiskVs *n = (DiskVs *)realloc(s_disk, cap * sizeof *n);
        if (!n)
            return;
        s_disk = n;
        s_capdisk = cap;
    }
    s_disk[s_ndisk].key = key;
    s_disk[s_ndisk].clip_space = clip_space;
    s_disk[s_ndisk].size = size;
    s_disk[s_ndisk].code = malloc(size);
    if (!s_disk[s_ndisk].code)
        return;
    memcpy(s_disk[s_ndisk].code, code, size);
    s_ndisk++;
}

static void disk_load(void)
{
    wchar_t p[MAX_PATH];
    FILE *f;
    uint32_t magic = 0, rec[4];
    disk_path(p);
    if ((f = _wfopen(p, L"rb")) != NULL) {
        if (fread(&magic, 4, 1, f) == 1 && magic == DISK_MAGIC) {
            while (fread(rec, sizeof rec, 1, f) == 1 && rec[3] && rec[3] < (1u << 20)) {
                void *code = malloc(rec[3]);
                if (!code || fread(code, 1, rec[3], f) != rec[3]) {
                    free(code);
                    break;
                }
                disk_add((uint64_t)rec[0] | ((uint64_t)rec[1] << 32), (int)rec[2], code, rec[3]);
                free(code);
            }
        }
        fclose(f);
    }
    s_disk_out = _wfopen(p, magic == DISK_MAGIC ? L"ab" : L"wb");
    if (s_disk_out && magic != DISK_MAGIC) {
        magic = DISK_MAGIC;
        fwrite(&magic, 4, 1, s_disk_out);
        fflush(s_disk_out);
    }
    fprintf(stderr, "[HLE-D3D11] shader cache: %u vertex shaders from disk\n", s_ndisk);
}

/* The pixel shader worker (ps_variant) shares the cache with the draw
 * thread. Entries are never removed and their code never moves. */
static SRWLOCK s_disk_lock = SRWLOCK_INIT;

static int disk_find(uint64_t key, int *clip_space, const void **code, uint32_t *size)
{
    unsigned i;
    int found = 0;
    AcquireSRWLockShared(&s_disk_lock);
    for (i = 0; i < s_ndisk; i++)
        if (s_disk[i].key == key) {
            *clip_space = s_disk[i].clip_space;
            *code = s_disk[i].code;
            *size = s_disk[i].size;
            found = 1;
            break;
        }
    ReleaseSRWLockShared(&s_disk_lock);
    return found;
}

static void disk_store(uint64_t key, int clip_space, const void *code, uint32_t size)
{
    uint32_t rec[4] = { (uint32_t)key, (uint32_t)(key >> 32), (uint32_t)clip_space, size };
    AcquireSRWLockExclusive(&s_disk_lock);
    disk_add(key, clip_space, code, size);
    if (s_disk_out) {
        fwrite(rec, sizeof rec, 1, s_disk_out);
        fwrite(code, 1, size, s_disk_out);
        fflush(s_disk_out);
    }
    ReleaseSRWLockExclusive(&s_disk_lock);
}

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static ID3D11VertexShader *vs_get(HleVs *v)
{
    static char src[256 * 1024];
    char err[160];
    size_t len;
    ID3DBlob *blob;
    unsigned i;

    uint64_t h;
    VsCompiled *slot = NULL;

    if (v->vs || v->failed)
        return v->vs;
    h = vs_hash(v);
    for (i = 0; i < 16; i++) {
        VsCompiled *c = &s_vs_compiled[(h + i) & (VS_COMPILED - 1)];
        if (c->vs && c->hash == h && c->n == v->n) {
            v->vs = c->vs;
            v->clip_space = c->clip_space;
            return v->vs;
        }
        if (!c->vs && !slot)
            slot = c;
    }
    len = hle_vsh_hlsl((const uint32_t (*)[4])v->code, v->n, src, sizeof src, &v->clip_space, err, sizeof err);
    if (!len) {
        fprintf(stderr, "[HLE-D3D11] VS %08X: %s\n", v->obj, err);
        v->failed = 1;
        return NULL;
    }
    {
        uint64_t key = fnv(src, len, 0xCBF29CE484222325ull);
        double t0 = now_ms();
        const void *code;
        uint32_t size;
        int clip;
        if (disk_find(key, &clip, &code, &size)) {
            v->clip_space = clip;
            if (FAILED(ID3D11Device_CreateVertexShader(g_hle.dev, code, size, NULL, &v->vs)))
                v->vs = NULL;
        }
        if (!v->vs) {
            blob = compile(src, len, "vs_5_0", "hle_vs");
            if (!blob || FAILED(ID3D11Device_CreateVertexShader(g_hle.dev, ID3D10Blob_GetBufferPointer(blob),
                                                                 ID3D10Blob_GetBufferSize(blob), NULL, &v->vs))) {
                v->failed = 1;
                v->vs = NULL;
            } else {
                disk_store(key, v->clip_space, ID3D10Blob_GetBufferPointer(blob),
                           (uint32_t)ID3D10Blob_GetBufferSize(blob));
            }
            if (blob) ID3D10Blob_Release(blob);
        }
        s_cost_vs_ms += now_ms() - t0;
        s_cost_vs_n++;
    }
    if (v->vs && slot) {
        slot->hash = h;
        slot->n = v->n;
        slot->vs = v->vs;
        slot->clip_space = v->clip_space;
    }
    if (g_hle.log) {
        fprintf(stderr, "[HLE-D3D11] VS %08X: %u instructions, %s%s\n", v->obj, v->n,
                v->clip_space ? "clip space" : "SCREEN SPACE", v->failed ? ", FAILED" : "");
        if (!v->clip_space)
            for (i = 0; i < v->n; i++)
                fprintf(stderr, "[HLE-D3D11]   %08X %08X %08X %08X\n",
                        v->code[i][0], v->code[i][1], v->code[i][2], v->code[i][3]);
    }
    return v->vs;
}

/* ------------------------------------------------------------- textures */

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* 64 words spread over the texture plus both ends: a cheap per-frame probe
 * for textures that have not changed in a while. */
static uint64_t sample_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i, step;
    if (n <= 2048)
        return hash_bytes(p, n, h);
    h = hash_bytes(p, 256, h);
    h = hash_bytes(p + n - 256, 256, h);
    step = (n - 8) / 64;
    for (i = 0; i < 64; i++) {
        uint64_t w;
        memcpy(&w, p + i * step, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

typedef struct { int bpp; int kind; } XFmt;
enum { K_NONE, K_ARGB8, K_XRGB8, K_565, K_1555, K_X555, K_4444, K_L8, K_AL8, K_A8, K_A8L8,
       K_P8, K_DXT1, K_DXT3, K_DXT5, K_YUY2, K_UYVY, K_L16 };

static XFmt xfmt_info(uint32_t f, int *swizzled)
{
    XFmt x = { 0, K_NONE };
    *swizzled = 1;
    switch (f) {
    case 0x06: x.bpp = 4; x.kind = K_ARGB8; break;
    case 0x07: x.bpp = 4; x.kind = K_XRGB8; break;
    case 0x05: x.bpp = 2; x.kind = K_565; break;
    case 0x02: x.bpp = 2; x.kind = K_1555; break;
    case 0x03: x.bpp = 2; x.kind = K_X555; break;
    case 0x04: x.bpp = 2; x.kind = K_4444; break;
    case 0x00: x.bpp = 1; x.kind = K_L8; break;
    case 0x01: x.bpp = 1; x.kind = K_AL8; break;
    case 0x19: x.bpp = 1; x.kind = K_A8; break;
    case 0x1A: x.bpp = 2; x.kind = K_A8L8; break;
    case 0x0B: x.bpp = 1; x.kind = K_P8; break;
    case 0x32: x.bpp = 2; x.kind = K_L16; break;
    case 0x0C: x.bpp = 0; x.kind = K_DXT1; *swizzled = 0; break;
    case 0x0E: x.bpp = 0; x.kind = K_DXT3; *swizzled = 0; break;
    case 0x0F: x.bpp = 0; x.kind = K_DXT5; *swizzled = 0; break;
    default:
        *swizzled = 0;
        switch (f) {
        case 0x12: x.bpp = 4; x.kind = K_ARGB8; break;
        case 0x1E: x.bpp = 4; x.kind = K_XRGB8; break;
        case 0x11: x.bpp = 2; x.kind = K_565; break;
        case 0x10: x.bpp = 2; x.kind = K_1555; break;
        case 0x1C: x.bpp = 2; x.kind = K_X555; break;
        case 0x1D: x.bpp = 2; x.kind = K_4444; break;
        case 0x13: x.bpp = 1; x.kind = K_L8; break;
        case 0x1B: x.bpp = 1; x.kind = K_AL8; break;
        case 0x1F: x.bpp = 1; x.kind = K_A8; break;
        case 0x20: x.bpp = 2; x.kind = K_A8L8; break;
        case 0x35: x.bpp = 2; x.kind = K_L16; break;
        case 0x24: x.bpp = 2; x.kind = K_YUY2; break;
        case 0x25: x.bpp = 2; x.kind = K_UYVY; break;
        }
    }
    return x;
}

static uint32_t e5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t e6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t e4(uint32_t v) { return v * 17; }
static uint32_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

static uint32_t yuv(int y, int u, int v)
{
    int c = y - 16, dd = u - 128, e = v - 128;
    return 0xFF000000u | (clamp8((298 * c + 409 * e + 128) >> 8) << 16)
         | (clamp8((298 * c - 100 * dd - 208 * e + 128) >> 8) << 8) | clamp8((298 * c + 516 * dd + 128) >> 8);
}

/* One level of an uncompressed texture, already in raster order, to BGRA8.
 * The format is chosen once per level, not per texel (same arithmetic). */
#define CONVERT_ROWS(expr) \
    for (y = 0; y < h; y++) { \
        const uint8_t *s = src + (size_t)y * pitch; \
        uint32_t *o = dst + (size_t)y * w; \
        for (x = 0; x < w; x++) { uint32_t v, l; (void)v; (void)l; o[x] = (expr); } \
    }

static uint32_t rd16(const uint8_t *s, uint32_t x) { return s[2 * x] | ((uint32_t)s[2 * x + 1] << 8); }
static uint32_t rd32(const uint8_t *s, uint32_t x) { uint32_t v; memcpy(&v, s + 4 * x, 4); return v; }

static void convert_level(uint32_t *dst, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                          int kind, const uint32_t *palette)
{
    uint32_t x, y;
    switch (kind) {
    case K_ARGB8:
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * w, src + (size_t)y * pitch, (size_t)w * 4);
        break;
    case K_XRGB8: CONVERT_ROWS(rd32(s, x) | 0xFF000000u) break;
    case K_565: CONVERT_ROWS((v = rd16(s, x), 0xFF000000u | (e5(v >> 11) << 16) | (e6((v >> 5) & 63) << 8) | e5(v & 31))) break;
    case K_1555: CONVERT_ROWS((v = rd16(s, x), ((v & 0x8000) ? 0xFF000000u : 0) | (e5((v >> 10) & 31) << 16) | (e5((v >> 5) & 31) << 8) | e5(v & 31))) break;
    case K_X555: CONVERT_ROWS((v = rd16(s, x), 0xFF000000u | (e5((v >> 10) & 31) << 16) | (e5((v >> 5) & 31) << 8) | e5(v & 31))) break;
    case K_4444: CONVERT_ROWS((v = rd16(s, x), (e4(v >> 12) << 24) | (e4((v >> 8) & 15) << 16) | (e4((v >> 4) & 15) << 8) | e4(v & 15))) break;
    case K_L8: CONVERT_ROWS(0xFF000000u | s[x] * 0x010101u) break;
    case K_AL8: CONVERT_ROWS(s[x] * 0x01010101u) break;
    case K_A8: CONVERT_ROWS(((uint32_t)s[x] << 24) | 0xFFFFFFu) break;
    case K_A8L8: CONVERT_ROWS(((uint32_t)s[2 * x + 1] << 24) | s[2 * x] * 0x010101u) break;
    case K_L16: CONVERT_ROWS(0xFF000000u | s[2 * x + 1] * 0x010101u) break;
    case K_P8: CONVERT_ROWS(palette ? palette[s[x]] : 0xFFFF00FFu) break;
    case K_YUY2: case K_UYVY: {
        /* Video frames: the chroma terms of yuv() once per pixel pair. */
        int yo = kind == K_YUY2 ? 0 : 1, uo = kind == K_YUY2 ? 1 : 0;
        for (y = 0; y < h; y++) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint32_t *o = dst + (size_t)y * w;
            for (x = 0; x < w; x += 2) {
                const uint8_t *p = s + 2 * x;
                int dd = p[uo] - 128, e = p[uo + 2] - 128;
                int rv = 409 * e + 128, gv = -100 * dd - 208 * e + 128, bv = 516 * dd + 128;
                int c0 = 298 * (p[yo] - 16);
                o[x] = 0xFF000000u | (clamp8((c0 + rv) >> 8) << 16) | (clamp8((c0 + gv) >> 8) << 8) | clamp8((c0 + bv) >> 8);
                if (x + 1 < w) {
                    int c1 = 298 * (p[yo + 2] - 16);
                    o[x + 1] = 0xFF000000u | (clamp8((c1 + rv) >> 8) << 16) | (clamp8((c1 + gv) >> 8) << 8) | clamp8((c1 + bv) >> 8);
                }
            }
        }
        break;
    }
    default: CONVERT_ROWS(0xFFFF00FFu) break;
    }
}
#undef CONVERT_ROWS

static void tex_release(HleTex *t)
{
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
}

/* Build the native texture for a guest texture object. */
static int tex_build(HleTex *t, uint32_t obj, uint32_t pal)
{
    uint32_t data = t->data, fmt = t->format, size = t->size, xf = (fmt >> 8) & 0xFF;
    uint32_t w, h, pitch = 0, levels = (fmt >> 16) & 0xF, lv, off = 0;
    int swz, linear = size != 0;
    XFmt x = xfmt_info(xf, &swz);
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[16];
    uint32_t *buf[16] = { 0 }, *palette = NULL, palbuf[256];
    uint8_t *tmp = NULL;
    const uint8_t *src = (const uint8_t *)XBOX_PTR(contig(data));
    int dxt = x.kind == K_DXT1 || x.kind == K_DXT3 || x.kind == K_DXT5;
    HRESULT hr;

    (void)obj;
    if (x.kind == K_NONE || (fmt & 4) || ((fmt >> 4) & 0xF) != 2) {
        if (warn(2, 24))
            fprintf(stderr, "[HLE-D3D11] texture %08X: format %02X%s not handled\n", data, xf,
                    (fmt & 4) ? " (cube)" : "");
        return 0;
    }
    if (linear) {
        w = (size & 0xFFF) + 1;
        h = ((size >> 12) & 0xFFF) + 1;
        pitch = ((size >> 24) + 1) * 64;
        levels = 1;
    } else {
        w = 1u << ((fmt >> 20) & 0xF);
        h = 1u << ((fmt >> 24) & 0xF);
    }
    if (!levels) levels = 1;
    if (levels > 12) levels = 12;
    if (dxt && (w & 3 || h & 3))
        dxt = 0;
    if (x.kind == K_P8 && pal) {
        uint32_t i, pa = contig(MEM32(pal + 4));
        for (i = 0; i < 256; i++)
            palbuf[i] = MEM32(pa + 4 * i);
        palette = palbuf;
    }

    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = dxt ? (x.kind == K_DXT1 ? DXGI_FORMAT_BC1_UNORM : x.kind == K_DXT3 ? DXGI_FORMAT_BC2_UNORM
                                                                                  : DXGI_FORMAT_BC3_UNORM)
                    : DXGI_FORMAT_B8G8R8A8_UNORM;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        if (x.kind == K_DXT1 || x.kind == K_DXT3 || x.kind == K_DXT5) {
            uint32_t bb = x.kind == K_DXT1 ? 8 : 16, bw = (lw + 3) / 4, bh = (lh + 3) / 4;
            if (dxt) {
                sd[lv].pSysMem = src + off;
                sd[lv].SysMemPitch = bw * bb;
            } else {
                uint32_t px, py;
                buf[lv] = (uint32_t *)malloc((size_t)lw * lh * 4);
                for (py = 0; py < lh; py++)
                    for (px = 0; px < lw; px++)
                        d3d8_dxt_decode_texel(src + off, xf, px, py, lw, &buf[lv][py * lw + px]);
                sd[lv].pSysMem = buf[lv];
                sd[lv].SysMemPitch = lw * 4;
            }
            off += bw * bh * bb;
        } else {
            uint32_t lpitch = linear ? pitch : lw * (uint32_t)x.bpp;
            const uint8_t *ls = src + off;
            buf[lv] = (uint32_t *)malloc((size_t)lw * lh * 4);
            if (swz) {
                tmp = (uint8_t *)realloc(tmp, (size_t)lw * lh * x.bpp);
                xbox_unswizzle_rect(tmp, ls, lw, lh, (uint32_t)x.bpp);
                ls = tmp;
            }
            convert_level(buf[lv], ls, lw, lh, lpitch, x.kind, palette);
            sd[lv].pSysMem = buf[lv];
            sd[lv].SysMemPitch = lw * 4;
            off += lpitch * lh;
        }
        sd[lv].SysMemSlicePitch = 0;
    }
    td.MipLevels = levels;
    /* Contents changed, same shape (video frames, HUD digits): new texels
     * into the same texture instead of a new texture and view per change.
     * RECOMP_HLE_NO_TEX_REUSE=1 always recreates. */
    {
        static int reuse = -1;
        if (reuse < 0) {
            const char *e = getenv("RECOMP_HLE_NO_TEX_REUSE");
            reuse = !(e && *e && *e != '0');
        }
        if (t->tex && (!reuse || hle_ab_off || t->w != w || t->h != h || t->levels != levels || t->dxgi != (uint32_t)td.Format)) {
            ID3D11Texture2D *old_tex = t->tex;
            ID3D11ShaderResourceView *old_srv = t->srv;
            t->tex = NULL;
            t->srv = NULL;
            if (old_srv) ID3D11ShaderResourceView_Release(old_srv);
            ID3D11Texture2D_Release(old_tex);
        }
    }
    if (t->tex && t->srv) {
        for (lv = 0; lv < levels; lv++)
            ID3D11DeviceContext_UpdateSubresource(g_hle.ctx, (ID3D11Resource *)t->tex, lv, NULL,
                                                  sd[lv].pSysMem, sd[lv].SysMemPitch, 0);
        hr = S_OK;
    } else {
        hr = ID3D11Device_CreateTexture2D(g_hle.dev, &td, sd, &t->tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
    }
    for (lv = 0; lv < 16; lv++)
        free(buf[lv]);
    free(tmp);
    if (FAILED(hr)) {
        fprintf(stderr, "[HLE-D3D11] texture %08X %ux%u fmt %02X: create failed 0x%08lX\n",
                data, w, h, xf, (unsigned long)hr);
        tex_release(t);
        return 0;
    }
    t->w = w;
    t->h = h;
    t->linear = linear;
    t->levels = levels;
    t->dxgi = (uint32_t)td.Format;
    return 1;
}

static uint32_t tex_bytes(uint32_t fmt, uint32_t size)
{
    int swz;
    uint32_t xf = (fmt >> 8) & 0xFF, w, h, levels = (fmt >> 16) & 0xF, lv, total = 0;
    XFmt x = xfmt_info(xf, &swz);
    if (size)
        return (((size >> 12) & 0xFFF) + 1) * (((size >> 24) + 1) * 64);
    w = 1u << ((fmt >> 20) & 0xF);
    h = 1u << ((fmt >> 24) & 0xF);
    if (!levels) levels = 1;
    for (lv = 0; lv < levels && lv < 12; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        if (x.kind == K_DXT1 || x.kind == K_DXT3 || x.kind == K_DXT5)
            total += ((lw + 3) / 4) * ((lh + 3) / 4) * (x.kind == K_DXT1 ? 8 : 16);
        else
            total += lw * lh * (uint32_t)(x.bpp ? x.bpp : 4);
    }
    return total;
}

int hle_trace_on(void);

/* Native view for the texture object bound to a stage; scale = 1/size for
 * linear textures, whose coordinates are in texels on the Xbox. */
static ID3D11ShaderResourceView *tex_get(uint32_t obj, uint32_t pal, float scale[4])
{
    uint32_t data = MEM32(obj + 4), fmt = MEM32(obj + 12), size = MEM32(obj + 16);
    uint32_t key = data ^ (fmt * 0x9E3779B1u) ^ (size * 0x85EBCA77u) ^ (pal * 0xC2B2AE3Du);
    ID3D11ShaderResourceView *rt;
    HleTex *t = NULL, *victim = NULL;
    uint64_t h, sample = 0;
    unsigned i;
    int changed_now = 0;

    scale[0] = scale[1] = 1.0f;
    scale[2] = scale[3] = 0.0f;
    if (size) {
        scale[0] = 1.0f / (float)((size & 0xFFF) + 1);
        scale[1] = 1.0f / (float)(((size >> 12) & 0xFFF) + 1);
    }
    rt = hle_surface_srv(data);
    if (!rt)
        rt = hle_front_srv(data);
    if (rt && hle_surface_is_depth(data))
        scale[2] = 1.0f;                  /* depth read as A8R8G8B8 */
    if (hle_trace_on())
        fprintf(stderr, "[HLE-T]   tex obj %08X data %08X fmt %08X size %08X%s\n",
                obj, data, fmt, size, rt ? " (native render target)" : "");
    if (rt) {
        rtlog_read(data);
        return rt;
    }
    for (i = 0; i < 16; i++) {
        HleTex *c = &d.texc[(key + i) & (TEX_SLOTS - 1)];
        if (c->tex && c->data == data && c->format == fmt && c->size == size && c->pal == pal) {
            t = c;
            break;
        }
        if (!victim || !c->tex || (victim->tex && c->checked < victim->checked))
            victim = c;
    }
    /* Change detection. A texture that changed in the last 30 frames is
     * "dynamic": small ones are re-hashed on every use, because the game
     * rewrites some (HUD digits) several times within one frame. Static
     * ones get a sampled probe once per frame and a full hash every 16th
     * frame (staggered per slot), which keeps hashing off the critical path
     * for the hundreds of loaded asset textures. */
    {
        uint64_t frame = g_hle.swaps + 1;
        const uint8_t *bytes = (const uint8_t *)XBOX_PTR(contig(data));
        size_t nb = tex_bytes(fmt, size);
        uint64_t sh = 0;
        int have_sh = 0;
        if (t) {
            int dynamic = t->changed + 30 > frame;
            if (t->checked == frame && !(dynamic && nb <= 65536))
                return t->srv;
            if (!dynamic && t->checked != frame &&
                ((frame + (uint64_t)(t - d.texc)) & 15) != 0) {
                sh = sample_hash(bytes, nb);
                have_sh = 1;
                if (pal)
                    sh = hash_bytes((const uint8_t *)XBOX_PTR(contig(MEM32(pal + 4))), 1024, sh);
                if (sh == t->shash) {
                    t->checked = frame;
                    return t->srv;
                }
            }
        }
        h = hash_bytes(bytes, nb, 0xCBF29CE484222325ull);
        if (pal)
            h = hash_bytes((const uint8_t *)XBOX_PTR(contig(MEM32(pal + 4))), 1024, h);
        if (!have_sh) {
            sh = sample_hash(bytes, nb);
            if (pal)
                sh = hash_bytes((const uint8_t *)XBOX_PTR(contig(MEM32(pal + 4))), 1024, sh);
        }
        if (t && t->hash == h) {
            t->checked = frame;
            t->shash = sh;
            return t->srv;
        }
        if (t && t->hash != h)
            changed_now = 1;
        sample = sh;
    }
    if (!t) {
        t = victim;
        tex_release(t);
    }                                     /* else tex_build updates it in place */
    t->data = data;
    t->format = fmt;
    t->size = size;
    t->pal = pal;
    {
        double t0 = now_ms();
        int ok = tex_build(t, obj, pal);
        s_cost_tex_ms += now_ms() - t0;
        s_cost_tex_n++;
        if (!ok) {
            t->data = 0;
            return NULL;
        }
    }
    t->hash = h;
    t->shash = sample;
    t->checked = g_hle.swaps + 1;
    t->changed = changed_now ? g_hle.swaps + 1 : 0;
    return t->srv;
}

/* ------------------------------------------------------- state objects */

static void *state_lookup(StateEntry *tab, uint64_t key)
{
    unsigned i, at = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 56);
    for (i = 0; i < STATE_SLOTS; i++) {
        StateEntry *e = &tab[(at + i) % STATE_SLOTS];
        if (!e->obj)
            return NULL;
        if (e->key == key)
            return e->obj;
    }
    return NULL;
}

static void state_insert(StateEntry *tab, uint64_t key, void *obj)
{
    unsigned i, at = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 56);
    for (i = 0; i < STATE_SLOTS; i++) {
        StateEntry *e = &tab[(at + i) % STATE_SLOTS];
        if (!e->obj) {
            e->key = key;
            e->obj = obj;
            return;
        }
    }
}

static D3D11_BLEND blend_factor(uint32_t gl, int alpha)
{
    switch (gl) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t gl)
{
    return (D3D11_COMPARISON_FUNC)(((gl - 0x200) & 7) + 1);
}

static void apply_output_state(int has_depth, uint32_t geometry_flags)
{
    uint32_t mask = NV(0x358), cm = 0;
    uint64_t key;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *ds;
    ID3D11RasterizerState *rs;
    float bf[4];
    uint32_t bc = NV(0x34C);

    if (mask & 0x00010000u) cm |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (mask & 0x00000100u) cm |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (mask & 0x00000001u) cm |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (mask & 0x01000000u) cm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

    key = ((uint64_t)(NV(0x304) & 1) << 63) | ((uint64_t)(NV(0x344) & 0xFFFF) << 40)
        | ((uint64_t)(NV(0x348) & 0xFFFF) << 24) | ((uint64_t)(NV(0x350) & 0xFFFF) << 8) | cm;
    if (!(bs = (ID3D11BlendState *)state_lookup(d.blend, key))) {
        D3D11_BLEND_DESC b;
        memset(&b, 0, sizeof b);
        b.RenderTarget[0].BlendEnable = NV(0x304) & 1;
        b.RenderTarget[0].SrcBlend = blend_factor(NV(0x344), 0);
        b.RenderTarget[0].DestBlend = blend_factor(NV(0x348), 0);
        b.RenderTarget[0].BlendOp = blend_op(NV(0x350));
        b.RenderTarget[0].SrcBlendAlpha = blend_factor(NV(0x344), 1);
        b.RenderTarget[0].DestBlendAlpha = blend_factor(NV(0x348), 1);
        b.RenderTarget[0].BlendOpAlpha = blend_op(NV(0x350));
        b.RenderTarget[0].RenderTargetWriteMask = (UINT8)cm;
        if (SUCCEEDED(ID3D11Device_CreateBlendState(g_hle.dev, &b, &bs)))
            state_insert(d.blend, key, bs);
    }
    if (NV(0x344) == 0x8003 || NV(0x344) == 0x8004 || NV(0x348) == 0x8003 || NV(0x348) == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3] = (bc >> 24) / 255.0f;
    else {
        bf[0] = ((bc >> 16) & 255) / 255.0f; bf[1] = ((bc >> 8) & 255) / 255.0f;
        bf[2] = (bc & 255) / 255.0f; bf[3] = (bc >> 24) / 255.0f;
    }
    if (!s_bind.valid || s_bind.bs != bs || memcmp(s_bind.bf, bf, sizeof bf)) {
        ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, bs, bf, 0xFFFFFFFFu);
        s_bind.bs = bs;
        memcpy(s_bind.bf, bf, sizeof bf);
    }

    {
        int zen = has_depth && RS(RS_ZENABLE) != 0, sen = has_depth && RS(RS_STENCILENABLE) != 0;
        key = ((uint64_t)zen << 63) | ((uint64_t)(NV(0x35C) & 1) << 62) | ((uint64_t)(NV(0x354) & 7) << 59)
            | ((uint64_t)sen << 58) | ((uint64_t)(NV(0x364) & 7) << 55)
            | ((uint64_t)stencil_op(NV(0x370)) << 51) | ((uint64_t)stencil_op(NV(0x374)) << 47)
            | ((uint64_t)stencil_op(NV(0x378)) << 43) | ((uint64_t)(NV(0x36C) & 0xFF) << 35)
            | ((uint64_t)(NV(0x360) & 0xFF) << 27);
        if (!(ds = (ID3D11DepthStencilState *)state_lookup(d.depth, key))) {
            D3D11_DEPTH_STENCIL_DESC z;
            memset(&z, 0, sizeof z);
            z.DepthEnable = zen;
            z.DepthWriteMask = (NV(0x35C) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            z.DepthFunc = cmp_func(NV(0x354));
            z.StencilEnable = sen;
            z.StencilReadMask = (UINT8)NV(0x36C);
            z.StencilWriteMask = (UINT8)NV(0x360);
            z.FrontFace.StencilFunc = cmp_func(NV(0x364));
            z.FrontFace.StencilFailOp = stencil_op(RS(145));
            z.FrontFace.StencilDepthFailOp = stencil_op(NV(0x374));
            z.FrontFace.StencilPassOp = stencil_op(NV(0x378));
            z.BackFace = z.FrontFace;
            if (SUCCEEDED(ID3D11Device_CreateDepthStencilState(g_hle.dev, &z, &ds)))
                state_insert(d.depth, key, ds);
        }
        if (!s_bind.valid || s_bind.ds != ds || s_bind.sref != (NV(0x368) & 0xFF)) {
            ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, ds, NV(0x368) & 0xFF);
            s_bind.ds = ds;
            s_bind.sref = NV(0x368) & 0xFF;
        }
    }

    {
        extern uint64_t hle_quality_raster(int has_depth, D3D11_RASTERIZER_DESC *out);
        D3D11_RASTERIZER_DESC r;
        key = hle_quality_raster(has_depth, &r);
        if (geometry_flags & 1u) {
            r.FillMode = D3D11_FILL_SOLID;
            r.CullMode = D3D11_CULL_NONE;
            key ^= 0x8000000000000000ULL;
        }
        if (!(rs = (ID3D11RasterizerState *)state_lookup(d.raster, key))) {
            if (SUCCEEDED(ID3D11Device_CreateRasterizerState(g_hle.dev, &r, &rs)))
                state_insert(d.raster, key, rs);
        }
        if (!s_bind.valid || s_bind.rs != rs) {
            ID3D11DeviceContext_RSSetState(g_hle.ctx, rs);
            s_bind.rs = rs;
        }
    }
}

static D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t a)
{
    switch (a) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState *sampler_for(unsigned st)
{
    extern uint64_t hle_quality_sampler(uint32_t stage, D3D11_SAMPLER_DESC *out);
    D3D11_SAMPLER_DESC sd;
    uint64_t key = hle_quality_sampler(st, &sd);
    ID3D11SamplerState *ss = (ID3D11SamplerState *)state_lookup(d.sampler, key);
    if (!ss) {
        if (SUCCEEDED(ID3D11Device_CreateSamplerState(g_hle.dev, &sd, &ss)))
            state_insert(d.sampler, key, ss);
    }
    return ss;
}

/* ------------------------------------------------------------- pixels */

static uint32_t pack_color(const float c[4])
{
    uint32_t r = (uint32_t)(fminf(fmaxf(c[0], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(fminf(fmaxf(c[1], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(fminf(fmaxf(c[2], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t a = (uint32_t)(fminf(fmaxf(c[3], 0.0f), 1.0f) * 255.0f + 0.5f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Pixel shader variants. The combiner shader above reads the register
 * combiner / fixed-function program from the constant buffer and branches
 * on it per pixel; at 4K that alone kept the GPU at 100%. Each distinct
 * program the title uses also gets a copy compiled with those words as
 * literals (static const in place of the cbuffer fields, same layout), so
 * the compiler folds every branch and loop away: same arithmetic, less
 * work per pixel. Colours that change every frame (combiner factors,
 * final constants, texture factor, fog) stay in the buffer. Variants
 * compile on a worker thread and the generic shader draws until one is
 * ready, so a new program never stalls a frame; they also go to the
 * on-disk cache. RECOMP_HLE_PS_VARIANTS=0 turns this off. */
#define PS_VARIANTS 4096
typedef struct {
    uint64_t key;
    volatile LONG state;                 /* 0 free, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *volatile ps;
    uint32_t words[4 * 8 + 4 + 16];      /* color/alpha icw/ocw, control, ff */
} PsVariant;
static PsVariant s_psv[PS_VARIANTS];
static HANDLE s_psv_event;
static volatile LONG s_psv_queue[256];
static volatile LONG s_psv_qw, s_psv_qr;
static int s_psv_on = -1;

static void ps_variant_words(const PsCB *p, uint32_t *w)
{
    memcpy(w, p->color_icw, 32);
    memcpy(w + 8, p->color_ocw, 32);
    memcpy(w + 16, p->alpha_icw, 32);
    memcpy(w + 24, p->alpha_ocw, 32);
    memcpy(w + 32, p->control, 16);
    memcpy(w + 36, p->ff, 64);
}

static int ps_variant_prefix(const uint32_t *w, char *out, size_t cap)
{
    static const char *const names[] = { "rcColorIcw", "rcColorOcw", "rcAlphaIcw", "rcAlphaOcw" };
    int n = snprintf(out, cap, "#define CB(x) x##_cb\n#define COMB_LOOP [unroll]\n"), k;
    for (k = 0; k < 4; k++)
        n += snprintf(out + n, cap - n, "static const uint4 %s[2]={uint4(%uu,%uu,%uu,%uu),uint4(%uu,%uu,%uu,%uu)};\n",
                      names[k], w[8 * k], w[8 * k + 1], w[8 * k + 2], w[8 * k + 3],
                      w[8 * k + 4], w[8 * k + 5], w[8 * k + 6], w[8 * k + 7]);
    n += snprintf(out + n, cap - n, "static const uint4 rcControl=uint4(%uu,%uu,%uu,%uu);\n",
                  w[32], w[33], w[34], w[35]);
    n += snprintf(out + n, cap - n, "static const uint4 ff[4]={");
    for (k = 0; k < 4; k++)
        n += snprintf(out + n, cap - n, "uint4(%uu,%uu,%uu,%uu)%s", w[36 + 4 * k], w[37 + 4 * k],
                      w[38 + 4 * k], w[39 + 4 * k], k < 3 ? "," : "};\n");
    return n;
}

static DWORD WINAPI ps_variant_worker(LPVOID unused)
{
    static char src[sizeof k_ps_source + 4096];
    (void)unused;
    for (;;) {
        PsVariant *v;
        ID3D11PixelShader *ps = NULL;
        const void *code;
        uint32_t size;
        int clip, n;
        uint64_t key;
        if (s_psv_qr == s_psv_qw) {
            WaitForSingleObject(s_psv_event, INFINITE);
            continue;
        }
        v = &s_psv[s_psv_queue[s_psv_qr & 255]];
        n = ps_variant_prefix(v->words, src, sizeof src);
        memcpy(src + n, k_ps_source, sizeof k_ps_source);
        key = fnv(src, (size_t)n + sizeof k_ps_source - 1, 0x84222325CBF29CE4ull);
        if (disk_find(key, &clip, &code, &size)) {
            if (FAILED(ID3D11Device_CreatePixelShader(g_hle.dev, code, size, NULL, &ps)))
                ps = NULL;
        }
        if (!ps) {
            ID3DBlob *b = compile(src, (size_t)n + sizeof k_ps_source - 1, "ps_5_0", "hle_ps_variant");
            if (b && SUCCEEDED(ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(b),
                                                              ID3D10Blob_GetBufferSize(b), NULL, &ps)))
                disk_store(key, 0, ID3D10Blob_GetBufferPointer(b), (uint32_t)ID3D10Blob_GetBufferSize(b));
            else
                ps = NULL;
            if (b) ID3D10Blob_Release(b);
        }
        v->ps = ps;
        InterlockedExchange(&v->state, ps ? 2 : 3);
        InterlockedIncrement(&s_psv_qr);
    }
}

/* The specialised shader for this draw's program, or NULL (use ps_main). */
static ID3D11PixelShader *ps_variant(const PsCB *p)
{
    uint32_t w[4 * 8 + 4 + 16];
    uint64_t key;
    unsigned i;
    if (s_psv_on < 0) {
        const char *e = getenv("RECOMP_HLE_PS_VARIANTS");
        s_psv_on = !(e && *e == '0');
        if (s_psv_on) {
            s_psv_event = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (!s_psv_event || !CreateThread(NULL, 0, ps_variant_worker, NULL, 0, NULL))
                s_psv_on = 0;
        }
    }
    if (!s_psv_on)
        return NULL;
    ps_variant_words(p, w);
    key = fnv(w, sizeof w, 0xCBF29CE484222325ull);
    for (i = 0; i < 32; i++) {
        PsVariant *v = &s_psv[(key + i) & (PS_VARIANTS - 1)];
        if (v->state && v->key == key)
            return v->state == 2 ? v->ps : NULL;
        if (!v->state) {
            if (s_psv_qw - s_psv_qr >= 255)
                return NULL;                 /* worker busy: try again next draw */
            v->key = key;
            memcpy(v->words, w, sizeof w);
            v->state = 1;
            s_psv_queue[s_psv_qw & 255] = (LONG)(v - s_psv);
            InterlockedIncrement(&s_psv_qw);
            SetEvent(s_psv_event);
            return NULL;
        }
    }
    return NULL;
}

static void setup_pixels(PsCB *p)
{
    ID3D11ShaderResourceView *srv[4] = { 0 };
    ID3D11SamplerState *smp[4] = { 0 };
    uint32_t i, used = 0, fc = RS(RS_FOGCOLOR), psdef = G_RENDER_STATE;

    memset(p, 0, sizeof *p);
    if (d.ps) {
        /* The definition the NV2A would hold: 0x29E6D0 is the library's copy
         * of it, but with device flag 0x10 that copy is never made and only
         * the constants SetPixelShaderConstant wrote since are current. */
        extern uint32_t hle_ps_constants_written;
        uint32_t def = G_RENDER_STATE, src = G_RENDER_STATE, written = 0x3FFFFu;
        if (MEM32(hle_device_va() + 8u) & 0x10u) {
            def = src = MEM32(d.ps + 8u);
            written = hle_ps_constants_written;
        }
        psdef = def;
        for (i = 0; i < 8; i++) {
            p->alpha_icw[i] = MEM32(def + 0x00 + 4 * i);
            p->factor0[i] = MEM32(((written >> i) & 1u ? G_RENDER_STATE : src) + 0x28 + 4 * i);
            p->factor1[i] = MEM32(((written >> (8 + i)) & 1u ? G_RENDER_STATE : src) + 0x48 + 4 * i);
            p->alpha_ocw[i] = MEM32(def + 0x68 + 4 * i);
            p->color_icw[i] = MEM32(def + 0x88 + 4 * i);
            p->color_ocw[i] = MEM32(def + 0xB4 + 4 * i);
        }
        p->final[0] = MEM32(def + 0x20);
        p->final[1] = MEM32(def + 0x24);
        p->final[2] = MEM32(((written >> 16) & 1u ? G_RENDER_STATE : src) + 0xAC);
        p->final[3] = MEM32(((written >> 17) & 1u ? G_RENDER_STATE : src) + 0xB0);
        p->control[0] = MEM32(def + 0xD4);
        p->control[1] = MEM32(hle_device_va() + 0x790u);
    } else {
        /* Fixed function: stage i samples with texture coordinate i. */
        for (i = 0; i < 4; i++) {
            uint32_t cop = TSS(i, TSS_COLOROP), aop = TSS(i, TSS_ALPHAOP);
            p->ff[i][0] = (cop & 255) | ((TSS(i, TSS_COLORARG1) & 255) << 8) | ((TSS(i, TSS_COLORARG2) & 255) << 16)
                        | ((TSS(i, TSS_COLORARG0) & 255) << 24);
            p->ff[i][1] = (aop & 255) | ((TSS(i, TSS_ALPHAARG1) & 255) << 8) | ((TSS(i, TSS_ALPHAARG2) & 255) << 16)
                        | ((TSS(i, TSS_ALPHAARG0) & 255) << 24);
            p->ff[i][2] = TSS(i, TSS_RESULTARG);
            if (cop > 1 && d.tex[i])
                p->control[1] |= 1u << (5 * i);
            if (cop <= 1)
                break;
        }
        p->control[3] |= 256;
        p->tfactor[0] = RS(RS_TEXTUREFACTOR);
    }
    for (i = 0; i < 4; i++) {
        uint32_t mode = (p->control[1] >> (5 * i)) & 31;
        uint32_t shader = MEM32(hle_device_va() + DEV_VERTEX_SHADER);
        uint32_t fixed_texgen = !(MEM8(shader + 4u) & 0x12u) && MEM32(hle_device_va() + 0x1928u) != 1u;
        uint32_t coord = d.ps || fixed_texgen ? i : TSS(i, 28u) & 3u;
        uint32_t sign = TSS(i, TSS_COLORSIGN) >> 28;
        p->ff[i][3] = coord | ((TSS(i, TSS_COLORKEYOP) & 3u) << 4) |
                        (TSS(i, TSS_ALPHAKILL) ? 256u : 0u) | ((sign & 15u) << 12);
        if (d.ps) {
            p->ff[i][3] |= ((MEM32(psdef + 0xA8u) >> (4u * i)) & 15u) << 16;
            if (i >= 2u) p->ff[i][3] |= ((MEM32(psdef + 0xE0u) >> (i == 2u ? 16u : 20u)) & 3u) << 20;
        }
        for (uint32_t k = 0; k < 4u; ++k)
            p->bump[i][k] = MEMF(G_TEXTURE_STATE + 128u * i + 4u * (22u + k));
        p->bump_luminance[i][0] = MEMF(G_TEXTURE_STATE + 128u * i + 104u);
        p->bump_luminance[i][1] = MEMF(G_TEXTURE_STATE + 128u * i + 108u);
        if (!mode || mode == 4 || !d.tex[i])
            continue;
        srv[i] = tex_get(d.tex[i], d.pal[i], p->tex_scale[i]);
        memcpy(&p->tex_scale[i][3], (const void *)XBOX_PTR(G_TEXTURE_STATE + 128u * i + 120u), 4);
        {
            uint32_t format = (MEM32(d.tex[i] + 12u) >> 8) & 255u;
            if (format == 3u || format == 7u || format == 0x1Cu || format == 0x1Eu)
                p->ff[i][3] |= 512u;
        }
        if (srv[i]) {
            used |= 1u << i;
            smp[i] = sampler_for(i);
        }
        if ((mode == 3 || mode > 4) && warn(3, 8))
            fprintf(stderr, "[HLE-D3D11] texture mode %u on stage %u approximated as 2D\n", mode, i);
    }
    p->control[2] = (NV(0x300) & 1) | (((NV(0x33C) - 0x200) & 7) << 8) | ((NV(0x340) & 255) << 16);
    p->control[3] |= used;
    p->fog[0] = ((fc >> 16) & 255) / 255.0f;
    p->fog[1] = ((fc >> 8) & 255) / 255.0f;
    p->fog[2] = (fc & 255) / 255.0f;
    p->fog[3] = 1.0f;
    /* D3DRS_FOGENABLE, FOGTABLEMODE, FOGSTART, FOGEND, FOGDENSITY. */
    p->tfactor[1] = RS(92) != 0;
    p->tfactor[2] = RS(93);
    {
        /* RECOMP_HLE_PSDEBUG: 1 magenta, 2 stage-0 texel, 3 diffuse, 4 colour
         * before the alpha test, 5 texcoord 0 (diagnostics only). */
        static int dbg = -1;
        if (dbg < 0) {
            const char *e = getenv("RECOMP_HLE_PSDEBUG");
            dbg = e ? atoi(e) : 0;
        }
        p->tfactor[3] = (uint32_t)dbg;
    }
    memcpy(&p->fogparm[0], (const void *)XBOX_PTR(G_RENDER_STATE + 4u * 94), 12);
    ID3D11DeviceContext_PSSetShaderResources(g_hle.ctx, 0, 4, srv);
    if (!s_bind.valid || memcmp(s_bind.smp, smp, sizeof smp)) {
        ID3D11DeviceContext_PSSetSamplers(g_hle.ctx, 0, 4, smp);
        memcpy(s_bind.smp, smp, sizeof smp);
    }
}

/* -------------------------------------------------------------- uploads */

/* A full ring under Vulkan. MAP_DISCARD of the 32 MB vertex ring is free on a
 * D3D11 driver (it renames the buffer) but cost DXVK ~17 ms -- one dropped
 * frame every time the ring wrapped (~24 frames in a match). So with DXVK the
 * ring is three buffers, written with NO_OVERWRITE only: at a wrap an event
 * query marks the end of the full one and the next is reused once its own
 * query (issued a full ring earlier, normally long done) has passed. */
#define RINGS 3

static void ring_next(int *cur, ID3D11Query **q, int *issued)
{
    ID3D11DeviceContext_End(g_hle.ctx, (ID3D11Asynchronous *)q[*cur]);
    issued[*cur] = 1;
    *cur = (*cur + 1) % RINGS;
    if (issued[*cur]) {
        BOOL done = FALSE;
        while (ID3D11DeviceContext_GetData(g_hle.ctx, (ID3D11Asynchronous *)q[*cur], &done, sizeof done, 0) == S_FALSE)
            YieldProcessor();
    }
}

static int upload(const void *const *src, const uint32_t *len, unsigned n, uint32_t *base)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t total = 0, i, pos;
    for (i = 0; i < n; i++)
        total += (len[i] + 15) & ~15u;
    if (total > d.vb_size)
        return 0;
    if (d.rings) {
        if (d.vb_pos + total > d.vb_size) {
            ring_next(&d.vb_cur, d.vb_q, d.vb_q_issued);
            d.vb = d.vb_ring[d.vb_cur];
            d.vb_srv = d.vb_ring_srv[d.vb_cur];
            d.vb_pos = 0;
        }
        if (FAILED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.vb, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
            return 0;
    } else if (!d.vb_no_overwrite || d.vb_pos + total > d.vb_size) {
        if (FAILED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return 0;
        d.vb_pos = 0;
    } else if (FAILED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.vb, 0,
                                              D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
        return 0;
    }
    pos = d.vb_pos;
    for (i = 0; i < n; i++) {
        memcpy((uint8_t *)m.pData + pos, src[i], len[i]);
        base[i] = pos;
        pos += (len[i] + 15) & ~15u;
    }
    ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)d.vb, 0);
    d.vb_pos = pos;
    return 1;
}

static int upload_indices(const uint32_t *idx, uint32_t n, uint32_t *first)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t bytes = n * 4;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (bytes > d.ib_size)
        return 0;
    if (d.ib_pos + bytes > d.ib_size && d.rings) {
        ring_next(&d.ib_cur, d.ib_q, d.ib_q_issued);
        d.ib = d.ib_ring[d.ib_cur];
        d.ib_pos = 0;
    } else if (d.ib_pos + bytes > d.ib_size) {
        mode = D3D11_MAP_WRITE_DISCARD;
        d.ib_pos = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.ib, 0, mode, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + d.ib_pos, idx, bytes);
    ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)d.ib, 0);
    *first = d.ib_pos / 4;
    d.ib_pos += bytes;
    return 1;
}

/* Xbox primitive (NV097 BEGIN_END op) + vertex list -> D3D11 list/strip. */
static uint32_t convert_prim(uint32_t prim, const uint32_t *in, uint32_t n, uint32_t *out,
                             D3D11_PRIMITIVE_TOPOLOGY *topo)
{
    uint32_t i, k = 0;
    switch (prim) {
    case 1: *topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; memcpy(out, in, n * 4); return n;
    case 2: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; memcpy(out, in, n * 4); return n & ~1u;
    case 3: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; memcpy(out, in, n * 4);
        if (n) out[n] = in[0];
        return n ? n + 1 : 0;
    case 4: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; memcpy(out, in, n * 4); return n;
    case 5: *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; memcpy(out, in, n * 4); return n - n % 3;
    case 6: case 9: *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; memcpy(out, in, n * 4); return n;
    case 7: case 10:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 2; i < n; i++) { out[k++] = in[0]; out[k++] = in[i - 1]; out[k++] = in[i]; }
        return k;
    case 8:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (i = 0; i + 3 < n; i += 4) {
            out[k++] = in[i]; out[k++] = in[i + 1]; out[k++] = in[i + 2];
            out[k++] = in[i]; out[k++] = in[i + 2]; out[k++] = in[i + 3];
        }
        return k;
    default:
        return 0;
    }
}

/* --------------------------------------------------------------- draws */

typedef struct {
    const uint8_t *host;              /* custom single stream (inline/immediate) or NULL */
    uint32_t bytes, stride;
    uint32_t attr_off[16], attr_fmt[16];
} CustomStream;

static void draw(uint32_t prim, const uint32_t *verts, uint32_t n, const CustomStream *cs)
{
    uint32_t dev = hle_device_va(), vs_obj = MEM32(dev + DEV_VERTEX_SHADER);
    HleSurface *rt = hle_surface_for(MEM32(dev + DEV_RENDER_TARGET));
    HleSurface *ds = hle_surface_for(MEM32(dev + DEV_DEPTH_STENCIL));
    HleVs *v = vs_find(vs_obj);
    ID3D11VertexShader *vsh;
    DrawCB cb;
    PsCB pcb;
    static uint32_t *idx;
    static uint32_t idx_cap;
    uint32_t vmin = 0xFFFFFFFFu, vmax = 0, i, nidx, first, dbase[16];
    D3D11_PRIMITIVE_TOPOLOGY topo;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    const void *src[16];
    uint32_t len[16], nsrc = 0, stream_slot[16];

    d.vp.x = MEM32(dev + 0xEE0); d.vp.y = MEM32(dev + 0xEE4);
    d.vp.w = MEM32(dev + 0xEE8); d.vp.h = MEM32(dev + 0xEEC);
    d.vp.minz = MEMF(dev + 0xEF0); d.vp.maxz = MEMF(dev + 0xEF4);
    for (uint32_t stage = 0; stage < 4; stage++) {
        d.tex[stage] = MEM32(dev + 0xF88 + stage * 4);
        d.pal[stage] = MEM32(dev + 0xF98 + stage * 4);
    }
    d.ps = MEM32(dev + 0x784);

    if (!n)
        return;
    if (!rt || !rt->rtv || !v) {
        d.skipped++;
        if (!v && warn(4, 8))
            fprintf(stderr, "[HLE-D3D11] draw with unknown vertex shader %08X\n", vs_obj);
        return;
    }
    if (!(vsh = vs_get(v))) {
        d.skipped++;
        return;
    }
    if (ds && (!ds->dsv || ds->w != rt->w || ds->h != rt->h))
        ds = NULL;
    if (s_rtlog) {
        if (s_rtlog < 0) {
            const char *e = getenv("RECOMP_HLE_RTLOG");
            s_rtlog = e && *e && *e != '0';
        }
        if (s_rtlog > 0)
            rtlog_draw(rt->data, d.ps, vs_obj);
        s_rtcur = rt->data;
    }

    for (i = 0; i < n; i++) {
        if (verts[i] < vmin) vmin = verts[i];
        if (verts[i] > vmax) vmax = verts[i];
    }
    memset(&cb, 0, sizeof cb);
    memcpy(cb.imm, d.attr, sizeof cb.imm);
    if (cs) {
        src[0] = cs->host;
        len[0] = cs->bytes;
        nsrc = 1;
    } else {
        memset(stream_slot, 0xFF, sizeof stream_slot);
        for (i = 0; i < 16; i++) {
            uint32_t fmt = MEM32(vs_obj + 28 + 16 * i), sidx = MEM32(vs_obj + 20 + 16 * i) & 15;
            uint32_t stride, vb, data, from, bytes;
            if (fmt == 0 || fmt == 2 || stream_slot[sidx] != 0xFFFFFFFFu)
                continue;
            stride = MEM32(G_STREAM + 12 * sidx);
            vb = MEM32(G_STREAM + 12 * sidx + 8);
            if (!vb)
                continue;
            data = contig(MEM32(vb + 4)) + MEM32(G_STREAM + 12 * sidx + 4);
            from = stride ? vmin * stride : 0;
            bytes = stride ? (vmax - vmin + 1) * stride : 64;
            if (bytes > (16u << 20)) {
                d.skipped++;
                if (warn(5, 8))
                    fprintf(stderr, "[HLE-D3D11] draw skipped: stream %u range %u..%u x %u\n", sidx, vmin, vmax, stride);
                return;
            }
            stream_slot[sidx] = nsrc;
            src[nsrc] = (const void *)XBOX_PTR(data + from);
            len[nsrc] = bytes + 16;           /* slack for unaligned tail reads */
            nsrc++;
        }
    }
    if (!upload(src, len, nsrc, dbase)) {
        d.skipped++;
        return;
    }
    for (i = 0; i < 16; i++) {
        if (cs) {
            if (!cs->attr_fmt[i])
                continue;
            cb.attr[i][0] = dbase[0] + cs->attr_off[i];
            cb.attr[i][1] = cs->stride;
            cb.attr[i][2] = cs->attr_fmt[i];
        } else {
            uint32_t fmt = MEM32(vs_obj + 28 + 16 * i), sidx = MEM32(vs_obj + 20 + 16 * i) & 15;
            uint32_t stride = MEM32(G_STREAM + 12 * sidx);
            if (fmt == 0 || fmt == 2 || stream_slot[sidx] == 0xFFFFFFFFu)
                continue;
            cb.attr[i][0] = dbase[stream_slot[sidx]] - (stride ? vmin * stride : 0) + MEM32(vs_obj + 24 + 16 * i);
            cb.attr[i][1] = stride;
            cb.attr[i][2] = fmt;
        }
    }

    if (idx_cap < n * 2 + 8) {
        idx_cap = n * 2 + 8;
        idx = (uint32_t *)realloc(idx, idx_cap * 4);
    }
    nidx = convert_prim(prim, verts, n, idx, &topo);
    if (!nidx || !upload_indices(idx, nidx, &first)) {
        d.skipped++;
        return;
    }

    /* Viewport: the game's for clip-space shaders, the whole target for
     * shaders that already produce screen positions. */
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    /* On a widened target (hle_surface_for) the viewport is xk times wider
     * around the same centre and the shader multiplies clip x by 1/xk: every
     * game pixel lands where hle_map_x puts it, and clip space beyond the
     * 4:3 edges fills the sides. */
    if (v->clip_space && d.vp.w && d.vp.h) {
        vp.Width = (float)d.vp.w * g_hle.scale * rt->xk;
        vp.TopLeftX = hle_map_x(rt, (float)d.vp.x + (float)d.vp.w * 0.5f) - vp.Width * 0.5f;
        vp.TopLeftY = (float)d.vp.y * g_hle.scale;
        vp.Height = (float)d.vp.h * g_hle.scale;
        vp.MinDepth = d.vp.minz;
        vp.MaxDepth = d.vp.maxz;
    } else {
        vp.TopLeftX = vp.TopLeftY = 0.0f;
        vp.Width = (float)rt->tw;
        vp.Height = (float)rt->th;
    }
    cb.ss[0] = 2.0f / (float)rt->w;
    cb.ss[1] = -2.0f / (float)rt->h;
    cb.ss[3] = g_hle.anamorphic ? 1.0f : 1.0f / rt->xk;  /* anamorphic: no squeeze, no edge band */
    cb.ss[2] = ds && ((ds->format >> 8) & 0xFF) >= 0x2C && ((ds->format >> 8) & 0xFF) != 0x2E &&
               ((ds->format >> 8) & 0xFF) != 0x2F ? 1.0f / 65535.0f : 1.0f / 16777215.0f;

    if (d.vsc_dirty) {
        ID3D11DeviceContext_UpdateSubresource(g_hle.ctx, (ID3D11Resource *)d.cb_vsc, 0, NULL, d.vsc, 0, 0);
        d.vsc_dirty = 0;
    }
    if (s_cb_skip < 0) {
        const char *e = getenv("RECOMP_HLE_NO_CB_SKIP");
        s_cb_skip = !(e && *e && *e != '0');
        e = getenv("RECOMP_HLE_NO_STATE_CACHE");
        s_bind.off = e && *e && *e != '0';
    }
    if (s_bind.off || hle_ab_off)
        s_bind.valid = 0;
    {
        static DrawCB last;
        if (!s_cb_skip || hle_ab_off || !s_cb_draw_valid || memcmp(&last, &cb, sizeof cb)) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &cb, sizeof cb);
                ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)d.cb_draw, 0);
                memcpy(&last, &cb, sizeof cb);
                s_cb_draw_valid = 1;
            }
        }
    }
    setup_pixels(&pcb);
    {
        extern int hle_trace_on(void);
        if (hle_trace_on()) {
            fprintf(stderr, "[HLE-D] draw prim %u n %u vs %08X ps %08X ctl %08X %08X %08X %08X rt %08X ds %s\n",
                    prim, n, vs_obj, d.ps, pcb.control[0], pcb.control[1], pcb.control[2], pcb.control[3],
                    rt->data, ds ? "yes" : "no");
            fprintf(stderr, "[HLE-D]   %s vp game %u,%u %ux%u z %.3f..%.3f -> native x %.1f w %.1f y %.1f h %.1f (rt %ux%u native %ux%u)\n",
                    v->clip_space ? "clip" : "screen", d.vp.x, d.vp.y, d.vp.w, d.vp.h, d.vp.minz, d.vp.maxz,
                    vp.TopLeftX, vp.Width, vp.TopLeftY, vp.Height, rt->w, rt->h, rt->tw, rt->th);
            if (d.ps)
                fprintf(stderr, "[HLE-D]   ps def %08X count %08X, render-state copy %08X, device flags %08X\n",
                        MEM32(d.ps + 8), MEM32(MEM32(d.ps + 8) + 0xD4), RS(0x35), MEM32(hle_device_va() + 8));
            {
                static uint64_t rs_dumped;
                if (rs_dumped != g_hle.swaps + 1) {
                    rs_dumped = g_hle.swaps + 1;
                    fprintf(stderr, "[HLE-D] RS[84..140]:");
                    for (i = 84; i <= 140; i++)
                        fprintf(stderr, " %u=%08X", i, RS(i));
                    fprintf(stderr, "\n");
                }
            }
            for (i = 0; i < 4; i++)
                if (d.tex[i])
                    fprintf(stderr, "[HLE-D]   stage %u tex %08X data %08X fmt %08X size %08X pal %08X\n", i,
                            d.tex[i], MEM32(d.tex[i] + 4), MEM32(d.tex[i] + 12), MEM32(d.tex[i] + 16), d.pal[i]);
        }
    }
    {
        static PsCB last;
        if (!s_cb_skip || hle_ab_off || !s_cb_ps_valid || memcmp(&last, &pcb, sizeof pcb)) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)d.cb_ps, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &pcb, sizeof pcb);
                ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)d.cb_ps, 0);
                memcpy(&last, &pcb, sizeof pcb);
                s_cb_ps_valid = 1;
            }
        }
    }
    extern uint32_t hle_geometry_flags(uint32_t primitive, D3D11_PRIMITIVE_TOPOLOGY topology);
    extern int hle_geometry_bind(uint32_t primitive, D3D11_PRIMITIVE_TOPOLOGY topology, const D3D11_VIEWPORT *vp);
    uint32_t geometry = hle_geometry_flags(prim, topo);
    if (geometry && hle_geometry_bind(prim, topo, &vp) <= 0) {
        ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
        s_bind.gs_null = 1;
        geometry = 0;
    } else if (geometry) {
        s_bind.gs_null = 0;
    } else if (!s_bind.valid || !s_bind.gs_null) {
        ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
        s_bind.gs_null = 1;
    }
    apply_output_state(ds != NULL, geometry);

    {
        ID3D11DepthStencilView *dsv = ds ? ds->dsv : NULL;
        ID3D11PixelShader *psv = ps_variant(&pcb);
        if (!psv)
            psv = d.ps_main;
        if (!s_bind.valid) {
            /* Never change between draws: the input layout, the constant
             * buffers (their contents are updated in place). */
            ID3D11Buffer *vcb[2] = { d.cb_vsc, d.cb_draw };
            ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
            ID3D11DeviceContext_VSSetConstantBuffers(g_hle.ctx, 0, 2, vcb);
            ID3D11DeviceContext_PSSetConstantBuffers(g_hle.ctx, 0, 1, &d.cb_ps);
        }
        if (!s_bind.valid || s_bind.rtv != rt->rtv || s_bind.dsv != dsv) {
            ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, 1, &rt->rtv, dsv);
            s_bind.rtv = rt->rtv;
            s_bind.dsv = dsv;
        }
        if (!s_bind.valid || memcmp(&s_bind.vp, &vp, sizeof vp)) {
            ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &vp);
            s_bind.vp = vp;
        }
        if (!s_bind.valid || s_bind.ib != d.ib) {
            ID3D11DeviceContext_IASetIndexBuffer(g_hle.ctx, d.ib, DXGI_FORMAT_R32_UINT, 0);
            s_bind.ib = d.ib;
        }
        if (!s_bind.valid || s_bind.topo != topo) {
            ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, topo);
            s_bind.topo = topo;
        }
        if (!s_bind.valid || s_bind.vs != vsh) {
            ID3D11DeviceContext_VSSetShader(g_hle.ctx, vsh, NULL, 0);
            s_bind.vs = vsh;
        }
        if (!s_bind.valid || s_bind.vb_srv != d.vb_srv) {
            ID3D11DeviceContext_VSSetShaderResources(g_hle.ctx, 0, 1, &d.vb_srv);
            s_bind.vb_srv = d.vb_srv;
        }
        if (!s_bind.valid || s_bind.ps != psv) {
            ID3D11DeviceContext_PSSetShader(g_hle.ctx, psv, NULL, 0);
            s_bind.ps = psv;
        }
        s_bind.valid = !s_bind.off && !hle_ab_off;
    }
    ID3D11DeviceContext_DrawIndexed(g_hle.ctx, nidx, first, 0);
    if (geometry) {
        ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
        s_bind.gs_null = 1;
    }
    /* Capture the first successfully submitted native draw at the next swap.
     * This distinguishes bad HLE state/shaders from a game that never submits
     * another frame, without dumping hundreds of full-size surfaces. */
    {
        static int dump_first_draw = -1;
        if (dump_first_draw < 0) {
            const char *e = getenv("RECOMP_HLE_DUMP_FIRST_DRAW");
            dump_first_draw = e && atoi(e) != 0;
        }
        if (dump_first_draw) {
            extern volatile int hle_dump_request;
            hle_dump_request = 1;
            dump_first_draw = 0;
        }
    }
    /* RECOMP_HLE_SEAMPROBE (traced frames, widened targets): mean brightness
     * just inside and just outside the right 4:3 edge, lower half, after
     * each draw -- which draw makes the two sides differ. */
    if (hle_trace_on() && rt->xk > 1.01f && getenv("RECOMP_HLE_SEAMPROBE")) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE sm;
        UINT e = (UINT)(hle_map_x(rt, (float)rt->w) + 0.5f);
        ID3D11Texture2D_GetDesc(rt->tex, &td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        if (e > 12 && e + 12 < rt->tw && SUCCEEDED(ID3D11Device_CreateTexture2D(g_hle.dev, &td, NULL, &st))) {
            ID3D11DeviceContext_CopyResource(g_hle.ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt->tex);
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &sm))) {
                double in = 0, out = 0;
                UINT y, x;
                for (y = rt->th / 2; y < rt->th; y++) {
                    const uint8_t *row = (const uint8_t *)sm.pData + (size_t)y * sm.RowPitch;
                    for (x = e - 10; x < e - 2; x++) in += row[4 * x] + row[4 * x + 1] + row[4 * x + 2];
                    for (x = e + 2; x < e + 10; x++) out += row[4 * x] + row[4 * x + 1] + row[4 * x + 2];
                }
                in /= 24.0 * (rt->th - rt->th / 2); out /= 24.0 * (rt->th - rt->th / 2);
                fprintf(stderr, "[SEAM] draw %llu: inside %.1f outside %.1f diff %+.1f\n",
                        (unsigned long long)d.draws, in, out, out - in);
                ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)st, 0);
            }
            ID3D11Texture2D_Release(st);
        }
    }
    d.draws++;
    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
        d.tris += nidx / 3;
    else if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && nidx > 2)
        d.tris += nidx - 2;
}

static double s_draw_ms;
static int s_measure_draws;

double hle_draw_frame_draw_ms(void)
{
    double v = s_draw_ms;
    s_draw_ms = 0.0;
    return v;
}

static void draw_api(uint32_t prim, uint32_t count, uint32_t indices, uint32_t start)
{
    static uint32_t *verts;
    static uint32_t cap;
    uint32_t i;
    if (!count || count > (1u << 20))
        return;
    if (cap < count) {
        cap = count;
        verts = (uint32_t *)realloc(verts, cap * 4);
    }
    if (indices) {
        const uint16_t *ix = (const uint16_t *)XBOX_PTR(indices);
        for (i = 0; i < count; i++)
            verts[i] = ix[i];
    } else {
        for (i = 0; i < count; i++)
            verts[i] = start + i;
    }
    if (s_measure_draws) {
        double t0 = now_ms();
        draw(prim, verts, count, NULL);
        s_draw_ms += now_ms() - t0;
    } else
        draw(prim, verts, count, NULL);
}

/* Inline vertices (immediate mode, BeginPush INLINE_ARRAY) through the same
 * path as one custom stream. */
static void draw_custom(uint32_t prim, CustomStream *cs)
{
    static uint32_t *verts;
    static uint32_t cap;
    uint32_t n = cs->stride ? cs->bytes / cs->stride : 0, i;
    if (!n)
        return;
    if (cap < n) {
        cap = n;
        verts = (uint32_t *)realloc(verts, cap * 4);
    }
    for (i = 0; i < n; i++)
        verts[i] = i;
    if (s_measure_draws) {
        double t0 = now_ms();
        draw(prim, verts, n, cs);
        s_draw_ms += now_ms() - t0;
    } else
        draw(prim, verts, n, cs);
}

static uint32_t attr_size(uint32_t fmt);

/* Host-owned engine vertices; no GPU command header or method is read. */
void hle_draw_engine_batch(uint32_t primitive, const uint8_t *vertices, uint32_t count, uint32_t stride)
{
    CustomStream cs;
    uint32_t offset=0;
    extern void hle_port_native_missing(const char *, unsigned);
    EnterCriticalSection(&g_hle.lock);
    uint32_t vs_obj=MEM32(hle_device_va()+DEV_VERTEX_SHADER);
    memset(&cs,0,sizeof cs);
    cs.host=vertices;
    cs.bytes=count*stride;
    cs.stride=stride;
    for(uint32_t i=0;i<16;i++) {
        uint32_t fmt=MEM32(vs_obj+28+16*i);
        if(!fmt || fmt==2) continue;
        uint32_t size=attr_size(fmt);
        if(!size || offset+size>stride) hle_port_native_missing("engine batch vertex declaration",0);
        cs.attr_off[i]=offset;
        cs.attr_fmt[i]=fmt;
        offset+=size;
    }
    if(offset!=stride) hle_port_native_missing("engine batch declaration does not match its vertex size",0);
    draw_custom(primitive,&cs);
    LeaveCriticalSection(&g_hle.lock);
}

static void flush_immediate(void)
{
    CustomStream cs;
    uint32_t i;
    if (!d.imm_prim)
        return;
    memset(&cs, 0, sizeof cs);
    cs.host = (const uint8_t *)d.imm;
    cs.stride = 256;
    cs.bytes = d.imm_count * 256;
    for (i = 0; i < 16; i++) {
        cs.attr_off[i] = 16 * i;
        cs.attr_fmt[i] = 0x42;
    }
    draw_custom(d.imm_prim, &cs);
    d.imm_prim = 0;
    d.imm_count = 0;
}

static uint32_t attr_size(uint32_t fmt)
{
    uint32_t n = fmt >> 4, ty = fmt & 15;
    switch (ty) {
    case 2: return fmt == 0x72 ? 12 : n * 4;
    case 0: case 6: return 4;
    case 1: case 5: return n * 2;
    case 4: return n;
    default: return 0;
    }
}

/* Game-written push data between BeginPush and EndPush: NV097 methods. The
 * draw methods become native draws; any other register write updates the
 * shadow like SetRenderState_Simple would. */
static void decode_push(uint32_t begin, uint32_t end)
{
    extern void hle_port_native_missing(const char *, unsigned);
    if (end > begin)
        hle_port_native_missing("NV097 push batch (replace the game producer with native draws)", MEM32(g_esp));
    static uint8_t *inl;
    static uint32_t inl_cap;
    uint32_t p = begin, prim = 0, inl_len = 0, dev = hle_device_va(), vs_obj = MEM32(dev + DEV_VERTEX_SHADER);
    CustomStream cs;
    uint32_t i, off = 0;

    memset(&cs, 0, sizeof cs);
    for (i = 0; i < 16; i++) {
        uint32_t fmt = MEM32(vs_obj + 28 + 16 * i);
        if (fmt == 0 || fmt == 2)
            continue;
        cs.attr_off[i] = off;
        cs.attr_fmt[i] = fmt;
        off += attr_size(fmt);
    }
    cs.stride = off;

    while (p < end) {
        uint32_t h = MEM32(p), count = (h >> 18) & 0x7FF, method = h & 0x1FFC, k;
        int ni = (h & 0x40000000u) != 0;
        p += 4;
        if ((h & 0xE0000003u) == 0x20000000u || (h & 3) == 1 || (h & 3) == 2) {
            if (warn(6, 8))
                fprintf(stderr, "[HLE-D3D11] push: jump/call %08X not followed\n", h);
            break;
        }
        for (k = 0; k < count && p < end; k++, p += 4) {
            uint32_t m = ni ? method : method + 4 * k, val = MEM32(p);
            if (hle_trace_on() && (m != 0x1818 || inl_len < 64))
                fprintf(stderr, "[HLE-T]   push %04X = %08X\n", m, val);
            if (m == 0x17FC) {
                if (val) {
                    prim = val;
                    inl_len = 0;
                } else if (prim) {
                    cs.host = inl;
                    cs.bytes = inl_len;
                    if (cs.stride)
                        draw_custom(prim, &cs);
                    prim = 0;
                }
            } else if (m == 0x1818) {
                if (inl_len + 4 > inl_cap) {
                    inl_cap = inl_cap ? inl_cap * 2 : 4096;
                    inl = (uint8_t *)realloc(inl, inl_cap);
                }
                memcpy(inl + inl_len, &val, 4);
                inl_len += 4;
            } else if (m < 0x2000) {
                d.nv[m >> 2] = val;
            }
        }
    }
}

/* ---------------------------------------------------------- API mirror */

void hle_draw_decode_push(uint32_t begin, uint32_t end)
{
    EnterCriticalSection(&g_hle.lock);
    decode_push(begin, end);
    LeaveCriticalSection(&g_hle.lock);
}

void hle_draw_flush(void)
{
    if (d.ready)
        flush_immediate();
}

void hle_native_observe(int id, int depth, int phase, const uint32_t a[6],
                        uint32_t ecx, uint32_t edx, uint32_t eax)
{
    uint32_t i;
    if (!hle_native_enabled())
        return;
    EnterCriticalSection(&g_hle.lock);
    if (!d.ready) {
        nv_defaults();
        for (i = 0; i < 16; i++)
            d.attr[i][3] = 1.0f;
        d.attr[3][0] = d.attr[3][1] = d.attr[3][2] = 1.0f;   /* diffuse: white */
        d.ready = 1;
    }
    /* Any other call ends an immediate-mode primitive (D3DDevice_End is
     * inlined by the game, not routed). */
    if (depth == 0 && phase == 0 && d.imm_prim && g_hle.dev &&
        id != HLE_D3DDevice_SetVertexData2f && id != HLE_D3DDevice_SetVertexData4f &&
        id != HLE_D3DDevice_SetVertexDataColor)
        flush_immediate();

    switch (id) {
    case HLE_D3DDevice_SetRenderState_Simple:
        if (phase == 0)
            d.nv[(ecx & 0x1FFC) >> 2] = edx;
        break;
    case HLE_D3DDevice_SetVertexShaderConstant1:
    case HLE_D3DDevice_SetVertexShaderConstant4:
    case HLE_D3DDevice_SetVertexShaderConstantNotInline:
        if (phase == 0) {
            /* NotInline's stack argument is a dword count (its body copies
             * it with rep movsd); it was read as a constant count, which
             * filled the registers after the real ones with whatever followed
             * the source array. */
            uint32_t n = id == HLE_D3DDevice_SetVertexShaderConstant1 ? 1
                       : id == HLE_D3DDevice_SetVertexShaderConstant4 ? 4 : (a[0] + 3) / 4;
            for (i = 0; i < n && ecx + i < 192; i++) {
                uint32_t k;
                for (k = 0; k < 4; k++) {
                    uint32_t w = MEM32(edx + 16 * i + 4 * k);
                    if (!s_cb_skip || hle_ab_off || memcmp(&d.vsc[ecx + i][k], &w, 4)) {
                        memcpy(&d.vsc[ecx + i][k], &w, 4);
                        d.vsc_dirty = 1;        /* re-uploaded only when a value changed */
                    }
                }
            }
        }
        break;
    case HLE_D3DDevice_CreateVertexShader:
        if (phase == 1 && a[2])
            vs_register(MEM32(a[2]), a[1]);
        break;
    case HLE_D3DDevice_DeleteVertexShader:
        if (phase == 0) {
            HleVs *v = vs_find(a[0] & ~1u);
            if (v) vs_drop(v);
        }
        break;
    case HLE_D3DDevice_SetPixelShader:
        if (phase == 0) {
            d.ps = a[0];
            d.psc_set = 0;
        }
        break;
    case HLE_D3DDevice_SetPixelShaderConstant:
        if (phase == 0)
            for (i = 0; i < a[2] && a[0] + i < 16; i++) {
                uint32_t k;
                for (k = 0; k < 4; k++) {
                    uint32_t w = MEM32(a[1] + 16 * i + 4 * k);
                    memcpy(&d.psc[a[0] + i][k], &w, 4);
                }
                d.psc_set |= 1u << (a[0] + i);
            }
        break;
    case HLE_D3DDevice_SetTexture:
        if (phase == 0 && a[0] < 4)
            d.tex[a[0]] = a[1];
        break;
    case HLE_D3DDevice_SetPalette:
        if (phase == 0 && a[0] < 4)
            d.pal[a[0]] = a[1];
        break;
    case HLE_D3DDevice_SetViewport:
        if (phase == 0 && a[0]) {
            d.vp.x = MEM32(a[0]);
            d.vp.y = MEM32(a[0] + 4);
            d.vp.w = MEM32(a[0] + 8);
            d.vp.h = MEM32(a[0] + 12);
            memcpy(&d.vp.minz, (const void *)XBOX_PTR(a[0] + 16), 4);
            memcpy(&d.vp.maxz, (const void *)XBOX_PTR(a[0] + 20), 4);
        }
        break;
    case HLE_D3DDevice_SetVertexData2f:
    case HLE_D3DDevice_SetVertexData4f:
    case HLE_D3DDevice_SetVertexDataColor:
        /* D3DVSDE_VERTEX (-1) is the position register and emits a vertex. */
        if (phase == 0 && depth == 0 && (a[0] < 16 || a[0] == 0xFFFFFFFFu)) {
            uint32_t reg = a[0] < 16 ? a[0] : 0;
            float *o = d.attr[reg];
            if (id == HLE_D3DDevice_SetVertexDataColor) {
                o[0] = ((a[1] >> 16) & 255) / 255.0f; o[1] = ((a[1] >> 8) & 255) / 255.0f;
                o[2] = (a[1] & 255) / 255.0f; o[3] = (a[1] >> 24) / 255.0f;
            } else {
                memcpy(&o[0], &a[1], 4);
                memcpy(&o[1], &a[2], 4);
                if (id == HLE_D3DDevice_SetVertexData4f) {
                    memcpy(&o[2], &a[3], 4);
                    memcpy(&o[3], &a[4], 4);
                } else {
                    o[2] = 0.0f;
                    o[3] = 1.0f;
                }
            }
            if (d.imm_prim && reg == 0) {
                if (d.imm_count == d.imm_cap) {
                    d.imm_cap = d.imm_cap ? d.imm_cap * 2 : 256;
                    d.imm = (float (*)[16][4])realloc(d.imm, d.imm_cap * sizeof *d.imm);
                }
                memcpy(d.imm[d.imm_count++], d.attr, sizeof d.attr);
            }
        }
        break;
    case HLE_D3DDevice_Begin:
        if (phase == 0 && depth == 0) {
            d.imm_prim = a[0];
            d.imm_count = 0;
        }
        break;
    case HLE_D3DDevice_BeginPush_4:
        if (phase == 1 && depth == 0)
            d.push_begin = eax;
        break;
    case HLE_D3DDevice_EndPush:
        if (phase == 0 && depth == 0 && d.push_begin && g_hle.dev) {
            decode_push(d.push_begin, a[0]);
            d.push_begin = 0;
        }
        break;
    case HLE_D3DDevice_DrawVertices:
        if (phase == 0 && depth == 0 && g_hle.dev)
            draw_api(a[0], a[2], 0, a[1]);
        break;
    case HLE_D3DDevice_DrawIndexedVertices:
        if (phase == 0 && depth == 0 && g_hle.dev)
            draw_api(a[0], a[1], a[2], 0);
        break;
    default:
        break;
    }
    LeaveCriticalSection(&g_hle.lock);
}

/* ------------------------------------------------------- partial clears */

/* Clear of a rectangle smaller than the surface. D3D11 clears views whole
 * (ClearView: colour only), so colour, depth and stencil are written by a
 * full-target triangle scissored to the rectangle: depth through SV_Depth
 * with the test off, stencil by REPLACE with the clear value as reference. */
static const char k_clear_vs[] =
"float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);"
"return float4(p*float2(2,-2)+float2(-1,1),0,1);}\n";
static const char k_clear_ps[] =
"cbuffer C:register(b0){float4 col;float4 zz;}\n"
"struct O{float4 c:SV_Target;float d:SV_Depth;};\n"
"O main(float4 p:SV_Position){O o;o.c=col;o.d=zz.x;return o;}\n";

void hle_draw_clear_rect(HleSurface *rt, HleSurface *ds, const D3D11_RECT *r, uint32_t flags,
                         const float c[4], float z, uint32_t stencil)
{
    static ID3D11VertexShader *vs;
    static ID3D11PixelShader *ps;
    static ID3D11Buffer *cb;
    static ID3D11RasterizerState *rs;
    static int failed;
    ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *dss = NULL;
    D3D11_BLEND_DESC b;
    D3D11_DEPTH_STENCIL_DESC z_desc;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    HleSurface *size_of = rt ? rt : ds;
    UINT8 wm = 0;

    if (failed || !size_of)
        return;
    hle_draw_state_invalidate();
    if (!vs) {
        ID3DBlob *v = compile(k_clear_vs, sizeof k_clear_vs - 1, "vs_5_0", "hle_clear_vs");
        ID3DBlob *p = compile(k_clear_ps, sizeof k_clear_ps - 1, "ps_5_0", "hle_clear_ps");
        D3D11_BUFFER_DESC bd;
        D3D11_RASTERIZER_DESC rd;
        if (!v || !p ||
            FAILED(ID3D11Device_CreateVertexShader(g_hle.dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &ps))) {
            failed = 1;
            fprintf(stderr, "[HLE-D3D11] partial clear shaders unavailable\n");
        }
        if (v) ID3D10Blob_Release(v);
        if (p) ID3D10Blob_Release(p);
        if (failed)
            return;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &cb);
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(g_hle.dev, &rd, &rs);
    }
    if (flags & 0x10u) wm |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (flags & 0x20u) wm |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (flags & 0x40u) wm |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (flags & 0x80u) wm |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    memset(&b, 0, sizeof b);
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = rt ? wm : 0;
    memset(&z_desc, 0, sizeof z_desc);
    z_desc.DepthEnable = (ds && (flags & 1u)) ? TRUE : FALSE;
    z_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    z_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    z_desc.StencilEnable = (ds && (flags & 2u)) ? TRUE : FALSE;
    z_desc.StencilReadMask = 0xFF;
    z_desc.StencilWriteMask = 0xFF;
    z_desc.FrontFace.StencilFunc = z_desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    z_desc.FrontFace.StencilPassOp = z_desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    z_desc.FrontFace.StencilFailOp = z_desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    z_desc.FrontFace.StencilDepthFailOp = z_desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
    if (FAILED(ID3D11Device_CreateBlendState(g_hle.dev, &b, &bs)) ||
        FAILED(ID3D11Device_CreateDepthStencilState(g_hle.dev, &z_desc, &dss))) {
        if (bs) ID3D11BlendState_Release(bs);
        return;
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float k[8] = { c[0], c[1], c[2], c[3], z, 0, 0, 0 };
        memcpy(m.pData, k, sizeof k);
        ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)cb, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)size_of->tw; vp.Height = (float)size_of->th;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(g_hle.ctx, rt ? 1 : 0, rt ? &rt->rtv : NULL, ds ? ds->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(g_hle.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(g_hle.ctx, 1, r);
    ID3D11DeviceContext_RSSetState(g_hle.ctx, rs);
    ID3D11DeviceContext_OMSetBlendState(g_hle.ctx, bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_hle.ctx, dss, stencil & 0xFF);
    ID3D11DeviceContext_IASetInputLayout(g_hle.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_hle.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_hle.ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_hle.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_hle.ctx, 0, 1, &cb);
    ID3D11DeviceContext_Draw(g_hle.ctx, 3, 0);
    ID3D11BlendState_Release(bs);
    ID3D11DepthStencilState_Release(dss);
}

/* ---------------------------------------------------------------- init */

int hle_draw_init(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt;
    ID3DBlob *ps;

    /* Profiling must not add two clock queries to every production draw. */
    const char *measure = getenv("RECOMP_HLE_PROFILE");
    s_measure_draws = measure && *measure && *measure != '0';
    disk_load();
    d.vb_size = 32u << 20;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = d.vb_size;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.vb)))
        return 0;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx.NumElements = d.vb_size / 4;
    sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)d.vb, &sv, &d.vb_srv)))
        return 0;
    memset(&opt, 0, sizeof opt);
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(g_hle.dev, D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt)))
        d.vb_no_overwrite = opt.MapNoOverwriteOnDynamicBufferSRV;

    d.ib_size = 8u << 20;
    bd.ByteWidth = d.ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.ib)))
        return 0;
    if (hle_gpu_api() == HLE_API_VULKAN && d.vb_no_overwrite) {
        D3D11_QUERY_DESC qd;
        int k, ok = 1;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        d.vb_ring[0] = d.vb; d.vb_ring_srv[0] = d.vb_srv; d.ib_ring[0] = d.ib;
        for (k = 0; k < RINGS && ok; k++) {
            if (k) {
                bd.ByteWidth = d.vb_size;
                bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                ok = SUCCEEDED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.vb_ring[k])) &&
                     SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_hle.dev, (ID3D11Resource *)d.vb_ring[k], &sv,
                                                                      &d.vb_ring_srv[k]));
                bd.ByteWidth = d.ib_size;
                bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
                bd.MiscFlags = 0;
                ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.ib_ring[k]));
            }
            ok = ok && SUCCEEDED(ID3D11Device_CreateQuery(g_hle.dev, &qd, &d.vb_q[k])) &&
                 SUCCEEDED(ID3D11Device_CreateQuery(g_hle.dev, &qd, &d.ib_q[k]));
        }
        d.rings = ok;
        bd.ByteWidth = d.ib_size;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.MiscFlags = 0;
        fprintf(stderr, "[HLE-D3D11] vertex/index rings: %s\n", ok ? "3 buffers each (Vulkan)" : "discard");
    }

    bd.ByteWidth = sizeof(DrawCB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.cb_draw)))
        return 0;
    bd.ByteWidth = (sizeof(PsCB) + 15) & ~15u;
    if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.cb_ps)))
        return 0;
    bd.ByteWidth = sizeof d.vsc;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &bd, NULL, &d.cb_vsc)))
        return 0;
    d.vsc_dirty = 1;

    ps = compile(k_ps_source, sizeof k_ps_source - 1, "ps_5_0", "hle_ps");
    if (!ps)
        return 0;
    if (FAILED(ID3D11Device_CreatePixelShader(g_hle.dev, ID3D10Blob_GetBufferPointer(ps),
                                              ID3D10Blob_GetBufferSize(ps), NULL, &d.ps_main))) {
        ID3D10Blob_Release(ps);
        return 0;
    }
    ID3D10Blob_Release(ps);
    fprintf(stderr, "[HLE-D3D11] draw path ready (vertex ring %u MB, no-overwrite %s)\n",
            d.vb_size >> 20, d.vb_no_overwrite ? "yes" : "no");
    return 1;
}

void hle_draw_stats(uint64_t *draws, uint64_t *skipped, uint64_t *tris)
{
    *draws = d.draws;
    *skipped = d.skipped;
    *tris = d.tris;
}
