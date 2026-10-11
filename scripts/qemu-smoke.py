#!/usr/bin/env python3
"""Boot the complete appliance disk through BIOS using a disposable QEMU snapshot.

This captures evidence; it does not qualify the HP's Intel graphics or browser.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time



def intel_driver_evidence(serial):
    """Report dynamic DDX load/link evidence, independently of finding a GPU.

    The Intel driver can load correctly then find no supported adapter under
    QEMU standard VGA. Conversely, an attempted LoadModule line alone does not
    prove successful linking (the vbeFree failure happened at that boundary).
    """
    module = re.compile(r'\(II\)\s+Module intel:\s+vendor=', re.IGNORECASE)
    failed_module = re.compile(r'failed to load module\s+[\"\']?intel(?:[\"\']|\s|$)',
                               re.IGNORECASE)
    loaded = []
    errors = []
    for line in serial.splitlines():
        if module.search(line):
            loaded.append(line.strip()[:512])
        if ('undefined symbol' in line.lower() or failed_module.search(line) or
                ('intel_drv' in line.lower() and
                 re.search(r'failed|error|cannot|not found', line, re.IGNORECASE))):
            errors.append(line.strip()[:512])
    return {'intel_ddx_load_observed': bool(loaded),
            'intel_ddx_load_success': bool(loaded) and not errors,
            'intel_ddx_load_evidence': loaded[-8:],
            'intel_ddx_load_errors': errors[-16:]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--require-intel-ddx', action='store_true',
                        help='Require Intel driver module load/link evidence; this does not require or prove a graphical session')
    parser.add_argument('--menu-delay', type=float, default=1.5,
                        help='Seconds before selecting the third, serial GRUB entry')
    args = parser.parse_args()
    if not args.image.is_file() or not 10 <= args.seconds <= 300 or not 0.2 <= args.menu_delay <= 10:
        parser.error('Require a regular image file, 10–300 seconds and a 0.2–10 second menu delay')
    qemu = shutil.which('qemu-system-i386')
    if not qemu:
        parser.error('qemu-system-i386 is required')
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    image = args.image.resolve()
    digest = hashlib.sha256()
    with image.open('rb') as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    with tempfile.TemporaryDirectory(prefix='remoteos-qemu-') as runtime:
        endpoint = Path(runtime) / 'qmp.sock'
        command = [qemu, '-accel', 'tcg', '-cpu', 'n270', '-smp', '2', '-m', '2048',
                   '-boot', 'order=c', '-drive', f'file={image},format=raw,if=ide,snapshot=on',
                   '-nic', 'none', '-display', 'none', '-vga', 'std', '-monitor', 'none',
                   '-serial', f'file:{output / "serial.log"}',
                   '-qmp', f'unix:{endpoint},server=on,wait=off']
        receipt = {'image': str(image), 'sha256': digest.hexdigest(), 'command': command,
                   'qemu_version': subprocess.check_output([qemu, '--version'], text=True).splitlines()[0],
                   'hardware_qualified': False, 'graphical_session_qualified': False,
                   'intel_ddx_required': args.require_intel_ddx}
        with (output / 'qemu.log').open('w') as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.settimeout(5)
                started = time.monotonic()
                while True:
                    if process.poll() is not None:
                        raise RuntimeError('QEMU exited before its control socket became available')
                    try:
                        connection.connect(str(endpoint))
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if time.monotonic() - started > 20:
                            raise RuntimeError('QEMU control socket timed out')
                        time.sleep(0.1)
                channel = connection.makefile('rwb')
                json.loads(channel.readline())  # QMP greeting

                def request(name, arguments=None):
                    payload = {'execute': name}
                    if arguments is not None:
                        payload['arguments'] = arguments
                    channel.write(json.dumps(payload).encode() + b'\n')
                    channel.flush()
                    while True:
                        reply = json.loads(channel.readline())
                        if 'error' in reply:
                            raise RuntimeError(str(reply['error']))
                        if 'return' in reply:
                            return reply['return']

                request('qmp_capabilities')
                time.sleep(max(0, args.menu_delay - (time.monotonic() - started)))
                request('screendump', {'filename': str(output / 'grub.png'), 'format': 'png'})
                for key in ('down', 'down', 'ret'):
                    request('human-monitor-command', {'command-line': f'sendkey {key}'})
                    time.sleep(0.15)
                time.sleep(max(0, args.seconds - (time.monotonic() - started)))
                receipt['qemu_status'] = request('query-status')
                request('screendump', {'filename': str(output / 'boot.png'), 'format': 'png'})
                request('quit')
                channel.close()
                connection.close()
                process.wait(timeout=10)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        serial = (output / 'serial.log').read_text(errors='replace')
        receipt['kernel_observed'] = 'Linux version 6.12.109' in serial
        receipt['squashfs_root_observed'] = 'VFS: Mounted root (squashfs filesystem) readonly' in serial
        receipt['init_observed'] = 'Run /sbin/init as init process' in serial
        receipt['userspace_observed'] = 'Starting system dbus-daemon' in serial
        receipt.update(intel_driver_evidence(serial))
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt, indent=2))
        if not all(receipt[key] for key in ('kernel_observed', 'squashfs_root_observed', 'init_observed', 'userspace_observed')):
            raise SystemExit('Boot evidence incomplete; inspect screenshots/logs and GRUB menu timing.')
        if args.require_intel_ddx and not receipt['intel_ddx_load_success']:
            raise SystemExit('Intel DDX load/link evidence incomplete or failed; inspect serial.log. This check is independent of GUI startup.')


if __name__ == '__main__':
    main()
