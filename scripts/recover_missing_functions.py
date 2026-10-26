"""Recover the remaining missing direct targets as whole functions (calls allowed), following new callees.

recover_direct_leaves.py accepts only call-free leaves backed by .text xrefs; the disassembly was --text-only, so
targets in DSOUND/D3D/XPP had no xref even though the generator itself emitted the direct call. Here the generated
call is the evidence. Each target is decoded within its code section up to the next known function, translated by
the same FunctionTranslator, and kept only when every instruction is representable. Callees that the recovered
bodies reference and nobody defines join the worklist; whatever cannot be translated stays an explicit abort.
"""
import bisect
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'xboxrecomp'))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

EXPECTED = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'
GEN = ROOT / 'project/src/recomp/gen'
OUT = GEN / 'recomp_recovered_funcs.c'
STUBS = GEN / 'recomp_stubs_unresolved.c'
STUB_RE = re.compile(r'^void (sub_([0-9A-F]{8}))\(void\) \{ fprintf\(stderr, "Black: missing direct target.*$', re.M)
MAX_BODY = 16384


def defined_names():
    names = set()
    for path in GEN.glob('*.c'):
        if path in (STUBS, OUT):
            continue
        # wrap_native_gpu.py renames GPU-bound bodies to *_gpu_original; the wrapper keeps the plain name.
        names.update(re.findall(r'^void (sub_[0-9A-F]{8})(?:_gpu_original)?\(void\)\s*\{?\s*$',
                                path.read_text(encoding='utf-8'), re.M))
    return names


def main():
    data = (ROOT / 'default.xbe').read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        raise RuntimeError('Wrong source XBE')
    config.configure_from_xbe(str(ROOT / 'default.xbe'))
    funcs = json.loads((ROOT / 'analysis/disasm/functions.json').read_text())
    db = {int(f['start'], 16): dict(f, end=int(f['end'], 16)) for f in funcs}
    translator = FunctionTranslator(data, db, seh_prolog=0, seh_epilog=0)
    starts = sorted(db)

    # Previous runs rewrote the stub file; regenerate from the generator's own list plus our earlier additions.
    stub_text = STUBS.read_text(encoding='utf-8')
    previous = json.loads((ROOT / 'analysis/aot/recovered-functions.json').read_text()) \
        if (ROOT / 'analysis/aot/recovered-functions.json').exists() else {}
    worklist = [int(a, 16) for _, a in STUB_RE.findall(stub_text)]
    worklist += [int(r['address'], 16) for r in previous.get('recovered', [])]
    defined = defined_names()
    seen, accepted, rejected, bodies = set(), [], [], []
    while worklist:
        start = worklist.pop()
        name = f'sub_{start:08X}'
        if start in seen or name in defined:
            continue
        seen.add(start)
        section = next((s for s in config._SECTIONS if s.is_code and s.va <= start < s.va + s.raw_size), None)
        if not section:
            rejected.append(dict(name=name, address=f'0x{start:08X}', reason='Not in an executable section'))
            continue
        following = bisect.bisect_right(starts, start)
        upper = min(start + MAX_BODY, section.va + section.raw_size,
                    starts[following] if following < len(starts) else start + MAX_BODY)
        recovered = translator._recover_cfg(start, upper, set(), set())
        reason = None
        if not recovered or not recovered[0]:
            reason = 'No bounded CFG'
        else:
            instructions, tables, _ = recovered
            if any(i.mnemonic in ('hlt', 'ud2', 'retf') for i in instructions):
                reason = 'Decoded into invalid code'  # int3 after a failed check is stepped over like the main AOT
            else:
                end = max(i.end_address for i in instructions)
                info = dict(start=f'0x{start:08X}', end=end, name=name, size=end - start)
                db[start] = info
                translator._recovered_cfg[start] = dict(end=end, instructions=instructions, jump_tables=tables)
                try:
                    body = translator.translate_function(start, info)
                except Exception as error:  # translator rejects shapes it cannot express
                    body, reason = None, f'Translator error: {error}'
                # lower_native_instructions.py turns verified WBINVD sites into host no-ops afterwards.
                pending = re.sub(r'RECOMP_UNIMPL\("wbinvd", 0x[0-9A-F]+u\); /\* TODO: wbinvd \*/', '', body or '')
                if body and ('RECOMP_UNIMPL' in pending or 'recomp_unimpl(' in pending or 'translation failed' in pending):
                    reason = 'Translator cannot represent every instruction'
                elif body:
                    accepted.append(dict(name=name, address=f'0x{start:08X}', end=f'0x{end:08X}',
                                         instructions=len(instructions)))
                    bodies.append(body)
                    defined.add(name)
                    for callee in set(re.findall(r'\bsub_([0-9A-F]{8})\b', body)):
                        if f'sub_{callee}' not in defined:
                            worklist.append(int(callee, 16))
                elif not reason:
                    reason = 'Translator produced no body'
        if reason:
            rejected.append(dict(name=name, address=f'0x{start:08X}', reason=reason))

    # Every referenced but undefined name keeps (or gets) an explicit abort stub.
    referenced = set()
    for body in bodies:
        referenced.update(re.findall(r'\b(sub_[0-9A-F]{8})\b', body))
    stubbed = dict(STUB_RE.findall(stub_text))
    keep = {n: a for n, a in stubbed.items() if n not in defined}
    for name in referenced - defined:
        keep.setdefault(name, name[4:])
    stub_lines = ['#include "recomp_funcs.h"', '#include <stdlib.h>', '#include <stdio.h>']
    stub_lines += [f'void {n}(void) {{ fprintf(stderr, "Black: missing direct target 0x{a}\\n"); abort(); }}'
                   for n, a in sorted(keep.items())]
    STUBS.write_text('\n'.join(stub_lines) + '\n', encoding='utf-8')
    prototypes = ''.join(f'void {n}(void);\n' for n in sorted(referenced | {r["name"] for r in accepted}))
    OUT.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include <math.h>\n\n' + prototypes +
                   '\n' + '\n\n'.join(bodies) + '\n', encoding='utf-8')
    (ROOT / 'analysis/aot/recovered-functions.json').write_text(json.dumps(dict(
        sourceSha256=EXPECTED, recovered=accepted, rejected=rejected, remainingAborts=len(keep),
        limitations=['Static recovery; runtime reachability and behaviour unvalidated.']), indent=2) + '\n')
    print(f'{len(accepted)} functions recovered; {len(rejected)} rejected; {len(keep)} aborts remain')


if __name__ == '__main__':
    main()
