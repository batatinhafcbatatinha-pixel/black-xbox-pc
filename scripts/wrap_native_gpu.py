"""Connect this game's generated direct/indirect CPU calls to native GPU entry routing."""
from pathlib import Path
import hashlib
import json
import re

root = Path(__file__).resolve().parents[1]
gen = root / 'project/src/recomp/gen'
bindings = root / 'project/src/native_gpu/black_gpu_bindings.inc'
metadata = json.loads((root / 'analysis/native-gpu-bindings.json').read_text())
addresses = {int(v,16) for v in re.findall(r'^\{(0x[0-9A-F]+)u,', bindings.read_text(), re.MULTILINE)}
symbols = {int(s['address'],16):s for s in metadata['apiSymbols']}
found = set()
for source in gen.glob('*.c'):
    text = source.read_text(encoding='utf-8')
    def rename(match):
        address = int(match[1],16)
        if address not in addresses:return match[0]
        found.add(address)
        return f'void sub_{address:08X}_gpu_original(void) {{'
    output = re.sub(r'^void sub_([0-9A-F]{8})\(void\)\s*\{', rename, text, flags=re.MULTILINE)
    found.update(int(a,16) for a in re.findall(r'^void sub_([0-9A-F]{8})_gpu_original(?:_gen)?\(void\)', text, re.MULTILINE))
    if output != text:source.write_text(output,encoding='utf-8')

lines=['/* Generated wrappers; addresses/ABIs belong to Black XBE only. */',
 '#include "recomp/recomp_types.h"', '#include "native_gpu/black_gpu.h"',
 '#include <string.h>', 'static RECOMP_TLS int black_gpu_call_depth;',
 'static void capture(BlackGpuCall* call, unsigned stack_words) {',
 '  memset(call,0,sizeof(*call));',
 '  call->gpr[0]=g_eax;call->gpr[1]=g_ecx;call->gpr[2]=g_edx;call->gpr[3]=g_ebx;',
 '  call->gpr[4]=g_esp;call->gpr[5]=g_ebp;call->gpr[6]=g_esi;call->gpr[7]=g_edi;',
 '  for(unsigned i=0;i<stack_words;++i)call->stack[i]=MEM32(g_esp+4+4*i);', '}']
for a in sorted(found):
    if a not in addresses:continue
    name=f'sub_{a:08X}';stack=sum(p[0]=='psh' for p in symbols[a]['arguments'])
    if stack>6:raise RuntimeError('Oversized stack ABI')
    lines += [f'extern void {name}_gpu_original(void);', f'void {name}(void) {{',
        f'  BlackGpuCall before; capture(&before,{stack});',
        '  int depth=black_gpu_call_depth++;',
        f'  if(BlackGpuRoute(0x{a:08X}u,0,depth,&before,g_eax)<0)',
        f'    recomp_unimpl("Native GPU host not attached",0x{a:08X}u);',
        f'  {name}_gpu_original();',
        f'  if(BlackGpuRoute(0x{a:08X}u,1,depth,&before,g_eax)<0)',
        f'    recomp_unimpl("Native GPU post-call failed",0x{a:08X}u);',
        '  --black_gpu_call_depth;', '}']
output='\n'.join(lines)+'\n'
source=root/'project/src/native_gpu_calls.c'
if not source.exists() or source.read_text()!=output:source.write_text(output)
report=dict(sourceXbeSha256=metadata['sourceXbeSha256'],wrappedAddresses=[f'0x{a:08X}' for a in sorted(found&addresses)],
    bindingsWithoutGeneratedBodies=[f'0x{a:08X}' for a in sorted(addresses-found)],
    wrapperSha256=hashlib.sha256(output.encode()).hexdigest(),mode='Native observer before/after original CPU library; direct and dispatch calls share wrapper symbols.',
    limitations=['Original CPU library bodies still perform hardware/miniport bookkeeping. Native host/kernel attachment must replace those boundaries before a complete native executable is possible.',
                 'No game executed. Unattached GPU invocation fails explicitly, not silently.'])
(root/'analysis/native-gpu-routing.json').write_text(json.dumps(report,indent=2)+'\n')
print('Native GPU CPU wrappers:',len(found&addresses),'unavailable bodies:',len(addresses-found))
