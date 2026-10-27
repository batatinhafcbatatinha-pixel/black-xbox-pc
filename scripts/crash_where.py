"""Run black_native.exe under cdb until it faults; print the faulting instruction and the guest functions on the stack.

Frames are resolved through project/out/black_native.map (lld /MAP), so translated functions show as sub_XXXXXXXX.
Diagnostic only; writes runs/crash/cdb.log.
"""
import bisect
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CDB = Path(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe')


def symbols():
    out = []
    for line in (ROOT / 'project/out/black_native.map').read_text(errors='replace').splitlines():
        m = re.match(r'\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})', line)
        if m:
            out.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    out.sort()
    return out


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    exe = ROOT / 'project/out/black_native.exe'
    commands = 'r; u @rip L1; kn 40; q'
    run = ROOT / 'runs/crash'
    run.mkdir(parents=True, exist_ok=True)
    try:
        proc = subprocess.run([str(CDB), '-g', '-c', commands, str(exe), str(ROOT / 'game_files')],
                              cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=seconds)
        text = proc.stdout
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        subprocess.run(['taskkill', '/IM', 'black_native.exe', '/F'], capture_output=True)
        print('no fault within the time limit')
    (run / 'cdb.log').write_text(text, encoding='utf-8')
    syms = symbols()
    keys = [a for a, _ in syms]
    for line in text.splitlines():
        if re.match(r'(rax|rbx|rcx|rdx|rsi|rdi|rip)=', line) or 'ds:' in line or 'Access violation' in line \
                or line.startswith('Black') or line.startswith('[BLACK'):
            print(line.strip()[:200])
    for m in re.finditer(r'^[0-9a-f]{2} [0-9a-f`]+ [0-9a-f`]+\s+(\S+)', text, re.M):
        site = m.group(1)
        if site.startswith('black_native+0x'):
            off = int(site.split('+')[1], 16)
            i = bisect.bisect_right(keys, off) - 1
            print(f'  {site:28s} {syms[i][1] if i >= 0 else "?"} +0x{off - syms[i][0]:x}')
        else:
            print('  ' + site)


if __name__ == '__main__':
    main()
