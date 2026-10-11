#!/usr/bin/env python3
"""Reject explicit Buildroot/kernel settings silently dropped by Kconfig."""
import re
import sys
from pathlib import Path

def values(path):
    text = Path(path).read_text()
    result = dict(re.findall(r'^((?:BR2_|CONFIG_)[A-Za-z0-9_]+)=(.*)$', text, re.M))
    result.update((key, 'n') for key in re.findall(r'^# ((?:BR2_|CONFIG_)[A-Za-z0-9_]+) is not set$', text, re.M))
    return result

requested, resolved = map(values, sys.argv[1:3])
errors = []
for key, value in requested.items():
    actual = resolved.get(key, 'n')
    if actual != value:
        errors.append(f'{key}: requested {value}, resolved {actual}')
if errors:
    sys.exit('\n'.join(errors))
print(f'Validated {len(requested)} explicit Kconfig settings.')
