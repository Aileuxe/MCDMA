#!/usr/bin/env python3
"""Apply and verify the pinned private llama.cpp integration overlay."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def check_files(source, records, key):
    for record in records:
        relative = Path(record['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('manifest path leaves the source tree')
        path = source / relative
        if path.is_symlink() or not path.resolve().is_relative_to(source):
            raise ValueError('manifest target is a symlink or leaves the source tree')
        expected = record[key]
        if expected is None:
            if path.exists(): raise ValueError('unexpected existing file: ' + str(relative))
        elif not path.is_file() or digest(path) != expected:
            raise ValueError('source hash mismatch: ' + str(relative))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('apply', 'verify'))
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    manifest = json.loads((HERE / 'overlay.json').read_text())
    patch = HERE / 'llama-mcdma.patch'
    if digest(patch) != manifest['patch_sha256']:
        raise ValueError('overlay patch hash mismatch')
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != manifest['base_commit']:
        raise ValueError('expected exact pinned llama.cpp HEAD')
    if args.action == 'apply':
        check_files(source, manifest['files'], 'before_sha256')
        subprocess.run(['git', '-C', str(source), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(source), 'apply', str(patch)], check=True)
    check_files(source, manifest['files'], 'after_sha256')
    print(json.dumps({'verified': True, 'base_commit': head, 'patch_sha256': manifest['patch_sha256'],
                      'backend_abi': manifest['backend_abi']}))

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print('Native overlay failed: ' + str(exc), file=sys.stderr)
        sys.exit(1)
