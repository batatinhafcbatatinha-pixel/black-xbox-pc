"""Generate and compile Black AOT code without launching game instructions."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'project'
TOOLKIT = ROOT.parent / 'xboxrecomp'
EXPECTED = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'
BUILD = Path(os.environ.get('BLACK_BUILD_ROOT', str(PROJECT / 'out'))).resolve()


def guard():
    size = sum(p.stat().st_size for p in PROJECT.rglob('*') if p.is_file())
    if not BUILD.is_relative_to(PROJECT):
        size += sum(p.stat().st_size for p in BUILD.rglob('*') if p.is_file())
    if size >= 2 * 1024 ** 3:
        raise RuntimeError('2 GiB project budget reached')


def run(command, cwd, log):
    guard()
    print(subprocess.list2cmdline(list(map(str, command))), flush=True)
    with (ROOT / 'analysis' / log).open('w', encoding='utf-8') as stream:
        proc = subprocess.Popen(list(map(str, command)), cwd=cwd, stdout=stream,
                                stderr=subprocess.STDOUT,
                                env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        try:
            while proc.poll() is None:
                guard()
                time.sleep(2)
        except BaseException:
            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
            proc.wait()
            raise
        if proc.returncode:
            raise RuntimeError(f'Exit {proc.returncode}; see analysis/{log}')
    guard()


def strict_missing_code():
    """Make the generator's declared gaps explicit failures, preserving their addresses."""
    gen = PROJECT / 'src/recomp/gen'
    unresolved = gen / 'recomp_stubs_unresolved.c'
    trapped = []
    if unresolved.exists():
        text = unresolved.read_text(encoding='utf-8')
        for line in text.splitlines():
            match = re.match(r'void (\w+)\(void\).*(?:direct call|missing direct target) 0x([0-9A-Fa-f]+)', line)
            if match:
                trapped.append((match[1], int(match[2], 16)))
        strict = ['#include "recomp_funcs.h"', '#include <stdlib.h>', '#include <stdio.h>']
        for name, address in trapped:
            strict.append(f'void {name}(void) {{ fprintf(stderr, "Black: missing direct target 0x{address:08X}\\n"); abort(); }}')
        new_text = '\n'.join(strict) + '\n'
        if new_text != text:
            unresolved.write_text(new_text, encoding='utf-8')
    failed = []
    for path in gen.glob('recomp_*.c'):
        if path == unresolved:
            continue
        text = path.read_text(encoding='utf-8')
        names = re.findall(r'void (\w+)\(void\) \{ /\* translation failed \*/ \}', text)
        for name in names:
            text = text.replace(f'void {name}(void) {{ /* translation failed */ }}',
                                f'void {name}(void) {{ recomp_unimpl("translation failed: {name}", 0); }}')
        if names:
            path.write_text(text, encoding='utf-8')
            failed.extend(names)
    (ROOT / 'analysis/aot/strict-policy.json').write_text(json.dumps(dict(
        missingDirectTargets=[dict(name=n, address=f'0x{a:08X}') for n, a in trapped],
        failedTranslations=failed, policy='abort; no success/no-op substitution',
        gameExecuted=False), indent=2) + '\n', encoding='utf-8')



def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('stage', choices=['generate', 'configure', 'build'], default='generate', nargs='?')
    args = ap.parse_args()
    guard()
    BUILD.mkdir(parents=True, exist_ok=True)
    temporary = BUILD / 'compiler-temp'
    temporary.mkdir(exist_ok=True)
    os.environ['TMP'] = os.environ['TEMP'] = str(temporary)
    if hashlib.sha256((ROOT / 'default.xbe').read_bytes()).hexdigest() != EXPECTED:
        raise RuntimeError('Wrong Black executable identity')
    if args.stage == 'generate':
        run([sys.executable, '-u', '-m', 'tools.recomp', ROOT / 'default.xbe', '--all',
             '--game-name', 'Black USA', '--disasm-dir', ROOT / 'analysis/disasm',
             '--func-id-dir', ROOT / 'analysis/func-id', '--split', '128',
             '--gen-dir', PROJECT / 'src/recomp/gen', '-o', ROOT / 'analysis/aot'],
            TOOLKIT, 'aot-generation.log')
        strict_missing_code()
        subprocess.run([sys.executable, str(ROOT / 'scripts/recover_direct_leaves.py')], check=True)
        subprocess.run([sys.executable, str(ROOT / 'scripts/recover_boot_functions.py')], check=True)
        subprocess.run([sys.executable, str(ROOT / 'scripts/recover_missing_functions.py')], check=True)
        subprocess.run([sys.executable, str(ROOT / 'scripts/lower_native_instructions.py')], check=True)
        return
    if not (PROJECT / 'src/recomp/gen/recomp_funcs.h').is_file():
        raise RuntimeError('Generate AOT code first: generate-native.cmd')
    subprocess.run([sys.executable, str(ROOT / 'scripts/recover_direct_leaves.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/recover_boot_functions.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/recover_missing_functions.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/lower_native_instructions.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/wrap_native_gpu.py')], check=True)
    run(['cmake', '-S', PROJECT, '-B', BUILD, '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang-cl'], ROOT, 'native-configure.log')
    if args.stage == 'build':
        run(['cmake', '--build', BUILD, '--parallel', '2'], ROOT, 'native-build.log')
        (ROOT / 'build-location.txt').write_text(str(BUILD) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
