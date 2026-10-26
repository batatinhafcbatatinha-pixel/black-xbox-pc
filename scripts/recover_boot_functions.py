"""Recover the observed D3D table lookup and its one direct helper from XBE."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent/'xboxrecomp'))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

def main():
    data = (ROOT/'default.xbe').read_bytes()
    expected = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError('Wrong source XBE')
    evidence = (ROOT/'analysis/aot/direct-002744E8.log').read_text(errors='replace')
    if 'missing direct target 0x002744E8' not in evidence:
        raise RuntimeError('Missing observed entry evidence')
    config.configure_from_xbe(str(ROOT/'default.xbe'))
    bounds = {0x2744c9: 0x2744e8, 0x2744e8: 0x274561}
    db = {start: dict(start=f'0x{start:08X}', end=end, name=f'sub_{start:08X}', size=end-start)
          for start,end in bounds.items()}
    translator = FunctionTranslator(data, db, seh_prolog=0, seh_epilog=0)
    bodies, records = [], []
    for start,end in bounds.items():
        instructions,tables,_ = translator._recover_cfg(start,end,set(),set())
        addresses = {i.address for i in instructions}
        if tables or not translator._arm_is_whole(start,instructions):
            raise RuntimeError('Not a closed standalone CFG')
        for i in instructions:
            if i.is_branch and i.jump_target not in addresses:
                raise RuntimeError('Unexpected nonlocal branch')
            if i.is_call:
                if i.call_target is not None and i.call_target not in bounds:
                    raise RuntimeError('Unexpected direct callee')
                # Sole indirect call is a verified XBE kernel thunk slot.
                if i.call_target is None and (i.address != 0x2744dc or i.bytes_hex.lower() != 'ff1570312800'):
                    raise RuntimeError('Unexpected indirect call')
        translator._recovered_cfg[start] = dict(end=end,instructions=instructions,jump_tables=tables)
        body = translator.translate_function(start,db[start])
        if not body or 'RECOMP_UNIMPL' in body:
            raise RuntimeError('Unsupported instruction in recovered body')
        bodies.append(body)
        records.append(dict(start=f'0x{start:08X}',endExclusive=f'0x{end:08X}',
            instructions=[dict(address=hex(i.address),bytes=i.bytes_hex,mnemonic=i.mnemonic,operands=i.op_str) for i in instructions]))
    gen = ROOT/'project/src/recomp/gen'
    source = '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n'
    source += ''.join(f'void sub_{start:08X}(void);\n' for start in bounds)+'\n\n'.join(bodies)+'\n'
    output = gen/'recomp_recovered_boot.c'
    if not output.exists() or output.read_text() != source:
        output.write_text(source)
    stub = gen/'recomp_stubs_unresolved.c'
    before = stub.read_text()
    after = re.sub(r'^void sub_002744E8\(void\).*\n','',before,flags=re.M)
    if before != after:
        stub.write_text(after)
    (ROOT/'analysis/aot/recovered-boot.json').write_text(json.dumps(dict(
        sourceSha256=expected,functions=records,kernelThunkSlot='0x00283170',
        limitations=['Closed CFG reconstruction; GPU initialization not yet validated.']),indent=2)+'\n')
    print('Recovered D3D lookup 0x2744e8 and helper 0x2744c9')

if __name__ == '__main__':
    main()
