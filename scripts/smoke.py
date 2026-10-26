"""Bounded boot diagnostic of the current Black native diagnostic build; never infers playable status."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=float, default=20)
args = parser.parse_args()
if args.seconds <= 0:
    parser.error('--seconds must be positive')
location = root / 'build-location.txt'
build = Path(location.read_text(encoding='utf-8-sig').strip()) if location.is_file() else root / 'project/out'
binary = build / 'black_native.exe'
elf = root / 'game_files/default.xbe'
for path in (binary, elf):
    if not path.is_file():
        raise SystemExit(f'Required input missing: {path}')
run = root / 'runs/smoke'
run.mkdir(parents=True, exist_ok=True)
if hashlib.sha256(elf.read_bytes()).hexdigest() != 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6':
    raise SystemExit('Wrong Black XBE identity')
command = [str(binary), str(elf.parent)]
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
started = time.monotonic()
with (run / 'runtime.log').open('w', encoding='utf-8') as log:
    proc = subprocess.Popen(command, cwd=root, startupinfo=startup,
        stdout=log, stderr=subprocess.STDOUT,
        env={k: v for k, v in os.environ.items() if k not in ('RECOMP_FMV_HOST', 'RECOMP_FB_WINDOW')})
    reason = 'exited'
    try:
        while proc.poll() is None:
            if time.monotonic() - started >= args.seconds:
                reason = 'time limit'
                break
            if (run / 'runtime.log').stat().st_size >= 32 * 1024**2:
                reason = 'log limit'
                break
            time.sleep(.25)
    finally:
        if proc.poll() is None:
            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
        proc.wait()
report = dict(command=command, binarySha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    exitCode=proc.returncode, stopReason=reason, seconds=round(time.monotonic()-started, 2),
    limitations=['Process/log evidence only; timeout is not successful completion.',
                 'No visual, gameplay, control or save validation.'])
(root / 'analysis/runtime-smoke.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
