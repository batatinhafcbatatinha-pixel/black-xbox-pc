"""Map FIFA Street 1 D3D8 (XDK 5849, non-LTCG) addresses to Black's (XDK 5849, LTCG) by aligning matched functions.

Both titles link the same D3D8 source, so a library function named by XbSymbolDatabase in both references the same
globals, tables and callees in nearly the same order. For every function pair, the ordered absolute references of
the two translated bodies are aligned (difflib on reference kinds) and each aligned pair votes. Output:
analysis/street1-d3d-map.json with every Street 1 address that got votes, its best Black candidate and the support.
Static evidence only; consumers must still byte-check what they rely on.
"""
import collections
import difflib
import glob
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
S1 = ROOT.parent / 'street1'
SYM = re.compile(r'(\w+?)__(FUN|VAR)__(?:(\w+?)__)?(\w+)(?:\(.*\))? = 0x([0-9a-f]+)')
FN = re.compile(r'^void (sub_[0-9A-F]{8})(?:_gpu_original|_gen)?\(void\)\s*\{(.*?)\n\}', re.M | re.S)
REF = re.compile(r'(?:MEM(?:8|16|32)\(|RECOMP_ABI_CALL\(|= |, |\+ |\(|PUSH32\(esp, )0x([0-9A-F]{5,8})u?\b'
                 r'|sub_([0-9A-F]{8})\(\); return; /\* tail')


def symbols(path):
    out = {}
    for line in path.read_text(errors='replace').splitlines():
        m = SYM.match(line.strip())
        if m and m.group(1).startswith(('D3D', 'XGRAPH')):
            out.setdefault(m.group(4), (m.group(2), int(m.group(5), 16)))
    return out


def bodies(gen_dir):
    out = {}
    for f in glob.glob(str(gen_dir / 'recomp_*.c')):
        for m in FN.finditer(Path(f).read_text(encoding='utf-8', errors='replace')):
            out[int(m.group(1)[4:], 16)] = m.group(2)
    return out


def refs(body, lo, hi):
    seq = []
    for m in REF.finditer(body):
        v = int(m.group(1) or m.group(2), 16)
        if lo <= v < hi:
            seq.append(v)
    return seq


def main():
    s1sym, bsym = symbols(S1 / 'hle/xbsdb_symbols.txt'), symbols(ROOT / 'analysis/xbsdb-symbols.txt')
    for s in json.loads((ROOT / 'analysis/native-gpu-bindings.json').read_text())['apiSymbols']:
        bsym.setdefault(s['name'], ('FUN', int(s['address'], 16)))
    s1b, bb = bodies(S1 / 'project/src/recomp/gen'), bodies(ROOT / 'project/src/recomp/gen')
    votes = collections.defaultdict(collections.Counter)
    pairs = 0
    # Seed: named symbols are their own evidence.
    for name, (kind, a) in s1sym.items():
        if name in bsym:
            votes[a][bsym[name][1]] += 100
    for name, (kind, a) in s1sym.items():
        if kind != 'FUN' or name not in bsym or a not in s1b or bsym[name][1] not in bb:
            continue
        x = refs(s1b[a], 0x10000, 0x800000)
        y = refs(bb[bsym[name][1]], 0x10000, 0x800000)
        if not x or not y:
            continue
        pairs += 1
        # Align on coarse kinds (code vs data region) so insertions in one body do not shift everything.
        kx = ['c' if 0x250020 <= v < 0x260178 else 'd' for v in x]
        ky = ['c' if 0x266880 <= v < 0x282FE0 or v < 0x217FC0 else 'd' for v in y]
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, kx, ky, autojunk=False).get_opcodes():
            if op == 'equal' or (op == 'replace' and i2 - i1 == j2 - j1):
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    votes[x[i]][y[j]] += 1
    result = []
    for a, c in sorted(votes.items()):
        best, n = c.most_common(1)[0]
        total = sum(c.values())
        result.append(dict(street1=f'0x{a:08X}', black=f'0x{best:08X}', support=n, total=total,
                           others=[f'0x{k:08X}:{v}' for k, v in c.most_common(4)[1:]]))
    (ROOT / 'analysis/street1-d3d-map.json').write_text(json.dumps(dict(
        functionPairs=pairs, entries=result,
        limitations=['Alignment votes; verify each address the native layer depends on.']), indent=1) + '\n')
    print(f'{pairs} aligned function pairs, {len(result)} Street 1 addresses mapped')


if __name__ == '__main__':
    main()
