#!/usr/bin/env python3
"""Run the built i386 supplicant only on a private global Unix socket.

No network interface, saved profile, system control socket, or network namespace
is used. This checks the real daemon's compiled options and request/response path
without claiming that radio association, DHCP, or internet access have passed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time


def probe(target):
    loader = target / 'lib/ld-linux.so.2'
    prefix = [str(loader), '--library-path', f'{target / "lib"}:{target / "usr/lib"}']
    daemon = prefix + [str(target / 'usr/sbin/wpa_supplicant')]
    cli = prefix + [str(target / 'usr/sbin/wpa_cli')]
    result = {
        'scope': 'private Unix global control socket; no network interfaces',
        'supplicant_sha256': hashlib.sha256((target / 'usr/sbin/wpa_supplicant').read_bytes()).hexdigest(),
        'hardware_association_tested': False,
        'passed': False,
    }
    version = subprocess.run(daemon + ['-s', '-v'], capture_output=True, text=True, timeout=5)
    result['syslog_option_exit_code'] = version.returncode
    result['version_output'] = version.stdout.strip()
    # This upstream parser can print usage and still return zero for an option
    # excluded at compile time. A successful exit code alone is insufficient.
    version_text = version.stdout.strip()
    if version.returncode != 0 or 'usage:' in version_text or not version_text.startswith('wpa_supplicant v'):
        result['failure'] = 'daemon rejects requested -s option before startup'
        return result
    with tempfile.TemporaryDirectory(prefix='remoteos-wifi-probe-') as directory:
        socket = Path(directory) / 'control'
        # Avoid inheriting a user's arbitrary logging/debug environment. The
        # command uses no -i, -c, -u, or default global socket; -g names only ours.
        process = subprocess.Popen(daemon + ['-s', '-g', str(socket)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
                                   env={'PATH': '/usr/bin:/bin', 'LANG': 'C'})
        try:
            deadline = time.monotonic() + 5
            while not socket.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            result['socket_created'] = socket.is_socket()
            if not result['socket_created']:
                result['failure'] = 'daemon did not create private global control socket'
            else:
                ping = subprocess.run(cli + ['-g', str(socket), 'ping'], capture_output=True,
                                      text=True, timeout=5)
                result['ping_exit_code'] = ping.returncode
                result['ping_response'] = ping.stdout.strip()
                result['passed'] = ping.returncode == 0 and ping.stdout.strip() == 'PONG'
                if not result['passed']:
                    result['failure'] = 'private control socket did not answer PING'
        finally:
            process.terminate()
            try:
                stdout, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
            result['daemon_exit_code'] = process.returncode
            result['daemon_output'] = (stdout + stderr)[:4096]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = probe(args.target.resolve())
    document = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(document)
    print(document, end='')
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
