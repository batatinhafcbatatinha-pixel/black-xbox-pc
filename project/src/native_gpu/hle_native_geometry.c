// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
/* Screen-space expansion for Xbox points, wide lines and polygon modes. */
#include "hle_d3d11_internal.h"
#include "hle_gpu_api.h"
#include "../recomp/recomp_types.h"
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define GRS(i) MEM32(0x27C008u + 4u * (i))
typedef struct GeometryConstants {
    float viewport[4];
    uint32_t polygon[4];
    float point[4];
    uint32_t flags[4];
} GeometryConstants;
static ID3D11GeometryShader *s_geometry[3];
static ID3D11Buffer *s_constants;
static ID3D11Device *s_device;

static const char s_source[] =
"struct V{float4 pos:SV_Position;float4 d0:COLOR0;float4 d1:COLOR1;float4 fog:FOG;"
"float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
"float4 b0:COLOR2;float4 b1:COLOR3;float pointSize:PSIZE;};\n"
"cbuffer Geometry:register(b1){float4 view;uint4 polygon;float4 pointParm;uint4 flags;}\n"
"V mixv(V a,V b,float t){V r;r.pos=lerp(a.pos,b.pos,t);r.d0=lerp(a.d0,b.d0,t);r.d1=lerp(a.d1,b.d1,t);"
"r.fog=lerp(a.fog,b.fog,t);r.t0=lerp(a.t0,b.t0,t);r.t1=lerp(a.t1,b.t1,t);r.t2=lerp(a.t2,b.t2,t);"
"r.t3=lerp(a.t3,b.t3,t);r.b0=lerp(a.b0,b.b0,t);r.b1=lerp(a.b1,b.b1,t);r.pointSize=lerp(a.pointSize,b.pointSize,t);return r;}\n"
"float plane(V v,uint p){return p==0?v.pos.z:v.pos.w-1e-6;}\n"
"bool clipline(inout V a,inout V b){[unroll]for(uint p=0;p<2;p++){float da=plane(a,p),db=plane(b,p);"
"if(da<0&&db<0)return false;if(da<0)a=mixv(a,b,da/(da-db));else if(db<0)b=mixv(a,b,da/(da-db));}return true;}\n"
"void quadpoint(V v,inout TriangleStream<V> stream){if(v.pos.w<=1e-6||v.pos.z<0)return;"
"float size=clamp(v.pointSize>0?v.pointSize:view.w,pointParm.x,pointParm.y)*pointParm.z;"
"float2 halfSize=size/view.xy;V a=v;"
"a.pos.xy=v.pos.xy+float2(-1,1)*halfSize*v.pos.w;if(flags.y!=0)a.t0=float4(0,0,0,1);stream.Append(a);"
"a.pos.xy=v.pos.xy+float2(1,1)*halfSize*v.pos.w;if(flags.y!=0)a.t0=float4(1,0,0,1);stream.Append(a);"
"a.pos.xy=v.pos.xy+float2(-1,-1)*halfSize*v.pos.w;if(flags.y!=0)a.t0=float4(0,1,0,1);stream.Append(a);"
"a.pos.xy=v.pos.xy+float2(1,-1)*halfSize*v.pos.w;if(flags.y!=0)a.t0=float4(1,1,0,1);stream.Append(a);stream.RestartStrip();}\n"
"void quadline(V a,V b,inout TriangleStream<V> stream){if(!clipline(a,b))return;"
"float2 delta=(b.pos.xy/b.pos.w-a.pos.xy/a.pos.w)*view.xy;float len=length(delta);"
"if(len<1e-7){quadpoint(a,stream);return;}float2 offset=float2(-delta.y,delta.x)/len*view.z/view.xy;"
"V v=a;v.pos.xy+=offset*a.pos.w;stream.Append(v);v.pos.xy=a.pos.xy-offset*a.pos.w;stream.Append(v);"
"v=b;v.pos.xy+=offset*b.pos.w;stream.Append(v);v.pos.xy=b.pos.xy-offset*b.pos.w;stream.Append(v);stream.RestartStrip();}\n"
"[maxvertexcount(4)]void points(point V input[1],inout TriangleStream<V> stream){quadpoint(input[0],stream);}\n"
"[maxvertexcount(4)]void lines(line V input[2],inout TriangleStream<V> stream){quadline(input[0],input[1],stream);}\n"
/* Triangles: orientation from the homogeneous determinant (valid with
 * vertices behind the eye; the rasterizer and quadpoint/quadline clip). */
