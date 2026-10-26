// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/*
 * Xbox vertex shader microcode -> HLSL vertex shader for the native D3D11
 * pipeline of the HLE backend.
 *
 * Instruction semantics follow xboxrecomp's nv2a_vsh_translate.c (compute
 * version, validated against the interpreter); this one is a graphics-stage
 * VS that pulls its inputs itself (SV_VertexID + ByteAddressBuffer), so any
 * Xbox vertex format works without an input layout per declaration.
 *
 * Screen space: the XDK assembler ends every shader with
 *     mul oPos.xyz, r12, c[-38]  +  rcc r1.x, r12.w
 *     mad oPos.xyz, r12, r1.x, c[-37]
 * which turns the clip-space position (r12 is oPos) into NV2A screen space
 * with the viewport constants the library keeps at c[-38]/c[-37]. The native
 * pipeline wants clip space -- its own viewport is what makes resolution and
 * aspect free -- so the position is taken from r12 just before the first
 * instruction that writes oPos from c[-38]/c[-37] (Cxbx-Reloaded removes the
 * same instructions). Shaders without that suffix produce screen-space
 * positions, converted back with the render target size (cbuffer).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hle_vsh.h"

typedef struct { char *p; size_t cap, len; int failed; } Text;

static void emit(Text *t, const char *fmt, ...)
{
    int n;
    va_list ap;
    if (t->failed)
        return;
    va_start(ap, fmt);
    n = vsnprintf(t->p + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= t->cap - t->len) {
        t->failed = 1;
        return;
    }
    t->len += (size_t)n;
}

static uint32_t f(const uint32_t *w, int d, int b, int n)
{
    return (w[d] >> b) & ((1u << n) - 1);
}

/* Constant index read by source `which`, or -1 when it is not a constant. */
static int source_const(const uint32_t *w, int which)
{
    uint32_t mux = which == 0 ? f(w, 2, 26, 2) : which == 1 ? f(w, 2, 11, 2) : f(w, 3, 28, 2);
    return mux == 3 ? (int)f(w, 1, 13, 8) : -1;
}

static int writes_opos_from_viewport(const uint32_t *w)
{
    int i;
    if (!f(w, 3, 12, 4) || !f(w, 3, 11, 1) || f(w, 3, 3, 8) != 0)
        return 0;
    for (i = 0; i < 3; i++) {
        int c = source_const(w, i);
        if (c == 58 || c == 59)
            return 1;
    }
    return 0;
}

static void source(Text *t, const uint32_t *w, int which, const char *name)
{
    uint32_t neg, sw, mux, reg;
    char base[96];
    const char *xyzw = "xyzw";
    if (which == 0) { neg = f(w,1,8,1); sw = f(w,1,0,8); mux = f(w,2,26,2); reg = f(w,2,28,4); }
    else if (which == 1) { neg = f(w,2,25,1); sw = f(w,2,17,8); mux = f(w,2,11,2); reg = f(w,2,13,4); }
    else { neg = f(w,2,10,1); sw = f(w,2,2,8); mux = f(w,3,28,2); reg = (f(w,2,0,2) << 2) | f(w,3,30,2); }
    if (mux == 1) {
        if (reg == 12) snprintf(base, sizeof base, "opos");
        else snprintf(base, sizeof base, "r[%u]", reg < 12 ? reg : 0);
    } else if (mux == 2) {
        snprintf(base, sizeof base, "v[%u]", f(w,1,9,4));
    } else {
        if (f(w,3,1,1)) {
            emit(t, "int ci%s=%u+a0;\n", name, f(w,1,13,8));
            snprintf(base, sizeof base, "c[(ci%s>=0&&ci%s<192)?ci%s:0]", name, name, name);
        } else {
            snprintf(base, sizeof base, "c[%u]", f(w,1,13,8) < 192 ? f(w,1,13,8) : 0);
        }
    }
    emit(t, "float4 %s=%s%s.%c%c%c%c;\n", name, neg ? "-" : "", base,
         xyzw[(sw>>6)&3], xyzw[(sw>>4)&3], xyzw[(sw>>2)&3], xyzw[sw&3]);
}

static void masked(Text *t, const char *dst, const char *src, uint32_t mask)
{
    const char *s = "xyzw";
    for (int i = 0; i < 4; i++)
        if (mask & (8u >> i))
            emit(t, "%s.%c=%s.%c;\n", dst, s[i], src, s[i]);
}

