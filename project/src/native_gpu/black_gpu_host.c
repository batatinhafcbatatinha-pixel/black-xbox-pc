/* Optional host UI is outside the guest D3D API; no fake game API results. */
#include "hle_options.h"
#include "hle_gpu_api.h"
#include <stdio.h>
/* Native CPU hosts use preemptive OS threads, with no global guest-CPU lock.
 * A future serialized host must replace this scheduling boundary explicitly. */
int guest_cpu_suspend(void){return 0;}
void guest_cpu_resume(int token){(void)token;}
void hle_stall_tick(void){} /* diagnostic watchdog is not part of frame semantics */
HleOptions g_opt={0,0,0,1,60,1,0,0,0,0,-1,1,100,1};
int hle_options_key(UINT m,WPARAM k,LPARAM l){(void)m;(void)k;(void)l;return 0;}
int hle_options_mouse(UINT m,int x,int y,int w,int cw,int ch){(void)m;(void)x;(void)y;(void)w;(void)cw;(void)ch;return 0;}
int hle_options_hide_cursor(void){return 0;}
void hle_options_frame(void){}
void hle_options_standalone(void){}
void hle_options_apply_display(uint32_t w,uint32_t h){(void)w;(void)h;}
void hle_options_draw_overlay(ID3D11RenderTargetView* r,UINT w,UINT h){(void)r;(void)w;(void)h;}
void hle_options_draw_button(ID3D11RenderTargetView* r){(void)r;}
const char* hle_options_capture_tag(void){return NULL;}
void hle_options_initial_window(DWORD* style,RECT* r){*style=WS_OVERLAPPEDWINDOW;*r=(RECT){0,0,1280,720};AdjustWindowRect(r,*style,FALSE);}
float hle_options_native_scale(void){return 1.0f;}
int hle_gpu_api(void){return HLE_API_D3D11;}
const char* hle_gpu_api_name(void){return "D3D11";}
void hle_gpu_api_load(void){}
int hle_gpu_vulkan_available(void){return 0;}
int hle_gpu_d3d11_available(void){return 1;}
HRESULT hle_d3d_compile(LPCVOID src,SIZE_T len,LPCSTR name,const D3D_SHADER_MACRO* defs,
 ID3DInclude* inc,LPCSTR entry,LPCSTR target,UINT f1,UINT f2,ID3DBlob** code,ID3DBlob** errors){
 return D3DCompile(src,len,name,defs,inc,entry,target,f1,f2,code,errors);
}
