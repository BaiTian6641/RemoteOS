#!/usr/bin/env python3
"""Fetch verified EVDI source only. Does not build, install or load modules."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024*1024):
            result.update(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    lock = json.loads((Path(__file__).with_name('dependencies.lock.json')).read_text())['evdi']
    args.destination.mkdir(parents=True, exist_ok=True)
    archive = args.destination/f'evdi-{lock["version"]}.tar.gz'
    if not archive.exists():
        with tempfile.NamedTemporaryFile(dir=args.destination, prefix='.evdi-download-', delete=False) as tmp:
            partial = Path(tmp.name)
        try:
            subprocess.run(['curl', '--fail', '--silent', '--show-error', '--location', '--proto', '=https', '--tlsv1.2',
                            '--output', str(partial), lock['url']], check=True)
            if digest(partial) != lock['sha256']:
                raise ValueError('Downloaded EVDI archive does not match the pinned SHA-256')
            partial.rename(archive)
        finally:
            partial.unlink(missing_ok=True)
    if digest(archive) != lock['sha256']:
        raise ValueError('Existing EVDI archive does not match the pinned SHA-256')
    source = args.destination/lock['source_directory']
    if source.exists():
        raise ValueError(f'Refusing to overwrite an existing source checkout: {source}')
    with tarfile.open(archive, 'r:gz') as package:
        for entry in package.getmembers():
            if entry.name == lock['source_directory'] and entry.isdir():
                continue
            if not entry.name.startswith(lock['source_directory'] + '/'):
                raise ValueError('Unexpected archive root')
        package.extractall(args.destination, filter='data')
    print(f'Verified EVDI {lock["version"]} source: {source}')
    print(f'Pinned source commit: {lock["commit"]}')
    print('Build for the intended host kernel using docs/hosts.md; no module has been installed or loaded.')


if __name__ == '__main__':
    main()
