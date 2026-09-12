#!/usr/bin/env python3
"""Vendor the same reviewed remote client into both standalone extension repos."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--check', action='store_true')
parser.add_argument('--rc', type=Path, default=Path(__file__).resolve().parents[2] / 'PVT-RC')
parser.add_argument('--rd', type=Path, default=Path(__file__).resolve().parents[2] / 'PVT-RD')
args = parser.parse_args()
source = Path(__file__).resolve().parents[1] / 'remote/client'
files = {p.name: p.read_bytes() for p in sorted(source.iterdir()) if p.is_file()}
manifest = json.dumps({'protocol': 1, 'source': 'procedural_visualizer_tool/remote/client', 'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}, indent=2).encode() + b'\n'
for repo in (args.rc, args.rd):
    target = repo / 'vendor/pvt-remote-client'
    if not args.check: target.mkdir(parents=True, exist_ok=True)
    for name, data in {**files, 'SOURCE.json': manifest}.items():
        if args.check:
            if not (target / name).exists() or (target / name).read_bytes() != data:
                raise SystemExit(f'Shared client drift: {target / name}')
        else: (target / name).write_bytes(data)
print('Both extension clients match the shared protocol source.')
