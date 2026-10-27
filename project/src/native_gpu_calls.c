/* Generated wrappers; addresses/ABIs belong to Black XBE only. */
#include "recomp/recomp_types.h"
#include "native_gpu/black_gpu.h"
#include <string.h>
static RECOMP_TLS int black_gpu_call_depth;
static void capture(BlackGpuCall* call, unsigned stack_words) {
  memset(call,0,sizeof(*call));
  call->gpr[0]=g_eax;call->gpr[1]=g_ecx;call->gpr[2]=g_edx;call->gpr[3]=g_ebx;
  call->gpr[4]=g_esp;call->gpr[5]=g_ebp;call->gpr[6]=g_esi;call->gpr[7]=g_edi;
  for(unsigned i=0;i<stack_words;++i)call->stack[i]=MEM32(g_esp+4+4*i);
}
extern void sub_00268880_gpu_original(void);
void sub_00268880(void) {
  BlackGpuCall before; capture(&before,6);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00268880u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00268880u);
  sub_00268880_gpu_original();
  if(BlackGpuRoute(0x00268880u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00268880u);
  --black_gpu_call_depth;
}
extern void sub_00268DE0_gpu_original(void);
void sub_00268DE0(void) {
  BlackGpuCall before; capture(&before,3);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00268DE0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00268DE0u);
  sub_00268DE0_gpu_original();
  if(BlackGpuRoute(0x00268DE0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00268DE0u);
  --black_gpu_call_depth;
}
extern void sub_00269480_gpu_original(void);
void sub_00269480(void) {
  BlackGpuCall before; capture(&before,4);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269480u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269480u);
  sub_00269480_gpu_original();
  if(BlackGpuRoute(0x00269480u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269480u);
  --black_gpu_call_depth;
}
extern void sub_002695D0_gpu_original(void);
void sub_002695D0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x002695D0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x002695D0u);
  sub_002695D0_gpu_original();
  if(BlackGpuRoute(0x002695D0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x002695D0u);
  --black_gpu_call_depth;
}
extern void sub_00269710_gpu_original(void);
void sub_00269710(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269710u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269710u);
  sub_00269710_gpu_original();
  if(BlackGpuRoute(0x00269710u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269710u);
  --black_gpu_call_depth;
}
extern void sub_00269880_gpu_original(void);
void sub_00269880(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269880u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269880u);
  sub_00269880_gpu_original();
  if(BlackGpuRoute(0x00269880u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269880u);
  --black_gpu_call_depth;
}
extern void sub_002698E0_gpu_original(void);
void sub_002698E0(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x002698E0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x002698E0u);
  sub_002698E0_gpu_original();
  if(BlackGpuRoute(0x002698E0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x002698E0u);
  --black_gpu_call_depth;
}
extern void sub_00269930_gpu_original(void);
void sub_00269930(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269930u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269930u);
  sub_00269930_gpu_original();
  if(BlackGpuRoute(0x00269930u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269930u);
  --black_gpu_call_depth;
}
extern void sub_002699E0_gpu_original(void);
void sub_002699E0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x002699E0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x002699E0u);
  sub_002699E0_gpu_original();
  if(BlackGpuRoute(0x002699E0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x002699E0u);
  --black_gpu_call_depth;
}
extern void sub_00269BC0_gpu_original(void);
void sub_00269BC0(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269BC0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269BC0u);
  sub_00269BC0_gpu_original();
  if(BlackGpuRoute(0x00269BC0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269BC0u);
  --black_gpu_call_depth;
}
extern void sub_00269C90_gpu_original(void);
void sub_00269C90(void) {
  BlackGpuCall before; capture(&before,4);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269C90u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269C90u);
  sub_00269C90_gpu_original();
  if(BlackGpuRoute(0x00269C90u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269C90u);
  --black_gpu_call_depth;
}
extern void sub_00269E40_gpu_original(void);
void sub_00269E40(void) {
  BlackGpuCall before; capture(&before,3);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269E40u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269E40u);
  sub_00269E40_gpu_original();
  if(BlackGpuRoute(0x00269E40u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269E40u);
  --black_gpu_call_depth;
}
extern void sub_00269F10_gpu_original(void);
void sub_00269F10(void) {
  BlackGpuCall before; capture(&before,3);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x00269F10u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x00269F10u);
  sub_00269F10_gpu_original();
  if(BlackGpuRoute(0x00269F10u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x00269F10u);
  --black_gpu_call_depth;
}
extern void sub_0026A1F0_gpu_original(void);
void sub_0026A1F0(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026A1F0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026A1F0u);
  sub_0026A1F0_gpu_original();
  if(BlackGpuRoute(0x0026A1F0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026A1F0u);
  --black_gpu_call_depth;
}
extern void sub_0026A420_gpu_original(void);
void sub_0026A420(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026A420u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026A420u);
  sub_0026A420_gpu_original();
  if(BlackGpuRoute(0x0026A420u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026A420u);
  --black_gpu_call_depth;
}
extern void sub_0026A730_gpu_original(void);
void sub_0026A730(void) {
  BlackGpuCall before; capture(&before,5);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026A730u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026A730u);
  sub_0026A730_gpu_original();
  if(BlackGpuRoute(0x0026A730u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026A730u);
  --black_gpu_call_depth;
}
extern void sub_0026AAA0_gpu_original(void);
void sub_0026AAA0(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026AAA0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026AAA0u);
  sub_0026AAA0_gpu_original();
  if(BlackGpuRoute(0x0026AAA0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026AAA0u);
  --black_gpu_call_depth;
}
extern void sub_0026AC00_gpu_original(void);
void sub_0026AC00(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026AC00u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026AC00u);
  sub_0026AC00_gpu_original();
  if(BlackGpuRoute(0x0026AC00u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026AC00u);
  --black_gpu_call_depth;
}
extern void sub_0026B370_gpu_original(void);
void sub_0026B370(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026B370u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026B370u);
  sub_0026B370_gpu_original();
  if(BlackGpuRoute(0x0026B370u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026B370u);
  --black_gpu_call_depth;
}
extern void sub_0026B530_gpu_original(void);
void sub_0026B530(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026B530u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026B530u);
  sub_0026B530_gpu_original();
  if(BlackGpuRoute(0x0026B530u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026B530u);
  --black_gpu_call_depth;
}
extern void sub_0026BA10_gpu_original(void);
void sub_0026BA10(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026BA10u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026BA10u);
  sub_0026BA10_gpu_original();
  if(BlackGpuRoute(0x0026BA10u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026BA10u);
  --black_gpu_call_depth;
}
extern void sub_0026BA50_gpu_original(void);
void sub_0026BA50(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026BA50u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026BA50u);
  sub_0026BA50_gpu_original();
  if(BlackGpuRoute(0x0026BA50u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026BA50u);
  --black_gpu_call_depth;
}
extern void sub_0026BCE0_gpu_original(void);
void sub_0026BCE0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026BCE0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026BCE0u);
  sub_0026BCE0_gpu_original();
  if(BlackGpuRoute(0x0026BCE0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026BCE0u);
  --black_gpu_call_depth;
}
extern void sub_0026BD00_gpu_original(void);
void sub_0026BD00(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026BD00u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026BD00u);
  sub_0026BD00_gpu_original();
  if(BlackGpuRoute(0x0026BD00u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026BD00u);
  --black_gpu_call_depth;
}
extern void sub_0026BF10_gpu_original(void);
void sub_0026BF10(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026BF10u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026BF10u);
  sub_0026BF10_gpu_original();
  if(BlackGpuRoute(0x0026BF10u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026BF10u);
  --black_gpu_call_depth;
}
extern void sub_0026C1E0_gpu_original(void);
void sub_0026C1E0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C1E0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C1E0u);
  sub_0026C1E0_gpu_original();
  if(BlackGpuRoute(0x0026C1E0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C1E0u);
  --black_gpu_call_depth;
}
extern void sub_0026C230_gpu_original(void);
void sub_0026C230(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C230u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C230u);
  sub_0026C230_gpu_original();
  if(BlackGpuRoute(0x0026C230u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C230u);
  --black_gpu_call_depth;
}
extern void sub_0026C4B0_gpu_original(void);
void sub_0026C4B0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C4B0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C4B0u);
  sub_0026C4B0_gpu_original();
  if(BlackGpuRoute(0x0026C4B0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C4B0u);
  --black_gpu_call_depth;
}
extern void sub_0026C620_gpu_original(void);
void sub_0026C620(void) {
  BlackGpuCall before; capture(&before,6);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C620u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C620u);
  sub_0026C620_gpu_original();
  if(BlackGpuRoute(0x0026C620u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C620u);
  --black_gpu_call_depth;
}
extern void sub_0026C6D0_gpu_original(void);
void sub_0026C6D0(void) {
  BlackGpuCall before; capture(&before,6);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C6D0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C6D0u);
  sub_0026C6D0_gpu_original();
  if(BlackGpuRoute(0x0026C6D0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C6D0u);
  --black_gpu_call_depth;
}
extern void sub_0026C780_gpu_original(void);
void sub_0026C780(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026C780u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026C780u);
  sub_0026C780_gpu_original();
  if(BlackGpuRoute(0x0026C780u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026C780u);
  --black_gpu_call_depth;
}
extern void sub_0026D0F0_gpu_original(void);
void sub_0026D0F0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026D0F0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026D0F0u);
  sub_0026D0F0_gpu_original();
  if(BlackGpuRoute(0x0026D0F0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026D0F0u);
  --black_gpu_call_depth;
}
extern void sub_0026D110_gpu_original(void);
void sub_0026D110(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026D110u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026D110u);
  sub_0026D110_gpu_original();
  if(BlackGpuRoute(0x0026D110u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026D110u);
  --black_gpu_call_depth;
}
extern void sub_0026D170_gpu_original(void);
void sub_0026D170(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026D170u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026D170u);
  sub_0026D170_gpu_original();
  if(BlackGpuRoute(0x0026D170u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026D170u);
  --black_gpu_call_depth;
}
extern void sub_0026DBB0_gpu_original(void);
void sub_0026DBB0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DBB0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DBB0u);
  sub_0026DBB0_gpu_original();
  if(BlackGpuRoute(0x0026DBB0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DBB0u);
  --black_gpu_call_depth;
}
extern void sub_0026DBF0_gpu_original(void);
void sub_0026DBF0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DBF0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DBF0u);
  sub_0026DBF0_gpu_original();
  if(BlackGpuRoute(0x0026DBF0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DBF0u);
  --black_gpu_call_depth;
}
extern void sub_0026DC30_gpu_original(void);
void sub_0026DC30(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DC30u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DC30u);
  sub_0026DC30_gpu_original();
  if(BlackGpuRoute(0x0026DC30u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DC30u);
  --black_gpu_call_depth;
}
extern void sub_0026DC90_gpu_original(void);
void sub_0026DC90(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DC90u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DC90u);
  sub_0026DC90_gpu_original();
  if(BlackGpuRoute(0x0026DC90u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DC90u);
  --black_gpu_call_depth;
}
extern void sub_0026DD10_gpu_original(void);
void sub_0026DD10(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DD10u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DD10u);
  sub_0026DD10_gpu_original();
  if(BlackGpuRoute(0x0026DD10u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DD10u);
  --black_gpu_call_depth;
}
extern void sub_0026DD60_gpu_original(void);
void sub_0026DD60(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DD60u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DD60u);
  sub_0026DD60_gpu_original();
  if(BlackGpuRoute(0x0026DD60u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DD60u);
  --black_gpu_call_depth;
}
extern void sub_0026DDB0_gpu_original(void);
void sub_0026DDB0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DDB0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DDB0u);
  sub_0026DDB0_gpu_original();
  if(BlackGpuRoute(0x0026DDB0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DDB0u);
  --black_gpu_call_depth;
}
extern void sub_0026DE50_gpu_original(void);
void sub_0026DE50(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DE50u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DE50u);
  sub_0026DE50_gpu_original();
  if(BlackGpuRoute(0x0026DE50u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DE50u);
  --black_gpu_call_depth;
}
extern void sub_0026DF00_gpu_original(void);
void sub_0026DF00(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DF00u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DF00u);
  sub_0026DF00_gpu_original();
  if(BlackGpuRoute(0x0026DF00u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DF00u);
  --black_gpu_call_depth;
}
extern void sub_0026DF80_gpu_original(void);
void sub_0026DF80(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DF80u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DF80u);
  sub_0026DF80_gpu_original();
  if(BlackGpuRoute(0x0026DF80u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DF80u);
  --black_gpu_call_depth;
}
extern void sub_0026DFF0_gpu_original(void);
void sub_0026DFF0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026DFF0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026DFF0u);
  sub_0026DFF0_gpu_original();
  if(BlackGpuRoute(0x0026DFF0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026DFF0u);
  --black_gpu_call_depth;
}
extern void sub_0026E060_gpu_original(void);
void sub_0026E060(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E060u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E060u);
  sub_0026E060_gpu_original();
  if(BlackGpuRoute(0x0026E060u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E060u);
  --black_gpu_call_depth;
}
extern void sub_0026E0F0_gpu_original(void);
void sub_0026E0F0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E0F0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E0F0u);
  sub_0026E0F0_gpu_original();
  if(BlackGpuRoute(0x0026E0F0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E0F0u);
  --black_gpu_call_depth;
}
extern void sub_0026E130_gpu_original(void);
void sub_0026E130(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E130u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E130u);
  sub_0026E130_gpu_original();
  if(BlackGpuRoute(0x0026E130u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E130u);
  --black_gpu_call_depth;
}
extern void sub_0026E1A0_gpu_original(void);
void sub_0026E1A0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E1A0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E1A0u);
  sub_0026E1A0_gpu_original();
  if(BlackGpuRoute(0x0026E1A0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E1A0u);
  --black_gpu_call_depth;
}
extern void sub_0026E210_gpu_original(void);
void sub_0026E210(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E210u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E210u);
  sub_0026E210_gpu_original();
  if(BlackGpuRoute(0x0026E210u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E210u);
  --black_gpu_call_depth;
}
extern void sub_0026E230_gpu_original(void);
void sub_0026E230(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E230u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E230u);
  sub_0026E230_gpu_original();
  if(BlackGpuRoute(0x0026E230u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E230u);
  --black_gpu_call_depth;
}
extern void sub_0026E250_gpu_original(void);
void sub_0026E250(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E250u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E250u);
  sub_0026E250_gpu_original();
  if(BlackGpuRoute(0x0026E250u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E250u);
  --black_gpu_call_depth;
}
extern void sub_0026E270_gpu_original(void);
void sub_0026E270(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E270u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E270u);
  sub_0026E270_gpu_original();
  if(BlackGpuRoute(0x0026E270u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E270u);
  --black_gpu_call_depth;
}
extern void sub_0026E320_gpu_original(void);
void sub_0026E320(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E320u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E320u);
  sub_0026E320_gpu_original();
  if(BlackGpuRoute(0x0026E320u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E320u);
  --black_gpu_call_depth;
}
extern void sub_0026E350_gpu_original(void);
void sub_0026E350(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E350u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E350u);
  sub_0026E350_gpu_original();
  if(BlackGpuRoute(0x0026E350u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E350u);
  --black_gpu_call_depth;
}
extern void sub_0026E380_gpu_original(void);
void sub_0026E380(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E380u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E380u);
  sub_0026E380_gpu_original();
  if(BlackGpuRoute(0x0026E380u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E380u);
  --black_gpu_call_depth;
}
extern void sub_0026E3C0_gpu_original(void);
void sub_0026E3C0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E3C0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E3C0u);
  sub_0026E3C0_gpu_original();
  if(BlackGpuRoute(0x0026E3C0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E3C0u);
  --black_gpu_call_depth;
}
extern void sub_0026E3E0_gpu_original(void);
void sub_0026E3E0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E3E0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E3E0u);
  sub_0026E3E0_gpu_original();
  if(BlackGpuRoute(0x0026E3E0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E3E0u);
  --black_gpu_call_depth;
}
extern void sub_0026E400_gpu_original(void);
void sub_0026E400(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E400u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E400u);
  sub_0026E400_gpu_original();
  if(BlackGpuRoute(0x0026E400u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E400u);
  --black_gpu_call_depth;
}
extern void sub_0026E460_gpu_original(void);
void sub_0026E460(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E460u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E460u);
  sub_0026E460_gpu_original();
  if(BlackGpuRoute(0x0026E460u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E460u);
  --black_gpu_call_depth;
}
extern void sub_0026E4B0_gpu_original(void);
void sub_0026E4B0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E4B0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E4B0u);
  sub_0026E4B0_gpu_original();
  if(BlackGpuRoute(0x0026E4B0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E4B0u);
  --black_gpu_call_depth;
}
extern void sub_0026E4D0_gpu_original(void);
void sub_0026E4D0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E4D0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E4D0u);
  sub_0026E4D0_gpu_original();
  if(BlackGpuRoute(0x0026E4D0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E4D0u);
  --black_gpu_call_depth;
}
extern void sub_0026E530_gpu_original(void);
void sub_0026E530(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E530u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E530u);
  sub_0026E530_gpu_original();
  if(BlackGpuRoute(0x0026E530u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E530u);
  --black_gpu_call_depth;
}
extern void sub_0026E580_gpu_original(void);
void sub_0026E580(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E580u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E580u);
  sub_0026E580_gpu_original();
  if(BlackGpuRoute(0x0026E580u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E580u);
  --black_gpu_call_depth;
}
extern void sub_0026E5B0_gpu_original(void);
void sub_0026E5B0(void) {
  BlackGpuCall before; capture(&before,0);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E5B0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E5B0u);
  sub_0026E5B0_gpu_original();
  if(BlackGpuRoute(0x0026E5B0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E5B0u);
  --black_gpu_call_depth;
}
extern void sub_0026E640_gpu_original(void);
void sub_0026E640(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E640u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E640u);
  sub_0026E640_gpu_original();
  if(BlackGpuRoute(0x0026E640u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E640u);
  --black_gpu_call_depth;
}
extern void sub_0026E750_gpu_original(void);
void sub_0026E750(void) {
  BlackGpuCall before; capture(&before,3);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E750u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E750u);
  sub_0026E750_gpu_original();
  if(BlackGpuRoute(0x0026E750u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E750u);
  --black_gpu_call_depth;
}
extern void sub_0026E7C0_gpu_original(void);
void sub_0026E7C0(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E7C0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E7C0u);
  sub_0026E7C0_gpu_original();
  if(BlackGpuRoute(0x0026E7C0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E7C0u);
  --black_gpu_call_depth;
}
extern void sub_0026E810_gpu_original(void);
void sub_0026E810(void) {
  BlackGpuCall before; capture(&before,2);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x0026E810u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x0026E810u);
  sub_0026E810_gpu_original();
  if(BlackGpuRoute(0x0026E810u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x0026E810u);
  --black_gpu_call_depth;
}
extern void sub_002773A0_gpu_original(void);
void sub_002773A0(void) {
  BlackGpuCall before; capture(&before,1);
  int depth=black_gpu_call_depth++;
  if(BlackGpuRoute(0x002773A0u,0,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU host not attached",0x002773A0u);
  sub_002773A0_gpu_original();
  if(BlackGpuRoute(0x002773A0u,1,depth,&before,g_eax)<0)
    recomp_unimpl("Native GPU post-call failed",0x002773A0u);
  --black_gpu_call_depth;
}
