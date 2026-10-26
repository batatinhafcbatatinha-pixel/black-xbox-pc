"""Verify every extracted XDVDFS file against source ISO bytes before media retirement.

This script never deletes or changes the media or extracted files.
"""
import hashlib
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
inventory = json.loads((root / 'analysis/iso-inventory.json').read_text(encoding='utf-8'))
source = Path(inventory['sourceIso']).resolve()
output = (root / 'game_files').resolve()
before = source.stat()
base = inventory['gamePartitionOffset']
records, directories = [], set()
checks = []
with source.open('rb') as stream:
    def read(offset, size):
        if offset < 0 or offset + size > before.st_size:
            raise ValueError('Extent outside ISO')
        stream.seek(offset)
        result = stream.read(size)
        if len(result) != size:
            raise ValueError('Short ISO read')
        return result

    descriptor = read(base + 0x10000, 2048)
    if descriptor[:20] != b'MICROSOFT*XBOX*MEDIA':
        raise ValueError('Wrong XDVDFS identity')

    def walk(sector, length, prefix='', depth=0):
        if depth > 64 or (sector, length) in directories or length > 64 * 1024**2:
            raise ValueError('Invalid directory structure')
        directories.add((sector, length))
        data = read(base + sector * 2048, length)
        pending, seen = [0], set()
        while pending:
            offset = pending.pop()
            if offset in seen or offset + 14 > len(data):
                raise ValueError('Invalid directory node')
            seen.add(offset)
            left, right, start, size, flags, n = struct.unpack_from('<HHIIBB', data, offset)
            if not n or offset + 14 + n > len(data):
                raise ValueError('Invalid name extent')
            name = data[offset + 14:offset + 14 + n].decode('ascii')
            if name in ('.', '..') or any(c in name for c in '\\/:*?"<>|'):
                raise ValueError('Unsafe directory name')
            if left: pending.append(left * 4)
            if right: pending.append(right * 4)
            path = prefix + name
            if flags & 0x10: walk(start, size, path + '/', depth + 1)
            else: records.append(dict(path=path, sector=start, bytes=size))

    walk(*struct.unpack_from('<II', descriptor, 20))
    expected = sorted(inventory['files'], key=lambda e: e['path'].lower())
    actual = sorted(records, key=lambda e: e['path'].lower())
    if expected != actual:
        raise ValueError('Current disc inventory differs from retained manifest')
    for entry in actual:
        target = (output / entry['path']).resolve()
        if output not in target.parents or not target.is_file() or target.stat().st_size != entry['bytes']:
            raise ValueError('Missing, unsafe or wrong-sized extracted file: ' + entry['path'])
        source_hash, file_hash = hashlib.sha256(), hashlib.sha256()
        stream.seek(base + entry['sector'] * 2048)
        remaining = entry['bytes']
        with target.open('rb') as extracted:
            while remaining:
                length = min(remaining, 4 * 1024**2)
                a, b = stream.read(length), extracted.read(length)
                if len(a) != length or len(b) != length or a != b:
                    raise ValueError('Content mismatch: ' + entry['path'])
                source_hash.update(a); file_hash.update(b); remaining -= length
        checks.append(dict(path=entry['path'], bytes=entry['bytes'], sha256=file_hash.hexdigest(),
                           identicalToIso=source_hash.digest() == file_hash.digest()))
after = source.stat()
if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
    raise ValueError('ISO changed while verifying')
report = dict(sourceIso=str(source), sourceIsoBytes=before.st_size, sourceMtimeNs=before.st_mtime_ns,
              gameFiles=str(output), inventoryRevalidated=True, allFilesIdentical=True,
              verifiedFileCount=len(checks), verifiedResourceBytes=sum(e['bytes'] for e in checks),
              mediaDeleted=False, checks=checks,
              scope='Byte-for-byte verification of every disc file, no game execution.')
(root / 'analysis/game-files-verification.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(f"Verified {len(checks)} files, {report['verifiedResourceBytes']} bytes; ISO has NOT been deleted")
