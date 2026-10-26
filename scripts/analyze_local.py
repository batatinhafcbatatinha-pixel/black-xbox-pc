"""Private Black analysis using the adjacent XboxRecomp checkout; no ISO extraction."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TOOLKIT = ROOT.parent / 'xboxrecomp'
ANALYSIS = ROOT / 'analysis'
EXPECTED = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'
MIB = 1024 ** 2


def tree_size(path):
    return sum(p.stat().st_size for p in path.rglob('*') if p.is_file())


def guard():
    # Polling is a stop threshold, not an atomic filesystem quota.
    if tree_size(ANALYSIS) > 768 * MIB:
        raise RuntimeError('Stopping: analysis exceeds the 768 MiB budget')


def stage(name, args, source_output=None):
    guard()
    log = ANALYSIS / (name + '.log')
    with ExitStack() as stack:
        output = stack.enter_context(log.open('w', encoding='utf-8'))
        source = stack.enter_context(source_output.open('w', encoding='utf-8')) if source_output else output
        proc = subprocess.Popen([sys.executable, '-u', '-m', *map(str, args)],
                                cwd=TOOLKIT, stdout=source, stderr=output,
                                env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        try:
            while proc.poll() is None:
                guard()
                time.sleep(1)
            if proc.returncode:
                raise RuntimeError(f'{name} failed ({proc.returncode}); see {log}')
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    print(f'{name}: OK ({log.relative_to(ROOT)})', flush=True)


def write_callsites(parsed, digest):
    dis = ANALYSIS / 'disasm'
    funcs = json.loads((dis / 'functions.json').read_text())
    refs = json.loads((dis / 'xrefs.json').read_text())
    by_thunk = {i['thunk_addr']: dict(i, callsites=[]) for i in parsed['kernel_imports']}
    unmatched = []
    for ref in refs:
        if ref['type'] == 'kernel_call':
            target = by_thunk.get(ref['to'])
            if target is None:
                unmatched.append(ref)
            else:
                target['callsites'].append(ref['from'])
    entry = next(f for f in funcs if f['start'] == parsed['entry_point'])
    report = dict(sourceSha256=digest, entry=entry, kernelImports=list(by_thunk.values()),
                  unmatched=unmatched,
                  limitations=['Static disassembly only; indirect calls may be absent.',
                               'Candidate function boundaries have not been validated in runtime.'])
    (ANALYSIS / 'kernel-call-sites.json').write_text(json.dumps(report, indent=2) + '\n')
    if unmatched:
        raise RuntimeError('Unmatched kernel callsites; inspect kernel-call-sites.json')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--disassemble', action='store_true',
                    help='also analyze .text and translate only the entry function')
    args = ap.parse_args()
    ANALYSIS.mkdir(exist_ok=True)
    # Refuse unrelated executables before passing any addresses to toolkit tools.
    xbe = ROOT / 'default.xbe'
    digest = hashlib.sha256(xbe.read_bytes()).hexdigest()
    if digest != EXPECTED:
        raise RuntimeError('Unexpected default.xbe SHA-256; no analysis was run')
    lock = ANALYSIS / '.analyze.lock'
    with lock.open('x'):
        pass
    try:
        stage('xbe-parser', ['tools.xbe_parser', xbe, '--quiet', '--json', ANALYSIS / 'xbe-analysis.json'])
        stage('kernel-coverage', ['tools.kernel_audit.coverage', ANALYSIS / 'xbe-analysis.json', '--list'])
        sys.path.insert(0, str(TOOLKIT))
        from tools.kernel_audit import coverage
        from tools.kernel_audit.gen_ordinal_remap import build_remap
        from inspect_iso import inspect_xbe
        parsed = json.loads((ANALYSIS / 'xbe-analysis.json').read_text())
        independent = inspect_xbe(xbe.read_bytes())
        covered, wrap, data, todo, sizes = coverage.classify(parsed['kernel_imports'])
        remap, identity, unresolved, total = build_remap(str(ANALYSIS / 'xbe-analysis.json'))
        checks = {
            'title': parsed['title_id'] == '0x' + independent['titleId'],
            'entry': parsed['entry_point'] == independent['entryPoint'],
            'thunks': parsed['kernel_thunk_addr'] == independent['kernelThunkAddress'],
            'imageSize': parsed['image_size'] == independent['imageBytes'],
            'sections': len(parsed['sections']) == len(independent['sections']),
            'copiesIdentical': all(hashlib.sha256(p.read_bytes()).hexdigest() == digest for p in
                [ANALYSIS / 'executables/default.xbe', ROOT / 'game_files/default.xbe']),
        }
        report = dict(sourceSha256=digest, checks=checks, allPassed=all(checks.values()),
                      kernelImports=total, staticallyRouted=len(covered),
                      needsWrapper=len(wrap), unroutedData=len(data), noImplementation=len(todo),
                      ordinalIdentity=identity, ordinalRemap=remap, unresolvedOrdinals=unresolved,
                      limitations=['Source-level route availability is not runtime correctness.',
                                   'No boot, graphics, audio, controls or saves validated.',
                                   'Function detection and translation remain heuristic.'])
        (ANALYSIS / 'consistency.json').write_text(json.dumps(report, indent=2) + '\n')
        if not report['allPassed']:
            raise RuntimeError('Parser/copy consistency check failed')
        # Supply every DB path explicitly: never use another title\'s default output.
        if args.disassemble:
            dis = ANALYSIS / 'disasm'
            stage('disasm', ['tools.disasm', xbe, '--analysis-json', ANALYSIS / 'xbe-analysis.json',
                              '--text-only', '-o', dis])
            write_callsites(parsed, digest)
            identified = ANALYSIS / 'func-id'
            stage('func-id', ['tools.func_id', xbe, '--functions', dis / 'functions.json',
                             '--strings', dis / 'strings.json', '--xrefs', dis / 'xrefs.json',
                             '-o', identified])
            empty = ANALYSIS / 'empty-abi.json'
            empty.write_text('[]\n')
            icalls = ANALYSIS / 'empty-icalls.json'
            icalls.write_text('{}\n')
            stage('entry-translation', ['tools.recomp', xbe, '--game-name', 'Black',
                  '--disasm-dir', dis, '--func-id-dir', identified, '--abi', empty,
                  '--icall-sites', icalls, '-o', ANALYSIS / 'entry-output',
                  '--function', independent['entryPoint']], ANALYSIS / 'entry-preview.c')
        guard()
        print(f'Consistency: passed; {total} imports; analysis {tree_size(ANALYSIS) / MIB:.1f} MiB')
    finally:
        lock.unlink()


if __name__ == '__main__':
    main()