static void temp_write(Text *t, uint32_t reg, const char *value, uint32_t mask)
{
    char d[24];
    if (reg == 12) snprintf(d, sizeof d, "opos");
    else if (reg < 12) snprintf(d, sizeof d, "r[%u]", reg);
    else return;
    masked(t, d, value, mask);
}

/* Vertex fetch: attr[i] = (byte address of vertex 0, stride, format, 0).
 * Format 0x02 (NONE) or 0 = not present: (0,0,0,1). */
static const char *k_prelude =
    "cbuffer Consts:register(b0){float4 c[192];}\n"
    "cbuffer Draw:register(b1){uint4 attr[16];float4 imm[16];float4 ss;uint4 dflags;"
    "row_major float4x4 texgenWorldView;row_major float4x4 texgenNormalView;uint4 texgenMode;"
    "row_major float4x4 textureTransform[4];}\n"
    "ByteAddressBuffer vb:register(t0);\n"
    "struct VSOut{float4 pos:SV_Position;float4 d0:COLOR0;float4 d1:COLOR1;float4 fog:FOG;"
    "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
    "float4 b0:COLOR2;float4 b1:COLOR3;float pointSize:PSIZE;};\n"
    "uint ld32(uint a){uint s=(a&3)*8;uint lo=vb.Load(a&~3u);if(!s)return lo;uint hi=vb.Load((a&~3u)+4);return (lo>>s)|(hi<<(32-s));}\n"
    "uint ld16(uint a){return ld32(a)&0xffff;}\n"
    "uint ld8(uint a){return ld32(a)&0xff;}\n"
    "float sn16(uint v){return max((float)((int)(v<<16)>>16)/32767.0,-1.0);}\n"
    "float sh16(uint v){return (float)((int)(v<<16)>>16);}\n"
    "float4 fetch(uint i,uint vid){uint4 a=attr[i];uint fmt=a.z;uint n=fmt>>4;uint ty=fmt&15;"
    "float4 o=float4(0,0,0,1);if(fmt==0||fmt==2)return imm[i];uint p=a.x+vid*a.y;"
    "if(ty==2){o.x=asfloat(ld32(p));if(n>1)o.y=asfloat(ld32(p+4));if(n>2)o.z=asfloat(ld32(p+8));if(n>3)o.w=asfloat(ld32(p+12));"
    "if(fmt==0x72){o.z=0;o.w=asfloat(ld32(p+8));}}"
    "else if(ty==0){uint u=ld32(p);o=float4((u>>16)&255,(u>>8)&255,u&255,u>>24)/255.0;}"
    "else if(ty==1){o.x=sn16(ld16(p));if(n>1)o.y=sn16(ld16(p+2));if(n>2)o.z=sn16(ld16(p+4));if(n>3)o.w=sn16(ld16(p+6));}"
    "else if(ty==5){o.x=sh16(ld16(p));if(n>1)o.y=sh16(ld16(p+2));if(n>2)o.z=sh16(ld16(p+4));if(n>3)o.w=sh16(ld16(p+6));}"
    "else if(ty==4){o.x=ld8(p)/255.0;if(n>1)o.y=ld8(p+1)/255.0;if(n>2)o.z=ld8(p+2)/255.0;if(n>3)o.w=ld8(p+3)/255.0;}"
    "else if(ty==6){uint u=ld32(p);o.x=max((float)((int)(u<<21)>>21)/1023.0,-1.0);o.y=max((float)((int)(u<<10)>>21)/1023.0,-1.0);"
    "o.z=max((float)((int)u>>22)/511.0,-1.0);}"
    "return o;}\n"
    "float4 rcc(float x){float v=1.0/x;if(abs(v)<5.42101e-20)v=v<0?-5.42101e-20:5.42101e-20;"
    "if(abs(v)>1.884467e19)v=v<0?-1.884467e19:1.884467e19;return v.xxxx;}\n"
    "float4 logv(float x){float ax=abs(x);if(ax==0)return float4(asfloat(0xff800000),1,asfloat(0xff800000),1);"
    "float e=floor(log2(ax));return float4(e,ax/exp2(e),log2(ax),1);}\n"
    "float4 litv(float4 x){float nl=x.x>0?x.x:0;float nh=x.y>0?x.y:0;float p=clamp(x.w,-127.9961,127.9961);"
    "return float4(1,nl,x.x>0?pow(nh,p):0,1);}\n"
    "float4 fixedTexcoord(uint stage,float4 p,float3 normal,float4 source){uint mode=texgenMode[stage];"
    "uint gen=(mode>>16)&15;float4 q=source;float3 n=mul(float4(normal,0),texgenNormalView).xyz;"
    "if(mode&8)n=normalize(n);float3 eye=mul(float4(p.xyz,1),texgenWorldView).xyz;"
    "if(gen==1)q=float4(n,1);else if(gen==2)q=float4(eye,1);"
    "else if(gen==3||gen==4){float3 r=reflect(normalize(eye),normalize(n));"
    "if(gen==3)q=float4(r,1);else{float m=2*sqrt(dot(r.xy,r.xy)+(r.z+1)*(r.z+1));"
    "q=float4(m>0?r.xy/m+0.5:float2(0.5,0.5),0,1);}}"
    "uint count=(mode>>4)&15;if(count){q=mul(q,textureTransform[stage]);"
    "float projected=count==1?q.x:count==2?q.y:count==3?q.z:q.w;"
    "if(count<4)q.w=1;if(count<3)q.z=0;if(count<2)q.y=0;if(mode&4096)q.w=projected;}return q;}\n";

