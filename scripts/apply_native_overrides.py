"""Give every function defined natively in project/src/native_d3d precedence over its translated body.

A native file defines `void sub_XXXXXXXX(void)` (or `sub_XXXXXXXX_gpu_original` for a body behind the native GPU
wrapper); the translated definition of the same name in project/src/recomp/gen is renamed to `<name>_gen`, so the
native one links and can still call the original where it wants to. Idempotent; run after wrap_native_gpu.py.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / 'project/src/native_d3d'
GEN = ROOT / 'project/src/recomp/gen'
DEF = re.compile(r'^void (sub_[0-9A-F]{8}(?:_gpu_original)?)\(void\)\s*(?:/\*.*?\*/\s*)?\{', re.M)


def main():
    names = set()
    for path in sorted(NATIVE.glob('*.c')):
        names.update(DEF.findall(path.read_text(encoding='utf-8')))
    renamed = restored = 0
    for path in sorted(GEN.glob('*.c')):
        text = path.read_text(encoding='utf-8')
        # A body no longer overridden natively gets its own name back.
        new = re.sub(r'^void (sub_[0-9A-F]{8}(?:_gpu_original)?)_gen\(void\)(\s*\{)',
                     lambda m: m[0] if m[1] in names else f'void {m[1]}(void){m[2]}', text, flags=re.M)
        restored += text.count('_gen(void)') - new.count('_gen(void)')
        new2 = re.sub(r'^void (sub_[0-9A-F]{8}(?:_gpu_original)?)\(void\)(\s*\{)',
                      lambda m: f'void {m[1]}_gen(void){m[2]}' if m[1] in names else m[0], new, flags=re.M)
        renamed += new2.count('_gen(void)') - new.count('_gen(void)')
        if new2 != text:
            path.write_text(new2, encoding='utf-8')
    missing = [n for n in sorted(names)
               if not any(re.search(rf'^void {n}_gen\(void\)', p.read_text(encoding='utf-8'), re.M)
                          for p in GEN.glob('*.c'))]
    print(f'Native overrides: {len(names)} functions, {renamed} renamed now, {restored} restored'
          + (f'; no translated body for {missing}' if missing else ''))


if __name__ == '__main__':
    main()
