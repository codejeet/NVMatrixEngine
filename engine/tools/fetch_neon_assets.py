#!/usr/bin/env python3
"""Restore the exact CC0 source files listed in neon-night/assets.json.
Uses pinned download URLs, validates SHA-256, and never overwrites modified files.
Python 3 and curl are required. No API calls or account/API key are needed.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1] / 'assets/neon-night'


def restore(entry):
    path = ROOT / entry['path']
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError(f'Modified asset: {path}; move it aside before restoring')
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.download')
    try:
        subprocess.run(['curl', '-fsSL', '--retry', '3', '-A', 'NVMatrixEngine/1.0',
                        entry['url'], '-o', str(temporary)], check=True)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError(f'SHA-256 mismatch: {entry["path"]}')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    manifest = json.loads((ROOT / 'assets.json').read_text())
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(restore, manifest['files']))
    print(f'Verified {len(manifest["files"])} source files. Assets by Poly Haven (CC0).')


if __name__ == '__main__':
    main()