size_t hle_vsh_hlsl(const uint32_t (*program)[4], unsigned count,
                    char *out, size_t cap, int *clip_space, char *error, size_t ecap)
{
    Text t = { out, cap, 0, 0 };
    unsigned end, s;
    int found = 0, suffix = -1;

    if (error && ecap) error[0] = 0;
    if (!out || !cap || !program || !count) {
        if (error && ecap) snprintf(error, ecap, "empty program");
        return 0;
    }
    for (end = 0; end < count; end++) {
        if (f(program[end],1,21,4) > 13) {
            if (error && ecap) snprintf(error, ecap, "unsupported MAC opcode %u at slot %u", f(program[end],1,21,4), end);
            return 0;
        }
        if (suffix < 0 && writes_opos_from_viewport(program[end]))
            suffix = (int)end;
        if (f(program[end],3,0,1)) { found = 1; break; }
    }
    if (!found)
        end = count - 1;            /* microcode from the API has no FINAL bit on its own */
    if (clip_space)
        *clip_space = suffix >= 0;

    emit(&t, "%s", k_prelude);
    emit(&t, "VSOut main(uint vid:SV_VertexID){\n"
             "float4 v[16];[unroll]for(uint k=0;k<16;k++)v[k]=fetch(k,vid);\n"
             "float4 r[12];[unroll]for(uint k=0;k<12;k++)r[k]=0;float4 opos=0,clip=0;"
             "float4 o[13];[unroll]for(uint k=0;k<13;k++)o[k]=0;o[5].x=-1e30;"
             "o[3].w=o[4].w=1;o[9].w=o[10].w=o[11].w=o[12].w=1;int a0=0;\n");
    for (s = 0; s <= end; s++) {
        const uint32_t *w = program[s];
        uint32_t mac = f(w,1,21,4), ilu = f(w,1,25,3), dst = f(w,3,20,4);
        if ((int)s == suffix)
            emit(&t, "clip=opos;\n");
        emit(&t, "{ // slot %u\n", s);
        source(&t, w, 0, "A"); source(&t, w, 1, "B"); source(&t, w, 2, "C");
        emit(&t, "float4 m=0,i=0;\n");
        switch (mac) {
        case 0: break;
        case 1: emit(&t, "m=A;\n"); break;
        case 2: emit(&t, "m=A*B;\n"); break;
        case 3: emit(&t, "m=A+C;\n"); break;
        case 4: emit(&t, "m=A*B+C;\n"); break;
        case 5: emit(&t, "m=(A.x*B.x+A.y*B.y+A.z*B.z).xxxx;\n"); break;
        case 6: emit(&t, "m=(A.x*B.x+A.y*B.y+A.z*B.z+B.w).xxxx;\n"); break;
        case 7: emit(&t, "m=(A.x*B.x+A.y*B.y+A.z*B.z+A.w*B.w).xxxx;\n"); break;
        case 8: emit(&t, "m=float4(1,A.y*B.y,A.z,B.w);\n"); break;
        case 9: case 10: case 11: case 12:
            for (int q = 0; q < 4; q++) {
                char ch = "xyzw"[q];
                if (mac == 9 || mac == 10)
                    emit(&t, "m.%c=A.%c%sB.%c?A.%c:B.%c;\n", ch, ch, mac == 9 ? "<" : ">=", ch, ch, ch);
                else
                    emit(&t, "m.%c=A.%c%sB.%c?1:0;\n", ch, ch, mac == 11 ? "<" : ">=", ch);
            }
            break;
        case 13: emit(&t, "m=A;a0=(int)floor(A.x+0.001);\n"); break;
        }
        switch (ilu) {
        case 0: break;
        case 1: emit(&t, "i=C;\n"); break;
        case 2: emit(&t, "i=(C.x==0?asfloat(0x7f800000):1.0/C.x).xxxx;\n"); break;
        case 3: emit(&t, "i=rcc(C.x);\n"); break;
        case 4: emit(&t, "i=(1.0/sqrt(abs(C.x))).xxxx;\n"); break;
        case 5: emit(&t, "float fl=floor(C.x);i=float4(exp2(fl),C.x-fl,exp2(C.x),1);\n"); break;
        case 6: emit(&t, "i=logv(C.x);\n"); break;
        case 7: emit(&t, "i=litv(C);\n"); break;
        }
        if (mac && mac != 13)
            temp_write(&t, dst, "m", f(w,3,24,4));
        if (ilu)
            temp_write(&t, mac ? 1 : dst, "i", f(w,3,16,4));
        if (f(w,3,12,4) && f(w,3,11,1)) {
            char d[32];
            uint32_t addr = f(w,3,3,8);
            int write = 1;
            if (!addr) snprintf(d, sizeof d, "opos");
            else if (addr < 13) snprintf(d, sizeof d, "o[%u]", addr);
            else write = 0;
            if (write)
                masked(&t, d, f(w,3,2,1) ? "i" : "m", f(w,3,12,4));
        }
        /* Constant writes from the shader (f(w,3,11,1)==0) are not mirrored. */
        emit(&t, "}\n");
    }
    emit(&t, "VSOut R;\n");
    if (suffix >= 0)
        emit(&t, "R.pos=clip;\n");
    else
        /* +0.5: the NV2A (D3D8) puts pixel centres on integers, D3D11 and
         * Vulkan half a pixel further on. Titles draw full-screen quads from
         * -0.5 to 639.5; without the shift the whole 2D layer sat half a game
         * pixel up and left, and the last native column and row were never
         * covered (a black line on the right edge, worst on wide targets). */
        emit(&t, "float w=opos.w>0?opos.w:1;R.pos=float4(((opos.x+0.5)*ss.x-1)*w,((opos.y+0.5)*ss.y+1)*w,opos.z*ss.z*w,w);\n");
    /* Widescreen squeeze (ss.w < 1 on a widened target). 2D vertices on the
     * screen edge stay on the (wider) edge, so full-screen quads and
     * edge-anchored panels still reach it; perspective (w != 1) always
     * squeezes, which is what extends the 3D view (hor+). "On the edge"
     * is a band of 8 game pixels either side of it (ss.x = 2/width), for
     * 2D only (w == 1): the intro movie's quad ends at x = 639.5 and a menu
     * panel at x = 638, and an exact test left a seam at the 4:3 edge.
     * Vertices far outside, or pre-transformed 3D (w != 1) crossing the
     * edge, squeeze with the rest -- left wide, they bent the polygon at
     * the 4:3 edge and the floor showed a line there in matches. */
    emit(&t, "if(ss.w<1){float xn=abs(R.pos.x/R.pos.w);"
             "if(!(xn>1-8*ss.x&&xn<1+8*ss.x&&abs(R.pos.w-1)<1e-3))R.pos.x*=ss.w;}\n");
    emit(&t, "R.d0=saturate(o[3]);R.d1=saturate(o[4]);R.b0=saturate(o[7]);R.b1=saturate(o[8]);R.pointSize=o[6].x;"
             "R.fog=o[5];R.t0=o[9];R.t1=o[10];R.t2=o[11];R.t3=o[12];\n"
             "if(texgenMode.x&0x80000000u)R.t0=fixedTexcoord(0,v[0],v[2].xyz,v[9+(texgenMode.x&3)]);\n"
             "if(texgenMode.y&0x80000000u)R.t1=fixedTexcoord(1,v[0],v[2].xyz,v[9+(texgenMode.y&3)]);\n"
             "if(texgenMode.z&0x80000000u)R.t2=fixedTexcoord(2,v[0],v[2].xyz,v[9+(texgenMode.z&3)]);\n"
             "if(texgenMode.w&0x80000000u)R.t3=fixedTexcoord(3,v[0],v[2].xyz,v[9+(texgenMode.w&3)]);return R;}\n");
    if (t.failed) {
        out[0] = 0;
        if (error && ecap) snprintf(error, ecap, "HLSL output buffer too small");
        return 0;
    }
    return t.len;
}
