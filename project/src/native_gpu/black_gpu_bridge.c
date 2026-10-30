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
/* Trace one frame at a configurable interval; disabled in normal play. */
int hle_trace_on(void) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("BLACK_GPU_TRACE_EVERY");
        every = e ? atoi(e) : 0;
        if (every < 0) every = 0;
    }
    return every > 0 && g_hle.swaps % (unsigned)every == 0;
}
int hle_trace_black_wanted(void) { return 0; }
void hle_trace_arm_next(void) {}

/* Every routed call looks its binding up twice (phase 0 and 1): an open-addressed table instead of a scan. */
#define BIND_SLOTS 256
static const Binding* s_bind_slot[BIND_SLOTS];
static volatile int s_bind_built;
static unsigned bind_hash(uint32_t va) { return (va * 0x9E3779B1u) >> 24; }
static const Binding* find(uint32_t va) {
    unsigned h;
    if(!s_bind_built) {
        for (unsigned i=0;i<sizeof(bindings)/sizeof(bindings[0]);++i) {
            for(h=bind_hash(bindings[i].va);s_bind_slot[h]&&s_bind_slot[h]->va!=bindings[i].va;h=(h+1)&(BIND_SLOTS-1)) {}
            if(!s_bind_slot[h]) s_bind_slot[h]=&bindings[i];   /* first entry wins, as the scan did */
        }
        s_bind_built=1;
    }
    for(h=bind_hash(va);s_bind_slot[h];h=(h+1)&(BIND_SLOTS-1))
        if(s_bind_slot[h]->va==va) return s_bind_slot[h];
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
int BlackGpuAbNewOff(void) { extern int hle_ab_new_off; return hle_ab_new_off; }
/* BLACK_GPU_STATS=<swaps>: per-API call counts (game calls, depth 0) every N swaps. */
static unsigned long long s_api_calls[256];
static void api_stats(const Binding* b, int depth, int phase) {
    static int every=-1; static unsigned long long swaps;
    if(every<0){const char*e=getenv("BLACK_GPU_STATS");every=e?atoi(e):0;}
    if(!every||phase!=0||depth) return;
    s_api_calls[(unsigned)(b-bindings)&255]++;
    if(b->id==HLE_D3DDevice_Swap && ++swaps%(unsigned)every==0){
        fprintf(stderr,"[BLACK-GPU] swap %llu API calls:",swaps);
        for(unsigned i=0;i<sizeof(bindings)/sizeof(bindings[0])&&i<256;++i)
            if(s_api_calls[i]) fprintf(stderr," %s=%llu",bindings[i].name,s_api_calls[i]);
        fputc('\n',stderr); memset(s_api_calls,0,sizeof s_api_calls);
    }
}
int BlackGpuRoute(uint32_t va, int phase, int depth, const BlackGpuCall* call, uint32_t result) {
    const Binding* b=find(va); uint32_t args[6]={0};
    if(!b) return 0;
    api_stats(b,depth,phase);
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
        /* Draws the game pushed since the last call are drawn before anything this call changes. */
        extern void hle_draw_pb_sync(void);
        hle_draw_pb_sync();
        if(b->id==HLE_D3DDevice_Clear) {
            float z;memcpy(&z,&args[4],4);
            hle_native_clear(args[0],args[1],args[2],args[3],z,args[5]);
        } else if(b->id==HLE_D3DDevice_Swap) {
            /* BLACK_MEMSNAP=<prefix> BLACK_MEMSNAP_AT=<swap>,<swap>,...: dump guest .data (diagnostic). */
            static unsigned long long nswap; static const char *at; static int init;
            if(!init){init=1;at=getenv("BLACK_MEMSNAP_AT");}
            ++nswap;
            if(at){
                const char *p=at;
                while(*p){
                    unsigned long long v=strtoull(p,(char**)&p,10);
                    if(v==nswap){
                        char path[512];FILE *f;
                        snprintf(path,sizeof path,"%s-%llu.bin",getenv("BLACK_MEMSNAP"),nswap);
                        if((f=fopen(path,"wb"))){fwrite((const void*)XBOX_PTR(0x2964E0u),1,0x2E65B8u,f);fclose(f);}
                    }
                    if(*p==',')p++; else break;
                }
            }
            hle_native_swap(args[0]);
        }
        else if(b->id==HLE_D3DDevice_CopyRects)
            hle_native_copy_rects(args[0],args[1],args[2],args[3],args[4]);
    }
    hle_native_observe(b->id,depth,phase,args,call->gpr[1],call->gpr[2],result);
    return 1;
}
