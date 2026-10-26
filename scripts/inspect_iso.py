"""Inventory an Xbox XDVDFS ISO and extract only the principal executable."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
BASES = (0, 0xFD90, 0x30600, 0x2080000, 0xFD90000, 0x18300000)

def inspect_xbe(blob):
    """Parse bounded XBE headers, without mapping or executing guest code."""
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(blob):
            raise ValueError('XBE field outside executable')
        return struct.unpack_from(fmt, blob, offset)
    if len(blob) < 0x174 or blob[:4] != b'XBEH':
        raise ValueError('Unexpected XBE header')
    base, headers, image_size = unpack('<III', 0x104)
    if headers < 0x174 or headers > len(blob):
        raise ValueError('Invalid XBE header extent')
    cert, n, table = unpack('<III', 0x118)
    def header_offset(va, size=1):
        off = va - base
        if off < 0 or off + size > headers:
            raise ValueError('XBE header VA outside headers')
        return off
    if n > 4096:
        raise ValueError('Invalid XBE section count')
    section_offset = header_offset(table, n * 56)
    sections = []
    for i in range(n):
        flags, va, virtual_size, raw, raw_size, name_va = unpack('<IIIIII', section_offset + i * 56)
        if raw + raw_size > len(blob) or va + virtual_size > 0x100000000:
            raise ValueError('Invalid XBE section extent')
        name_off = header_offset(name_va)
        name_end = blob.find(b'\0', name_off, min(headers, name_off + 256))
        if name_end < 0:
            raise ValueError('Unterminated XBE section name')
        sections.append(dict(name=blob[name_off:name_end].decode('ascii'), flags=flags,
                             virtualAddress=f'0x{va:08X}', virtualSize=virtual_size,
                             rawOffset=raw, rawSize=raw_size, executable=bool(flags & 4)))
    encoded_entry, tls = unpack('<II', 0x128)
    candidates = [(kind, encoded_entry ^ key) for kind, key in
                  (('retail', 0xA8FC57AB), ('debug', 0x94859D4B))]
    valid = [(kind, entry) for kind, entry in candidates if any(
        s['executable'] and int(s['virtualAddress'], 16) <= entry < int(s['virtualAddress'], 16) + s['virtualSize']
        for s in sections)]
    if len(valid) != 1:
        raise ValueError('Cannot select unique XBE entry encoding')
    kind, entry = valid[0]
    cert_off = header_offset(cert, 0xB0)
    cert_size, timestamp, title_id = unpack('<III', cert_off)
    header_offset(cert, cert_size)
    title = blob[cert_off + 12:cert_off + 92].decode('utf-16-le').split('\0')[0]
    media, region, ratings, disc, version = unpack('<IIIII', cert_off + 0x9C)
    # Actual parser layout has an additional field before kernel thunks;
    # see xboxrecomp/tools/xbe_parser/xbe_parser.py, not the stale prose table.
    library_count, library_va = unpack('<II', 0x160)
    if library_count > 4096:
        raise ValueError('Invalid XBE library count')
    libraries = []
    if library_count:
        library_off = header_offset(library_va, library_count * 16)
        for i in range(library_count):
            name, major, minor, build, flags = unpack('<8sHHHH', library_off + i * 16)
            libraries.append(dict(name=name.split(b'\0')[0].decode('ascii'), major=major,
                                  minor=minor, build=build, flags=flags))
    thunk_raw, = unpack('<I', 0x158)
    thunk = thunk_raw ^ (0x5B6D40B6 if kind == 'retail' else 0xEFB1F152)
    return dict(format='XBEH', architecture='original Xbox x86 little-endian',
                imageBase=f'0x{base:08X}', imageBytes=image_size, headerBytes=headers,
                entryPoint=f'0x{entry:08X}', entryEncoding=kind, tlsAddress=f'0x{tls:08X}',
                kernelThunkAddress=f'0x{thunk:08X}', titleId=f'{title_id:08X}', title=title,
                certificateBytes=cert_size, certificateTimestamp=timestamp,
                gameRegion=f'0x{region:08X}', allowedMedia=f'0x{media:08X}',
                discNumber=disc, version=f'0x{version:08X}', sections=sections, libraries=libraries)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('iso', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    length = args.iso.stat().st_size
    records, seen_dirs = [], set()
    with args.iso.open('rb') as f:
        def read(offset, size):
            if offset < 0 or size < 0 or offset + size > length:
                raise ValueError('Out-of-image file extent')
            f.seek(offset)
            data = f.read(size)
            if len(data) != size:
                raise ValueError('Truncated image')
            return data
        base = next((b for b in BASES if b + 0x10000 + len(MAGIC) <= length and read(b + 0x10000, len(MAGIC)) == MAGIC), None)
        if base is None:
            raise ValueError('No XDVDFS game partition found at known offsets')
        descriptor = read(base + 0x10000, SECTOR)
        def walk(sector, size, prefix='', depth=0):
            if depth > 64 or (sector, size) in seen_dirs or size > 64 * 1024 * 1024:
                raise ValueError('Invalid directory tree')
            seen_dirs.add((sector, size))
            data = read(base + sector * SECTOR, size)
            pending, seen = [0], set()
            while pending:
                offset = pending.pop()
                if offset in seen or offset + 14 > len(data):
                    raise ValueError('Invalid directory node')
                seen.add(offset)
                left, right, start, count, flags, n = struct.unpack_from('<HHIIBB', data, offset)
                if not n or offset + 14 + n > len(data):
                    raise ValueError('Invalid directory name')
                name = data[offset + 14:offset + 14 + n].decode('ascii')
                if name in ('.', '..') or any(c in name for c in '\\/:*?"<>|'):
                    raise ValueError('Unsafe directory name')
                if left:
                    pending.append(left * 4)
                if right:
                    pending.append(right * 4)
                path = prefix + name
                if flags & 0x10:
                    walk(start, count, path + '/', depth + 1)
                else:
                    if base + start * SECTOR + count > length:
                        raise ValueError('Invalid file extent')
                    records.append({'path': path, 'sector': start, 'bytes': count})
        walk(*struct.unpack_from('<II', descriptor, 20))
        executables = []
        for entry in records:
            if entry['path'].lower() not in ('default.xex', 'default.xbe'):
                continue
            blob = read(base + entry['sector'] * SECTOR, entry['bytes'])
            if blob[:4] == b'XBEH':
                info = dict(path=entry['path'], bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
                info.update(inspect_xbe(blob))
                target = output / 'executables' / entry['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() and target.read_bytes() != blob:
                    raise ValueError('Refusing to replace different executable')
                target.write_bytes(blob)
                executables.append(info)
                continue
            if blob[:4] != b'XEX2' or len(blob) < 24:
                raise ValueError('Unexpected XEX header')
            target = output / 'executables' / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and target.read_bytes() != blob:
                raise ValueError('Refusing to replace different executable')
            target.write_bytes(blob)
            flags, pe_offset, reserved, security, n = struct.unpack_from('>IIIII', blob, 4)
            headers = []
            info = dict(path=entry['path'], bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(),
                        format='XEX2', moduleFlags=f'0x{flags:08X}', imageDataOffset=pe_offset,
                        securityInfoOffset=security, optionalHeaderCount=n)
            if n > 10000 or 24 + n * 8 > len(blob):
                raise ValueError('Invalid optional header count')
            for i in range(n):
                key, value = struct.unpack_from('>II', blob, 24 + i * 8)
                headers.append({'key': f'0x{key:08X}', 'value': f'0x{value:08X}'})
                if key == 0x00040006:
                    media, version, base_version, title, platform, exe_type, disc, discs, save = struct.unpack_from('>IIIIBBBBI', blob, value)
                    info.update(titleId=f'{title:08X}', mediaId=f'{media:08X}', version=f'0x{version:08X}',
                                baseVersion=f'0x{base_version:08X}', platform=platform, executableType=exe_type,
                                discNumber=disc, discCount=discs, savegameId=f'{save:08X}')
                elif key == 0x00010100:
                    info['entryPoint'] = f'0x{value:08X}'
                elif key == 0x00010201:
                    info['imageBase'] = f'0x{value:08X}'
                elif key == 0x000003FF:
                    size, encryption, compression = struct.unpack_from('>IHH', blob, value)
                    info['fileFormatInfo'] = {'bytes': size, 'encryptionType': encryption, 'compressionType': compression}
            info['optionalHeaders'] = headers
            executables.append(info)
        report = dict(sourceIso=str(args.iso.resolve()), isoBytes=length, gamePartitionOffset=base,
                      fileCount=len(records), resourceBytes=sum(e['bytes'] for e in records),
                      files=sorted(records, key=lambda e: e['path'].lower()), executables=executables,
                      limitations=['Static inventory only; no game execution.', 'Only principal executables extracted; remaining game assets stay in the ISO.',
                                   'Language filenames are candidates, not verified language selection.'])
        (output / 'iso-inventory.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        summary = {k:v for k,v in report.items() if k not in ('files', 'executables')}
        summary['executables'] = [{k:v for k,v in e.items() if k != 'optionalHeaders'} for e in executables]
        print(json.dumps(summary, indent=2))
        print('Language-related paths:', [e['path'] for e in records if any(s in e['path'].lower() for s in ('locale', 'language', 'portug', 'brazil', 'eng_', 'por_', 'pt_', 'en_'))])

if __name__ == '__main__':
    main()
