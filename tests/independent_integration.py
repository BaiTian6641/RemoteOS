#!/usr/bin/env python3
"""Exercise two private displays; never capture or inject the real desktop."""
import importlib.util
import json
import math
import os
from pathlib import Path
import secrets
import select
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import struct
import wave


def main():
    host, probe, launcher = (Path(value).resolve() for value in sys.argv[1:])
    xvfb = shutil.which('Xvfb')
    if not xvfb or not shutil.which('Xtigervnc') or not shutil.which('openbox') or os.geteuid() == 0:
        print('Private Xvfb/Xtigervnc/Openbox and a non-root test user are required')
        return 77
    spec = importlib.util.spec_from_file_location('private_desktop', launcher)
    private = importlib.util.module_from_spec(spec); spec.loader.exec_module(private)
    plasma_test = bool(os.environ.get('REMOTEOS_INDEPENDENT_TEST_PLASMA'))
    if plasma_test and any(not shutil.which(name) for name in ('kwin_wayland', 'plasmashell', 'kded6', 'kscreen-doctor')):
        print('Installed KDE Plasma is required for the matching desktop test'); return 77
    with tempfile.TemporaryDirectory(prefix='remoteos-independent-test-') as tmp:
        root = Path(tmp)
        auth = root / 'local-authority'
        cookie = secrets.token_bytes(16); private.authority(auth, 0, cookie)
        read_fd, write_fd = os.pipe()
        server = local = audio_player = plasma_app = session = None
        with (root / 'local.log').open('w+') as local_log, (root / 'desktop.log').open('w+') as log:
            try:
                local = subprocess.Popen([xvfb, '-displayfd', str(write_fd), '-screen', '0', '640x480x24', '-nolisten', 'tcp', '-auth', str(auth)], pass_fds=(write_fd,), stdout=local_log, stderr=local_log)
                os.close(write_fd); write_fd = -1
                if not select.select([read_fd], [], [], 10)[0]: raise AssertionError('Private local Xvfb did not start')
                number = os.read(read_fd, 32).decode().strip()
                if not number.isdecimal(): raise AssertionError('Private local Xvfb exited')
                private.authority(auth, number, cookie)
                local_display = ':' + number
                env = dict(os.environ, DISPLAY=local_display, XAUTHORITY=str(auth), XDG_SESSION_TYPE='wayland',
                           WAYLAND_DISPLAY='must-not-use-the-local-compositor', DBUS_SESSION_BUS_ADDRESS='unix:path=must-not-use-the-local-bus')
                if plasma_test:
                    original = root / 'original-config'; original.mkdir()
                    (original / 'kdeglobals').write_text('[General]\nColorScheme=BreezeDark\n[Icons]\nTheme=breeze-dark\n')
                    env.update(XDG_CURRENT_DESKTOP='KDE', DESKTOP_SESSION='plasma', XDG_CONFIG_HOME=str(original), XDG_DATA_HOME=str(root / 'local-data'))
                    env['REMOTEOS_PRIVATE_ACTIVATION_TEST'] = '1'
                    # Register before dbus-run-session starts. Calling this
                    # service later must use the daemon's activation values,
                    # not values supplied directly to a desktop subprocess.
                    services = root / 'independent-desktop/data/dbus-1/services'
                    services.mkdir(parents=True)
                    activation_report = root / 'activated-session.json'
                    fixture = Path(__file__).with_name('session_activation_fixture.py').resolve()
                    (services / 'org.remoteos.TestActivation.service').write_text(
                        '[D-BUS Service]\nName=org.remoteos.TestActivation\nExec=' +
                        shlex.join(['/usr/bin/python3', str(fixture), str(activation_report)]) + '\n')
                    unowned_env = {key: value for key, value in env.items() if key != 'REMOTEOS_INDEPENDENT_DESKTOP'}
                    unowned = subprocess.run([sys.executable, str(launcher), '--plasma-inside',
                        '--config', str(root / 'not-a-session.json'), '--host', str(host)],
                        env=unowned_env, capture_output=True, text=True, timeout=5)
                    if unowned.returncode == 0 or 'launcher-owned independent desktop' not in unowned.stdout:
                        raise AssertionError('Unowned Plasma startup reached session setup or D-Bus activation')
                initialized = subprocess.run([str(host), '--identity', str(root / 'identity'), '--init'], env=env, capture_output=True, text=True, check=True, timeout=10)
                identity = json.loads(initialized.stdout)
                config = root / 'host.json'
                config.write_text(json.dumps({'identity_dir': str(root / 'identity'), 'pairing_code_file': identity['pairing_code_file'],
                    'bind': '127.0.0.1', 'port': 0, 'application_mode': True, 'desktop_source': 'independent', 'independent_environment': 'auto' if plasma_test else 'minimal',
                    'virtual_width': 1024, 'virtual_height': 576, 'mpeg2_max_fps': 60, 'media_transport': 'rtp-udp',
                    'jitter_ms': 10, 'audio_source': 'pipewire-user' if os.environ.get('REMOTEOS_INDEPENDENT_TEST_AUDIO') else 'none', 'usb_enabled': False, 'discovery_enabled': False, 'bandwidth_limit_bps': 40000000}))
                config.chmod(0o600)
                # Exercise ownership of applications which double-fork and
                # create their own process group, like some desktop launchers.
                wrapper = root / 'host-with-detached-child.py'
                detached = root / 'detached.pid'
                wrapper.write_text('#!' + sys.executable + '\n' +
                    'import os, pathlib, sys, time\n' +
                    'if os.fork() == 0:\n' +
                    ' os.setsid()\n' +
                    ' if os.fork() != 0: os._exit(0)\n' +
                    ' pathlib.Path(' + repr(str(detached)) + ').write_text(str(os.getpid()))\n' +
                    ' time.sleep(90); os._exit(0)\n' +
                    'os.execv(' + repr(str(host)) + ', [' + repr(str(host)) + '] + sys.argv[1:])\n')
                wrapper.chmod(0o700)
                server = subprocess.Popen([sys.executable, str(launcher), '--host', str(wrapper), '--config', str(config)], env=env, stdout=subprocess.PIPE, stderr=log, text=True)
                deadline = time.monotonic() + 20
                session = listening = None
                while not listening:
                    if not select.select([server.stdout], [], [], max(0, deadline - time.monotonic()))[0]: raise AssertionError('Independent host did not start')
                    line = server.stdout.readline()
                    if not line: raise AssertionError('Independent supervisor exited')
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError as error:
                        raise AssertionError('Unexpected independent host event: ' + repr(line)) from error
                    if event.get('event') == 'sharing-failed': raise AssertionError(event)
                    if event.get('event') == 'independent-desktop': session = event
                    if event.get('event') == 'listening': listening = event
                if not session or session['display'] == local_display: raise AssertionError('The host reused the local display')
                runtime = Path(session['runtime'])
                if runtime.stat().st_mode & 0o777 != 0o700: raise AssertionError('Session runtime is not private')
                for port in (5900 + int(session['display'][1:]), 6000 + int(session['display'][1:])):
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.3): raise AssertionError('Virtual desktop exposed a TCP listener')
                    except ConnectionRefusedError:
                        pass
                both = root / 'both.authority'
                both.write_bytes(auth.read_bytes() + (runtime / 'Xauthority').read_bytes()); both.chmod(0o600)
                probe_env = dict(os.environ, DISPLAY=session['display'], XAUTHORITY=str(both), XDG_SESSION_TYPE='x11',
                                 REMOTEOS_INDEPENDENT_DESKTOP='1', REMOTEOS_PRIVATE_X_TEST='1', REMOTEOS_LOCAL_TEST_DISPLAY=local_display,
                                 XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime), PULSE_SERVER='unix:' + str(runtime / 'pulse/native'))
                if plasma_test:
                    if session['environment'] != 'plasma': raise AssertionError('Matching mode substituted the minimal desktop')
                    if session.get('presentation_clock_fps') != 60: raise AssertionError('Private compositor clock is not active')
                    desktop_env = json.loads((runtime / 'plasma-desktop-environment.json').read_text())
                    subprocess.run(['qdbus6', 'org.freedesktop.DBus', '/org/freedesktop/DBus',
                        'org.freedesktop.DBus.StartServiceByName', 'org.remoteos.TestActivation', '0'],
                        env=desktop_env, check=True, capture_output=True, text=True, timeout=8)
                    activated = json.loads(activation_report.read_text())
                    mismatched = [key for key, value in activated.items() if value != desktop_env.get(key)]
                    if mismatched:
                        raise AssertionError('D-Bus helpers use another desktop: ' + ', '.join(mismatched))
                    if activated['DISPLAY'] == session['display'] or activated['QT_QPA_PLATFORM'] != 'wayland':
                        raise AssertionError('D-Bus helper dialogs target the hidden outer display')
                    print('Private D-Bus activation matches visible KDE Wayland/Xwayland session; wallet and portal dialogs stay reachable')
                    probe_env.update(REMOTEOS_NESTED_LAYOUT_HELPER=str(launcher), REMOTEOS_CAPTURE_DISPLAY=session['display'],
                                     REMOTEOS_CAPTURE_AUTHORITY=str(runtime / 'Xauthority'), REMOTEOS_PLASMA_TEST_EVENTS=str(root / 'plasma-app-events.json'),
                                     DBUS_SESSION_BUS_ADDRESS=desktop_env['DBUS_SESSION_BUS_ADDRESS'],
                                     WAYLAND_DISPLAY=desktop_env['WAYLAND_DISPLAY'],
                                     XDG_CONFIG_HOME=desktop_env['XDG_CONFIG_HOME'], XDG_DATA_HOME=desktop_env['XDG_DATA_HOME'],
                                     XDG_CACHE_HOME=desktop_env['XDG_CACHE_HOME'])
                    # This fixture checks the physical ASCII input path; it
                    # does not depend on the builder's separate global IME.
                    plasma_app = subprocess.Popen(['/usr/bin/python3', str(Path(__file__).with_name('plasma_fixture.py')), str(root / 'plasma-app-events.json')],
                        env=dict(desktop_env, GTK_IM_MODULE='simple'), stdout=log, stderr=log)
                    deadline = time.monotonic() + 8
                    while not (root / 'plasma-app-events.json').exists() and time.monotonic() < deadline:
                        if plasma_app.poll() is not None: raise AssertionError('Native Wayland app exited')
                        time.sleep(.05)
                    if not (root / 'plasma-app-events.json').exists(): raise AssertionError('Native Wayland app did not start')
                audio_player = None
                if os.environ.get('REMOTEOS_INDEPENDENT_TEST_AUDIO'):
                    devices = json.loads(subprocess.check_output(['pw-dump'], env=probe_env, text=True, timeout=5))
                    node_names = [item.get('info', {}).get('props', {}).get('node.name', '') for item in devices]
                    if 'remoteos-output' not in node_names or any(name.startswith(('alsa_', 'bluez_')) for name in node_names):
                        raise AssertionError('Independent audio is missing or has attached host hardware')
                    sound = root / 'tone.wav'
                    with wave.open(str(sound), 'wb') as output:
                        output.setnchannels(2); output.setsampwidth(2); output.setframerate(48000)
                        output.writeframes(b''.join(struct.pack('<hh', int(12000 * math.sin(2 * math.pi * 440 * i / 48000)), int(12000 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000 * 8)))
                    audio_player = subprocess.Popen(['pw-play', '--target', 'remoteos-output', str(sound)], env=probe_env, stdout=log, stderr=log)
                result = subprocess.run([str(probe), str(listening['port']), str(root / 'client'), identity['fingerprint'], identity['pairing_code_file']],
                                        env=probe_env, capture_output=True, text=True, timeout=40)
                print(result.stdout, end='')
                if result.returncode: raise AssertionError(result.stderr)
                if audio_player:
                    audio_player.terminate(); audio_player.wait(timeout=5)
                if server.poll() is not None: raise AssertionError('Supervisor died during capture')
                detached_pid = int(detached.read_text())
                if not Path('/proc/' + str(detached_pid)).exists(): raise AssertionError('Detached application fixture did not stay alive')
                if plasma_test:
                    result = subprocess.run(['qdbus6', 'org.kde.plasmashell', '/PlasmaShell', 'org.kde.PlasmaShell.evaluateScript',
                                             'print(JSON.stringify({desktops:desktops().length,panels:panels().length}))'], env=desktop_env, capture_output=True, text=True, timeout=8)
                    if result.returncode: raise AssertionError('Real Plasma shell was not running: ' + result.stderr)
                    shell = json.loads(result.stdout)
                    if shell['desktops'] != 2 or shell['panels'] < 1: raise AssertionError(shell)
                    inventory = json.loads(subprocess.check_output(['kscreen-doctor', '-j'],
                        env={**{k: v for k, v in desktop_env.items() if k != 'KSCREEN_BACKEND'},
                             'KSCREEN_BACKEND_INPROCESS': '1'}, text=True, timeout=8))
                    outputs = [item for item in inventory['outputs'] if item.get('enabled')]
                    outputs.sort(key=lambda item: item['name'])
                    if len(outputs) != 2: raise AssertionError(inventory)
                    for item, expected in zip(outputs, ((0, 0, 1366, 768), (1366, 0, 640, 480))):
                        mode = next(mode for mode in item['modes'] if mode['id'] == item['currentModeId'])
                        actual = (item['pos']['x'], item['pos']['y'], mode['size']['width'], mode['size']['height'])
                        if actual != expected or item['scale'] != 1: raise AssertionError((actual, expected, item))
                    if (original / 'kdeglobals').read_text() != '[General]\nColorScheme=BreezeDark\n[Icons]\nTheme=breeze-dark\n':
                        raise AssertionError('Remote Plasma modified the logged-in desktop settings')
                    print('Matching KDE Plasma shell, native Wayland application input, two desktops, panel, private appearance and local input isolation passed')
                # Capture the desktop in this fixture for visual validation only.
                screenshot = os.environ.get('REMOTEOS_INDEPENDENT_SCREENSHOT')
                if screenshot and shutil.which('scrot'):
                    subprocess.run(['scrot', screenshot], env={k: v for k, v in probe_env.items() if k not in private.RUNTIME_KEYS}, check=True, timeout=10)
                server.terminate(); server.wait(timeout=5)
                if plasma_test:
                    component_log = (config.parent / 'independent-desktop/independent-desktop.log').read_text()
                    if 'Framebuffer updates: 0' not in component_log:
                        raise AssertionError('Private clock client requested or encoded VNC framebuffer updates')
                    print('Private presentation clock active; zero VNC framebuffer updates passed')
                if Path('/proc/' + str(detached_pid)).exists(): raise AssertionError('Stop sharing left a detached application alive')
                if runtime.exists(): raise AssertionError('Stop sharing left session credentials or sockets')
                with socket.socket(socket.AF_UNIX) as client:
                    try: client.connect('/tmp/.X11-unix/X' + session['display'][1:])
                    except (FileNotFoundError, ConnectionRefusedError): pass
                    else: raise AssertionError('Independent X server survived Stop sharing')
                if local.poll() is not None: raise AssertionError('Stop sharing terminated the local X server')
                print('Detached application cleanup passed')
                print('Independent desktop startup, native resize, dual streams, input isolation, TCP exposure and cleanup passed')
            except Exception:
                if plasma_test and os.environ.get('REMOTEOS_INDEPENDENT_SCREENSHOT') and 'probe_env' in locals():
                    subprocess.run(['scrot', os.environ['REMOTEOS_INDEPENDENT_SCREENSHOT']],
                        env={k: v for k, v in probe_env.items() if k not in private.RUNTIME_KEYS}, timeout=10)
                if plasma_test and (root / 'plasma-app-events.json').exists():
                    print('Native Plasma application events:', (root / 'plasma-app-events.json').read_text(), file=sys.stderr)
                if plasma_test and session and (Path(session['runtime']) / 'plasma-layout-last.json').exists():
                    print('Private output layout:', (Path(session['runtime']) / 'plasma-layout-last.json').read_text(), file=sys.stderr)
                for stream in (local_log, log):
                    stream.flush(); stream.seek(0); print(stream.read(), file=sys.stderr)
                for path in root.glob('independent-desktop/*.log'):
                    print(path.read_text(), file=sys.stderr)
                raise
            finally:
                for process in (plasma_app, audio_player, server, local):
                    if process and process.poll() is None:
                        process.terminate()
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                os.close(read_fd)
                if write_fd >= 0: os.close(write_fd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
