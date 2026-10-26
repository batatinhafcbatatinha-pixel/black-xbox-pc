"""Extract every file listed in analysis/iso-inventory.json from the XDVDFS ISO.

Usage: python street2/scripts/extract_iso.py <iso> <inventory.json> <out dir>
Paths are validated (no absolute paths / '..'); existing files with the right
size are skipped.
"""
import json
import sys
from pathlib import Path, PurePosixPath

SECTOR = 2048


def main():
    iso, inv, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    data = json.loads(inv.read_text())
    base = data.get('gamePartitionOffset', 0)
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    with iso.open('rb') as f:
        for entry in data['files']:
            rel = PurePosixPath(entry['path'])
            if rel.is_absolute() or '..' in rel.parts:
                raise SystemExit(f'unsafe path {rel}')
            dst = out.joinpath(*rel.parts)
            size = entry['bytes']
            if dst.exists() and dst.stat().st_size == size:
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            f.seek(base + entry['sector'] * SECTOR)
            left = size
            with dst.open('wb') as o:
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    if not chunk:
                        raise SystemExit(f'short read {rel}')
                    o.write(chunk)
                    left -= len(chunk)
            total += size
    print(f'extracted {total} bytes, {len(data["files"])} files listed')


if __name__ == '__main__':
    main()
