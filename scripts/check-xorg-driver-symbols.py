#!/usr/bin/env python3
"""Check an Xorg driver's initial symbol scope, including transitive DT_NEEDED.

Optional Xorg modules are intentionally excluded: a BIND_NOW driver cannot
depend on symbols in modules it intends to load after its own dlopen succeeds.
Every ELF object in the initial closure is checked against the combined scope.
This is a packaging/linkage guard, not a hardware initialization test.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def dynamic_symbols(path, readelf):
    output = subprocess.check_output(
        [readelf, '--dyn-syms', '--wide', str(path)], text=True)
    required, exported = set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(':'):
            continue
        binding, visibility, section, name = fields[4:8]
        if section == 'UND':
            if binding == 'GLOBAL':
                required.add(name)
        elif binding in ('GLOBAL', 'WEAK') and visibility in ('DEFAULT', 'PROTECTED'):
            exported.add(name.replace('@@', '@'))
            if '@@' in name or '@' not in name:
                exported.add(name.split('@')[0])
    return required, exported


def check(target, server, module, readelf='readelf'):
    target = target.resolve()
    initial = [target / server.lstrip('/'), target / module.lstrip('/')]
    work = deque(initial)
    visited, exports, module_required = set(), set(), set()
    required_by_object = {}
    missing_libraries = set()
    while work:
        path = work.popleft().resolve()
        if path in visited:
            continue
        if not path.is_relative_to(target):
            raise ValueError(f'ELF path escapes target: {path}')
        visited.add(path)
        required, provided = dynamic_symbols(path, readelf)
        required_by_object[str(path.relative_to(target))] = required
        exports.update(provided)
        if path == initial[1].resolve():
            module_required = required
        dynamic = subprocess.check_output([readelf, '-d', str(path)], text=True)
        for soname in re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic):
            if '/' in soname:
                raise ValueError(f'Unexpected dependency path: {soname}')
            for directory in ('lib', 'usr/lib'):
                dependency = target / directory / soname
                if dependency.is_file():
                    work.append(dependency)
                    break
            else:
                missing_libraries.add(soname)
    unresolved_by_object = {
        name: sorted(required - exports)
        for name, required in sorted(required_by_object.items())
        if required - exports
    }
    all_required = set().union(*required_by_object.values())
    unresolved = sorted(all_required - exports)
    return {
        'status': 'failed' if unresolved or missing_libraries else 'passed',
        'server': server, 'module': module,
        'sha256': {str(path.relative_to(target)): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in initial},
        'strong_undefined_symbols': len(module_required),
        'initial_scope_elf_files': len(visited),
        'initial_scope_strong_undefined_symbols': sum(map(len, required_by_object.values())),
        'initial_scope_unique_required_symbols': len(all_required),
        'module_unresolved_symbols': sorted(module_required - exports),
        'unresolved_symbols': unresolved,
        'unresolved_by_object': unresolved_by_object,
        'missing_libraries': sorted(missing_libraries),
        'scope': 'Xorg executable and driver DT_NEEDED closures; no optional modules',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', type=Path)
    parser.add_argument('--server', default='/usr/bin/Xorg')
    parser.add_argument('--module', default='/usr/lib/xorg/modules/drivers/intel_drv.so')
    parser.add_argument('--readelf', default='readelf')
    args = parser.parse_args()
    try:
        result = check(args.target, args.server, args.module, args.readelf)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Xorg symbol check failed: {error}')
    print(json.dumps(result, indent=2))
    sys.exit(0 if result['status'] == 'passed' else 1)
