#include "black_gpu.h"
#include "hle_d3d11_internal.h"
#include "../recomp/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This GPU library owns no CPU interpreter and links no NV2A/APU library.
 * Its memory view is explicitly attached by the future native CPU host. */
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_esp;
volatile int xbox_nv2a_draw_disabled = 1;
static BlackGpuKeyCallback key_callback;
static int attached;
uint32_t hle_ps_constants_written;
typedef struct Binding { uint32_t va; int id; unsigned count; unsigned char source[6]; const char* name; } Binding;
static const Binding bindings[] = {
#include "black_gpu_bindings.inc"
};

void xbox_FramebufferKeyEvent(int key, int down) { if (key_callback) key_callback(key, down); }
void hle_port_native_missing(const char* boundary, unsigned caller) {
    fprintf(stderr,"Black native GPU boundary unavailable: %s (caller %08X)\n",boundary,caller);
    abort();
}
int hle_trace_on(void) { return 0; }
int hle_trace_black_wanted(void) { return 0; }
void hle_trace_arm_next(void) {}

static const Binding* find(uint32_t va) {
    for (unsigned i=0;i<sizeof(bindings)/sizeof(bindings[0]);++i)
        if(bindings[i].va==va) return &bindings[i];
    return NULL;
}
int BlackGpuAttachMemory(ptrdiff_t offset, BlackGpuKeyCallback callback) {
    if (attached || g_hle.state) return 0;
    g_xbox_mem_offset=offset; key_callback=callback;
    /* A primary host renderer, never shadow comparison against an emulator. */
    _putenv_s("RECOMP_HLE_PRIMARY","1");
    _putenv_s("RECOMP_HLE_LIBDRAW","0");
    _putenv_s("RECOMP_HLE_LIBSTATE","0");
    attached=1;
    return 1;
}
const char* BlackGpuApiName(uint32_t va) { const Binding* b=find(va);return b?b->name:NULL; }
int BlackGpuApiCount(void) { return sizeof(bindings)/sizeof(bindings[0]); }
int BlackGpuReady(void) { return attached && g_hle.dev!=NULL; }
int BlackGpuRoute(uint32_t va, int phase, int depth, const BlackGpuCall* call, uint32_t result) {
    const Binding* b=find(va); uint32_t args[6]={0};
    if(!b) return 0;
    if(!attached || !call || (phase!=0 && phase!=1) || depth<0) return -1;
    g_esp=call->gpr[4];
    for(unsigned i=0;i<b->count;++i) {
        unsigned selector=b->source[i];
        if(selector>=14) return -1;
        args[i]=selector<8?call->gpr[selector]:call->stack[selector-8];
    }
    if(b->id==HLE_Direct3D_CreateDevice && phase==1 && result==0)
        hle_native_device_created(args[4]);
    if(phase==0 && b->id==HLE_D3DDevice_SetPixelShader) hle_ps_constants_written=0;
    if(phase==0 && b->id==HLE_D3DDevice_SetPixelShaderConstant)
        for(unsigned i=0;i<args[2] && args[0]+i<18;++i) hle_ps_constants_written|=1u<<(args[0]+i);
    if(phase==0 && depth==0) {
        if(b->id==HLE_D3DDevice_Clear) {
            float z;memcpy(&z,&args[4],4);
            hle_native_clear(args[0],args[1],args[2],args[3],z,args[5]);
        } else if(b->id==HLE_D3DDevice_Swap) hle_native_swap(args[0]);
        else if(b->id==HLE_D3DDevice_CopyRects)
            hle_native_copy_rects(args[0],args[1],args[2],args[3],args[4]);
    }
    hle_native_observe(b->id,depth,phase,args,call->gpr[1],call->gpr[2],result);
    return 1;
}
