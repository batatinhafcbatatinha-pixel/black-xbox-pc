"""Native copies of translated library functions with their hardware accesses removed.

Some of Black's D3D functions mix CPU work with NV2A accesses inline (LTCG folded the miniport's hardware layer
into them), so the hardware part cannot be replaced by overriding a callee. For each function in SPECS the
translated body is copied into project/src/native_d3d/black_native_copies.c with exact, counted edits:

  drop_stores(start, end): between two marker strings, drop every store whose address is built from the GPU
                           register base (no ebx = miniport, esp/ebp = stack frame, or absolute guest global);
  replace(old, new, count): exact text replacement, `count` occurrences required.

apply_native_overrides.py then renames the translated definition to *_gen. Run after recovery/lowering/wrapping.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'project/src/recomp/gen'
OUT = ROOT / 'project/src/native_d3d/black_native_copies.c'

SPECS = [
    dict(address=0x002718F0, name='CDevice::Init', notes=[
        'RAMIN hash entries and graphics-object instances are not written (no instance memory);',
        'the FIFO channel is guest memory: GET is PUT before the wait for the first kick;',
        'no wait for the GPU to drain after the first fence.'],
        edits=[
            ('drop_stores', 'RECOMP_ABI_CALL(0x0027336Du', 'MEM32(ebp + 0x934) = eax;'),
            ('replace', 'loc_0027234E: ;\n',
             'loc_0027234E: ;\n    MEM32(MEM32(ebp + 0x1C20) + 0x44) = MEM32(ebp) & 0x0FFFFFFFu; /* native channel: GET = PUT */\n', 1),
            ('cut', 'loc_00272410: ;\n', 'loc_0027242F: ;\n',
             'loc_00272410: ;\n    goto loc_0027242F; /* native: no GPU to drain */\n\n'),
        ]),
    dict(address=0x00277130, name='pending display-mode change at Swap', notes=[
        'the kernel calls (AvSetDisplayMode, AvSendTVEncoderOption) and the device bookkeeping stay;',
        'the PRAMDAC PLL/general-control register writes do not exist without the NV2A.'],
        edits=[
            ('replace', '    MEM32(ecx + 0x680680) = eax;\n',
             '    /* native: PRAMDAC 0x680680 not written */\n', 1),
            ('replace', '    eax = MEM32(ecx + 0x6806A0);\n',
             '    eax = 0; /* native: PRAMDAC 0x6806A0 not read */\n', 1),
            ('replace', '    MEM32(ecx + 0x6806A0) = eax;\n',
             '    /* native: PRAMDAC 0x6806A0 not written */\n', 1),
        ]),
    dict(address=0x000C4570, name='cutscene skip at the XMV movie step', notes=[
        'every .xmv cutscene (front-end FMV page and in-game movie loop) is stepped here;',
        'A (pad 0) or F10 raises the end-of-stream/end-of-file flags of the open movie, so the game',
        'runs its own teardown and advances (black_skip.c).'],
        edits=[
            ('replace', 'loc_000C4570: ;\n    SET_LO8(eax, MEM8(esi + 0x61));\n',
             'loc_000C4570: ;\n'
             '    /* BLACK native: cutscene skip. A (host pad 0) or F10 ends the open movie through the flags the\n'
             '       decoder itself sets at end of stream/file; nothing else is bypassed. */\n'
             '    if (MEM32(esi + 0x4C) && black_cutscene_skip()) {\n'
             '        MEM8(esi + 0x62) = 1;\n'
             '        MEM8(esi + 0x64) = 1;\n'
             '        esp += 4; return; /* ret: prologue not entered yet, pop the return address */\n'
             '    }\n'
             '    SET_LO8(eax, MEM8(esi + 0x61));\n', 1),
        ]),
]

STORE = re.compile(r'^\s*MEM(?:8|16|32)\((?P<addr>[^;]*?)\) = [^;]*;\s*$')


def body_of(address):
    name = f'sub_{address:08X}'
    pattern = re.compile(r'\nvoid ' + name + r'(?:_gpu_original)?(?:_gen)?\(void\)\s*\{.*?\n\}\n', re.S)
    for path in sorted(GEN.glob('*.c')):
        m = pattern.search(path.read_text(encoding='utf-8'))
        if m:
            return m.group(0)
    raise SystemExit(f'No translated body for {name}')


def keeps(addr):
    return any(t in addr for t in ('ebx', 'esp', 'ebp')) or re.fullmatch(r'\s*0x[0-9A-Fa-f]+u?\s*', addr) is not None


def apply(body, edits):
    for edit in edits:
        kind = edit[0]
        if kind == 'replace':
            _, old, new, count = edit
            if body.count(old) != count:
                raise SystemExit(f'Edit expects {count} x {old!r}, found {body.count(old)}')
            body = body.replace(old, new)
        elif kind == 'cut':
            _, start, end, new = edit
            i, j = body.find(start), body.find(end)
            if i < 0 or j < i or body.count(start) != 1:
                raise SystemExit(f'Cut markers not found once: {start!r} .. {end!r}')
            body = body[:i] + new + body[j:]
        elif kind == 'drop_stores':
            _, start, end = edit
            i, j = body.find(start), body.find(end)
            if i < 0 or j < i:
                raise SystemExit(f'Region markers not found: {start!r} .. {end!r}')
            lines, dropped = [], 0
            for line in body[i:j].split('\n'):
                m = STORE.match(line)
                if m and not keeps(m.group('addr')):
                    lines.append('    /* native: NV2A instance memory not written: ' + line.strip() + ' */')
                    dropped += 1
                else:
                    lines.append(line)
            if not dropped:
                raise SystemExit(f'No hardware stores between {start!r} and {end!r}')
            body = body[:i] + '\n'.join(lines) + body[j:]
    return body


def main():
    parts = ['/* Generated by scripts/make_native_copies.py from the translated library; do not edit. */',
             '#define RECOMP_GENERATED_CODE', '#include "recomp_funcs.h"', '#include <math.h>', '',
             'int black_cutscene_skip(void); /* native_d3d/black_skip.c */', '']
    bodies = []
    for spec in SPECS:
        body = apply(body_of(spec['address']), spec['edits'])
        body = re.sub(r'\nvoid (sub_[0-9A-F]{8})(?:_gpu_original)?(?:_gen)?\(void\)', r'\nvoid \1(void)', body, count=1)
        bodies.append('/* ' + spec['name'] + ': ' + ' '.join(spec['notes']) + ' */' + body)
    referenced = sorted(set(re.findall(r'\b(sub_[0-9A-F]{8})\b', '\n'.join(bodies))))
    parts += [f'void {n}(void);' for n in referenced] + ['']
    text = '\n'.join(parts) + '\n'.join(bodies)
    if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
        OUT.write_text(text, encoding='utf-8')
    print(f'Native copies: {len(bodies)} functions')


if __name__ == '__main__':
    main()
