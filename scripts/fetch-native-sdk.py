#!/usr/bin/env python3
"""Download the official SDK, verifying committed hashes before installation."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

p=argparse.ArgumentParser();p.add_argument('platform',choices=['windows','macos']);p.add_argument('directory',type=Path)
a=p.parse_args();lock=json.loads((Path(__file__).resolve().parents[1]/'packaging/native-sdk.lock.json').read_text())
a.directory.mkdir(parents=True,exist_ok=True)
for entry in lock[a.platform]:
    path=a.directory/entry['file']
    if not path.exists():
        temporary=path.with_suffix(path.suffix+'.partial')
        with urllib.request.urlopen(entry['url'],timeout=120) as stream,temporary.open('wb') as out:
            while chunk:=stream.read(1024*1024):out.write(chunk)
        temporary.rename(path)
    with path.open('rb') as stream:actual=hashlib.file_digest(stream,'sha256').hexdigest()
    if actual!=entry['sha256']:raise SystemExit('SDK hash mismatch: '+path.name)
    print(path.name,actual)
