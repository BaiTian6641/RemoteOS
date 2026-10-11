#!/usr/bin/env python3
"""Actual application capture/input only on a private cookie-authenticated Xvfb.

Never connect to the user's display or open their input devices.
"""
import json
import os
import pathlib
import secrets
import selectors
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    host, probe = sys.argv[1:]
    xvfb = os.environ.get('REMOTEOS_XVFB') or shutil.which('Xvfb')
    if not xvfb or not shutil.which('xauth'):
        print('Private Xvfb and xauth are required'); return 77
    with tempfile.TemporaryDirectory(prefix='remoteos-desktop-') as directory:
        root = pathlib.Path(directory)
        authority = root / 'Xauthority'; authority.touch(mode=0o600)
        subprocess.run(['xauth', '-f', str(authority), 'add', ':96', 'MIT-MAGIC-COOKIE-1', secrets.token_hex(16)], check=True, timeout=5)
        env = dict(os.environ, DISPLAY=':96', XAUTHORITY=str(authority), XDG_SESSION_TYPE='x11', REMOTEOS_PRIVATE_X_TEST='1')
        with (root / 'xvfb.log').open('w+') as xlog, (root / 'host.log').open('w+') as log:
            xserver = subprocess.Popen([xvfb, ':96', '-screen', '0', '640x480x24', '-nolisten', 'tcp', '-auth', str(authority)], env=env, stdout=xlog, stderr=xlog)
            server = None
            try:
                time.sleep(0.5)
                if xserver.poll() is not None: raise AssertionError('Private Xvfb failed to start')
                initialized = subprocess.run([host, '--identity', str(root / 'host'), '--init'], text=True, capture_output=True, check=True, timeout=10, env=env)
                identity = json.loads(initialized.stdout)
                config = root / 'host.json'
                config.write_text(json.dumps({'identity_dir': str(root / 'host'), 'pairing_code_file': identity['pairing_code_file'],
                                             'bind': '127.0.0.1', 'port': 0, 'application_mode': True, 'audio_source': 'none', 'usb_enabled': False,
                                             'discovery_enabled': False, 'bandwidth_limit_bps': 1000000000}))
                server = subprocess.Popen([host, '--config', str(config), '--desktop'], stdout=subprocess.PIPE, stderr=log, text=True, env=env)
                with selectors.DefaultSelector() as selector:
                    selector.register(server.stdout, selectors.EVENT_READ)
                    deadline = time.monotonic() + 10
                    while True:
                        if not selector.select(timeout=max(0, deadline - time.monotonic())): raise AssertionError('Host did not start')
                        listening = json.loads(server.stdout.readline())
                        if listening.get('event') == 'listening': break
                        if listening.get('event') == 'sharing-failed': raise AssertionError(listening)
                result = subprocess.run([probe, str(listening['port']), str(root / 'client'), identity['fingerprint'], identity['pairing_code_file']],
                                        capture_output=True, text=True, env=env, timeout=30)
                print(result.stdout, end='')
                if result.returncode: raise AssertionError(result.stderr)
                if server.poll() is not None: raise AssertionError('Host exited during session')
            except Exception:
                for stream in (xlog, log):
                    stream.flush(); stream.seek(0); print(stream.read(), file=sys.stderr)
                raise
            finally:
                for process in (server, xserver):
                    if process:
                        process.terminate()
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                if server: server.stdout.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
