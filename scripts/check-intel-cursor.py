#!/usr/bin/env python3
"""Reject an HP image selecting a cursor option absent from its Intel driver."""
import argparse
import hashlib
import json
from pathlib import Path
import re

MARKER = b'Software cursor enabled (SWCursor)'
CONFIG = 'etc/X11/xorg.conf.d/10-remoteos.conf'
DRIVER = 'usr/lib/xorg/modules/drivers/intel_drv.so'


def check(target):
    target = Path(target)
    errors = []
    try:
        text = (target / CONFIG).read_text()
        # Remove comments so a documented example cannot satisfy the guard.
        text = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
        devices = re.findall(r'Section\s+"Device"(.*?)EndSection', text,
                             re.IGNORECASE | re.DOTALL)
        intel = [section for section in devices
                 if re.search(r'Driver\s+"intel"', section, re.IGNORECASE)]
        if len(intel) != 1:
            errors.append('expected one Intel Device section')
        else:
            for name, expected in (('AccelMethod', 'sna'), ('SWCursor', 'true'), ('SwapbuffersWait', 'false')):
                values = re.findall(r'Option\s+"' + name + r'"\s+"([^"]+)"',
                                    intel[0], re.IGNORECASE)
                if [value.lower() for value in values] != [expected]:
                    errors.append('Intel Device must select %s=%s exactly once' %
                                  (name, expected))
    except OSError as error:
        errors.append('cannot read cursor configuration: ' + str(error))
    digest = None
    try:
        binary = (target / DRIVER).read_bytes()
        digest = hashlib.sha256(binary).hexdigest()
        if MARKER + b'\n\0' not in binary:
            errors.append('Intel driver lacks compiled SWCursor support; clean-rebuild the package')
    except OSError as error:
        errors.append('cannot read Intel driver: ' + str(error))
    return {'status': 'failed' if errors else 'passed', 'config': CONFIG,
            'driver': DRIVER, 'driver_sha256': digest,
            'cursor_policy': 'SNA with software cursor', 'video_policy': 'immediate textured video without scanline waits', 'errors': errors}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', type=Path)
    args = parser.parse_args()
    result = check(args.target)
    print(json.dumps(result, indent=2))
    raise SystemExit(result['status'] != 'passed')
