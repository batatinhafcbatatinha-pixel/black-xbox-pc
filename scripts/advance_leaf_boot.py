"""Bounded diagnostic: recover observed closed leaf targets, rebuild, retry.

Run from the configured compiler environment. Stops on any non-leaf gap;
never substitutes a successful return or claims gameplay completion.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempts', type=int, default=6)
    args = parser.parse_args()
    build = Path((ROOT/'build-location.txt').read_text().strip())
    manifest = ROOT/'config/runtime-targets.json'
    for _ in range(args.attempts):
        subprocess.run([sys.executable, str(ROOT/'scripts/smoke.py'), '--seconds', '15'], check=True)
        log = ROOT/'runs/smoke/runtime.log'
        match = re.search(r'untranslated indirect target (0x[0-9A-F]{8})', log.read_text(errors='replace'))
        if not match:
            print('Stopped: no missing indirect target in the current run; inspect smoke evidence.')
            return
        address = match[1]
        records = json.loads(manifest.read_text())
        if any(r['address'] == address for r in records):
            print(f'Stopped: {address} already considered; requires manual analysis.')
            return
        evidence = f'analysis/aot/indirect-{address[2:]}.log'
        shutil.copyfile(log, ROOT/evidence)
        records.append(dict(name='sub_'+address[2:], address=address, evidence=evidence))
        manifest.write_text(json.dumps(records, indent=2)+'\n')
        subprocess.run([sys.executable, str(ROOT/'scripts/recover_direct_leaves.py')], check=True)
        report = json.loads((ROOT/'analysis/aot/recovered-leaves.json').read_text())
        if not any(r['address'] == address for r in report['recovered']):
            print(f'Stopped: {address} is not a proven closed leaf; no replacement installed.')
            return
        with (ROOT/'analysis/native-host-link.log').open('w') as output:
            subprocess.run(['cmake','--build',str(build),'--parallel','2'],
                           stdout=output,stderr=subprocess.STDOUT,check=True)
    # Verify the last rebuilt executable as well.
    subprocess.run([sys.executable, str(ROOT/'scripts/smoke.py'), '--seconds','15'],check=True)

if __name__ == '__main__':
    main()