"[maxvertexcount(12)]void triangles(triangle V input[3],inout TriangleStream<V> stream){"
"V v0=input[0],v1=input[1],v2=input[2];"
"float3 p0=v0.pos.xyw,p1=v1.pos.xyw,p2=v2.pos.xyw;float area=dot(p0,cross(p1,p2));bool ccw=area>0;"
"if(polygon.z==(ccw?0x901:0x900))return;bool front=ccw==(polygon.w==0x901);"
"uint mode=front||flags.x==0?polygon.x:polygon.y;"
"if(!front&&flags.x!=0){v0.d0=v0.b0;v0.d1=v0.b1;v1.d0=v1.b0;v1.d1=v1.b1;v2.d0=v2.b0;v2.d1=v2.b1;}"
"if(mode==0x1b00){quadpoint(v0,stream);quadpoint(v1,stream);quadpoint(v2,stream);}"
"else if(mode==0x1b01){quadline(v0,v1,stream);quadline(v1,v2,stream);quadline(v2,v0,stream);}"
"else{stream.Append(v0);stream.Append(v1);stream.Append(v2);stream.RestartStrip();}}\n";

uint32_t hle_geometry_flags(uint32_t primitive, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    (void)primitive;
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) return 1;
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP)
        return MEMF(0x29E944u) != 1.0f ? 1 : 0;
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
        return GRS(139) != 0x1B02u || GRS(141) ? 1 : 0;
    return 0;
}

static int geometry_init(void)
{
    if (s_device != g_hle.dev) {
        for (unsigned i = 0; i < 3; i++) {
            if (s_geometry[i]) ID3D11GeometryShader_Release(s_geometry[i]);
            s_geometry[i] = NULL;
        }
        if (s_constants) ID3D11Buffer_Release(s_constants);
        s_constants = NULL; s_device = g_hle.dev;
    }
    if (!s_constants) {
        D3D11_BUFFER_DESC desc;
        memset(&desc, 0, sizeof desc);
        desc.ByteWidth = sizeof(GeometryConstants); desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_hle.dev, &desc, NULL, &s_constants))) return 0;
    }
    return 1;
}

int hle_geometry_bind(uint32_t primitive, D3D11_PRIMITIVE_TOPOLOGY topology, const D3D11_VIEWPORT *vp)
{
    if (!hle_geometry_flags(primitive, topology)) {
        ID3D11DeviceContext_GSSetShader(g_hle.ctx, NULL, NULL, 0);
        return 0;
    }
    if (!geometry_init()) return -1;
    unsigned kind = topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST ? 0 :
        topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP ? 1 : 2;
    if (!s_geometry[kind]) {
        static const char *entry[3] = {"points", "lines", "triangles"};
        ID3DBlob *code = NULL, *errors = NULL;
        HRESULT hr = hle_d3d_compile(s_source, sizeof(s_source) - 1, "native_geometry", NULL, NULL,
            entry[kind], "gs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "[HLE-GEOMETRY] %s compilation failed: %.*s\n", entry[kind],
                errors ? (int)ID3D10Blob_GetBufferSize(errors) : 0,
                errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
        } else hr = ID3D11Device_CreateGeometryShader(g_hle.dev,
            ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &s_geometry[kind]);
        if (errors) ID3D10Blob_Release(errors);
        if (code) ID3D10Blob_Release(code);
        if (FAILED(hr)) return -1;
    }
    GeometryConstants constants;
    memset(&constants, 0, sizeof constants);
    constants.viewport[0] = vp->Width; constants.viewport[1] = vp->Height;
    constants.viewport[2] = MEMF(0x29E944u) * (g_hle.scale > 0.0f ? g_hle.scale : 1.0f);
    constants.viewport[3] = MEMF(0x29E8A0u);
    constants.polygon[0] = GRS(139); constants.polygon[1] = GRS(140);
    constants.polygon[2] = GRS(147); constants.polygon[3] = GRS(146);
    constants.point[0] = MEMF(0x29E8A4u); constants.point[1] = MEMF(0x29E8BCu);
    constants.point[2] = g_hle.scale > 0.0f ? g_hle.scale : 1.0f;
    constants.flags[0] = GRS(141); constants.flags[1] = MEM32(0x29E8A8u);
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = ID3D11DeviceContext_Map(g_hle.ctx, (ID3D11Resource *)s_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) return -1;
    memcpy(map.pData, &constants, sizeof constants);
    ID3D11DeviceContext_Unmap(g_hle.ctx, (ID3D11Resource *)s_constants, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(g_hle.ctx, 1, 1, &s_constants);
    ID3D11DeviceContext_GSSetShader(g_hle.ctx, s_geometry[kind], NULL, 0);
    return 1;
}
