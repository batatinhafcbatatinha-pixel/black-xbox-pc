"""Lower verified MXCSR/WBINVD instructions to host SSE intrinsics, preserving source evidence."""
import hashlib
import json
from pathlib import Path
import re
import capstone

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = 'df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6'


def main():
    binary = (ROOT / 'default.xbe').read_bytes()
    if hashlib.sha256(binary).hexdigest() != EXPECTED:
        raise RuntimeError('Black executable identity mismatch')
    sections = json.loads((ROOT / 'analysis/xbe-analysis.json').read_text())['sections']
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def verify(address, mnemonic, operand):
        for section in sections:
            start, size = int(section['virtual_addr'], 16), section['raw_size']
            if start <= address < start + size:
                offset = int(section['raw_addr'], 16) + address - start
                instructions = list(decoder.disasm(binary[offset:offset + 15], address, count=1))
                if not instructions:
                    break
                ins = instructions[0]
                if ins.mnemonic != mnemonic or ins.op_str != operand:
                    raise RuntimeError(f'Instruction evidence mismatch at {address:08X}')
                return ins.bytes.hex()
        raise RuntimeError(f'Instruction not backed by XBE file at {address:08X}')

    evidence = {}
    pattern = re.compile(r'RECOMP_UNIMPL\("(stmxcsr|ldmxcsr) (dword ptr \[esp(?: \+ 0x[0-9a-f]+| \+ [0-9]+)?\])", '
                         r'0x([0-9A-Fa-f]+)u\); /\* TODO: [^*]+\*/')
    wbinvd = re.compile(r'RECOMP_UNIMPL\("wbinvd", 0x([0-9A-Fa-f]+)u\); /\* TODO: wbinvd \*/')
    for path in (ROOT / 'project/src/recomp/gen').glob('recomp_*.c'):
        text = path.read_text(encoding='utf-8')

        def lower(match):
            op, operand, raw_address = match.groups()
            address = int(raw_address, 16)
            raw = verify(address, op, operand)
            expression = operand[len('dword ptr ['):-1]
            native = f'MEM32({expression}) = _mm_getcsr();' if op == 'stmxcsr' else f'_mm_setcsr(MEM32({expression}));'
            evidence[address] = dict(address=f'0x{address:08X}', mnemonic=op, operand=operand, bytes=raw)
            return native + f' /* BLACK_NATIVE_SSE 0x{address:08X} {op} */'

        def lower_wbinvd(match):
            address = int(match.group(1), 16)
            raw = verify(address, 'wbinvd', '')
            evidence[address] = dict(address=f'0x{address:08X}', mnemonic='wbinvd', operand='', bytes=raw)
            # Cache write-back/invalidate has no observable effect on the host's coherent memory.
            return f'/* BLACK_NATIVE wbinvd 0x{address:08X}: no host effect */'

        updated = pattern.sub(lower, text)
        updated = wbinvd.sub(lower_wbinvd, updated)
        if updated != text:
            path.write_text(updated, encoding='utf-8')
    report = ROOT / 'analysis/aot/native-instructions.json'
    if evidence:
        report.write_text(json.dumps(dict(sourceSha256=EXPECTED, nativeLowering=list(evidence.values()),
            semantics='STMXCSR/LDMXCSR use host thread SSE control/status register through native intrinsics; WBINVD has no host effect.',
            limitations=['Other unsupported instructions retain explicit failure.', 'No game was executed.']), indent=2) + '\n')
    print(f'Native SSE lowering: {len(evidence)} verified unique instruction sites updated')


if __name__ == '__main__':
    main()
