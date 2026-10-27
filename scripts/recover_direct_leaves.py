"""Recover bounded leaf continuations from direct edges in the verified Black XBE.

No guessed returns: ambiguous CFGs and unsupported instructions remain aborts.
"""
import bisect
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
from toolkit import TOOLKIT
sys.path.insert(0, str(TOOLKIT))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

def main():
    binary = ROOT / 'default.xbe'
    data = binary.read_bytes()
    expected = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError('Wrong source XBE')
    config.configure_from_xbe(str(binary))
    funcs = json.loads((ROOT / 'analysis/disasm/functions.json').read_text())
    db = {int(f['start'], 16): dict(f, end=int(f['end'], 16)) for f in funcs}
    translator = FunctionTranslator(data, db, seh_prolog=0, seh_epilog=0)
    starts = sorted(db)
    policy = json.loads((ROOT / 'analysis/aot/strict-policy.json').read_text())
    # A strict-policy refresh sees only remaining stubs. Keep proven leaves in
    # the candidate set, but decode and validate them again against the XBE.
    report_path = ROOT / 'analysis/aot/recovered-leaves.json'
    candidates = {r['name']: r for r in policy['missingDirectTargets']}
    runtime_path = ROOT / 'config/runtime-targets.json'
    runtime_targets = json.loads(runtime_path.read_text()) if runtime_path.exists() else []
    for rec in runtime_targets:
        candidates[rec['name']] = dict(name=rec['name'], address=rec['address'])
    if report_path.exists():
        previous = json.loads(report_path.read_text())
        if previous.get('sourceSha256') != expected:
            raise RuntimeError('Recovery report belongs to a different XBE')
        for rec in previous['recovered']:
            candidates.setdefault(rec['name'], dict(name=rec['name'], address=rec['address']))
    refs = json.loads((ROOT / 'analysis/disasm/xrefs.json').read_text())
    calls = {}
    for ref in refs:
        if ref['type'] in ('call', 'jump', 'cond_jump'):
            calls.setdefault(int(ref['to'], 16), []).append(ref['from'])
    for rec in runtime_targets:
        evidence = (ROOT / rec['evidence']).read_text()
        if f"untranslated indirect target {rec['address']}" not in evidence:
            raise RuntimeError(f"Missing runtime target evidence: {rec['address']}")
        calls.setdefault(int(rec['address'], 16), []).append('runtime:' + rec['evidence'])
    accepted, rejected, bodies = [], [], []
    for rec in sorted(candidates.values(), key=lambda r: int(r['address'], 16)):
        start = int(rec['address'], 16)
        reason = None
        section = next((s for s in config._SECTIONS if s.is_code and s.va <= start < s.va+s.raw_size), None)
        if not section or start not in calls:
            rejected.append(dict(**rec, reason='No mapped code plus direct edge evidence'))
            continue
        following = bisect.bisect_right(starts, start)
        # Overlapping discovery fragments are not boundaries of the original body.
        # Decode within the enclosing observed extent; never delete those fragments.
        enclosing = [f['end'] for a, f in db.items() if a <= start < f['end']]
        extent = max(enclosing) if enclosing else (starts[following] if following < len(starts) else start+512)
        upper = min(start + 4096, section.va + section.raw_size, extent)
        recovered = translator._recover_cfg(start, upper, set(), set())
        if not recovered or not recovered[0]:
            reason = 'No bounded CFG'
        else:
            instructions, tables, _ = recovered
            # This pass accepts leaves only, with all jumps local and at least one RET.
            addresses = {i.address for i in instructions}
            if (tables or not any(i.is_ret for i in instructions)
                or any(i.is_call or (i.is_branch and i.jump_target not in addresses)
                       or i.mnemonic in ('retf', 'int3', 'hlt', 'ud2') for i in instructions)
                or not translator._arm_is_whole(start, instructions)):
                reason = 'Not a closed standalone leaf CFG'
            else:
                end = max(i.end_address for i in instructions)
                info = dict(start=rec['address'], end=end, name=rec['name'], size=end-start)
                db[start] = info
                translator._recovered_cfg[start] = dict(end=end, instructions=instructions, jump_tables=tables)
                body = translator.translate_function(start, info)
                if not body or 'RECOMP_UNIMPL' in body or 'recomp_unimpl(' in body:
                    reason = 'Translator cannot represent every instruction'
                else:
                    accepted.append(dict(**rec, end=f'0x{end:08X}', calls=calls[start],
                        instructions=[dict(address=f'0x{i.address:08X}', bytes=i.bytes_hex,
                                           mnemonic=i.mnemonic, operands=i.op_str) for i in instructions]))
                    bodies.append(body)
        if reason:
            rejected.append(dict(**rec, reason=reason))
    gen = ROOT / 'project/src/recomp/gen'
    recovered_path = gen / 'recomp_recovered_leaves.c'
    source = '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include <math.h>\n\n' + '\n\n'.join(bodies)+'\n'
    if not recovered_path.exists() or recovered_path.read_text() != source:
        recovered_path.write_text(source)
    stubs = gen / 'recomp_stubs_unresolved.c'
    text = stubs.read_text()
    for rec in accepted:
        text = re.sub(r'^void '+re.escape(rec['name'])+r'\(void\).*\n', '', text, flags=re.M)
    if stubs.read_text() != text:
        stubs.write_text(text)
    # Only observed indirect entries need an extra lookup; direct leaves are
    # already referenced by the original translation's symbols.
    indirect_names = {r['name'] for r in runtime_targets}
    indirect = [r for r in accepted if r['name'] in indirect_names]
    header = '#pragma once\n'
    for rec in indirect:
        header += f"extern void {rec['name']}(void);\n"
    header += 'static recomp_func_t black_recovered_lookup(uint32_t va) {\n    switch (va) {\n'
    for rec in indirect:
        header += f"    case {rec['address']}u: return {rec['name']};\n"
    header += '    default: return NULL;\n    }\n}\n'
    header_path = gen / 'recovered_indirect.h'
    if not header_path.exists() or header_path.read_text() != header:
        header_path.write_text(header)
    report = dict(sourceSha256=expected, recovered=accepted, rejected=rejected,
        limitations=['Static bounded leaf recovery only; runtime reachability and gameplay unvalidated.',
                     'Other missing targets remain explicit aborts.'])
    (ROOT / 'analysis/aot/recovered-leaves.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(accepted)} recovered leaves; {len(rejected)} targets retained as aborts')

if __name__ == '__main__':
    main()
