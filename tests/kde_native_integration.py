#!/usr/bin/env python3
"""Real software KWin virtual outputs + private portal contract and PipeWire.

All compositor, D-Bus, PipeWire, application declarations and caches are private.
No actual desktop is captured, changed, or receives input. The portal fixture
checks the permission contract; it is not the real KDE permission dialog.
"""
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def wait_for(check, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value: return value
        time.sleep(0.05)
    raise AssertionError('Private KDE test readiness timed out')


def main():
    if '--private' not in sys.argv:
        required = ('kwin_wayland', 'kbuildsycoca6', 'pipewire', 'wireplumber', 'pw-dump', 'dbus-run-session', 'gst-launch-1.0')
        if any(not shutil.which(name) for name in required):
            print('KDE software compositor and PipeWire test tools are required'); return 77
        if subprocess.run(['/usr/bin/python3', '-c', 'import gi'], capture_output=True).returncode:
            print('System Python GIO introspection is required'); return 77
        # An ordinary file avoids D-Bus-activated child processes holding the
        # test runner's stdout pipe open after the private bus shuts down.
        with tempfile.TemporaryFile(mode='w+') as log:
            process = subprocess.Popen(['dbus-run-session', '--', '/usr/bin/python3', __file__, *sys.argv[1:], '--private'],
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try: status = process.wait(timeout=80)
            finally:
                try: os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait()
            log.seek(0); print(log.read(), end='')
            return status
    from gi.repository import Gio, GLib
    probe = str(Path(sys.argv[1]).resolve())
    host = str(Path(sys.argv[2]).resolve())
    session_client = str(Path(sys.argv[3]).resolve())
    processes, logs = [], []
    with tempfile.TemporaryDirectory(prefix='remoteos-kde-native-') as temp:
        root = Path(temp); runtime = root / 'runtime'; runtime.mkdir(mode=0o700)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'), XDG_STATE_HOME=str(root / 'state'),
                   WAYLAND_DISPLAY='remoteos-native-test', XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_TYPE='wayland',
                   REMOTEOS_PRIVATE_PORTAL_TEST='1', GST_REGISTRY_FORK='no', GST_REGISTRY=str(root / 'gst.bin'))
        plugins = root / 'plugins'; plugins.mkdir()
        candidates = [Path(p) for p in os.environ.get('GST_PLUGIN_PATH', '').split(':') if p]
        candidates.append(Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0'))
        for name in ('coreelements', 'app', 'pipewire', 'videoconvertscale', 'videotestsrc',
                     'udp', 'rtp', 'rtpmanager', 'dtls', 'srtp', 'libav', 'videoparsersbad',
                     'opus', 'audioconvert', 'audioresample', 'xvimagesink'):
            filename = 'libgst' + name + '.so'
            source = next((p / filename for p in candidates if (p / filename).is_file()), None)
            if source is None: raise RuntimeError('Test plugin missing: ' + filename)
            (plugins / filename).symlink_to(source.resolve())
        env.update(GST_PLUGIN_SYSTEM_PATH_1_0='', GST_PLUGIN_PATH_1_0=str(plugins))
        for key in ('DISPLAY', 'KWIN_WAYLAND_NO_PERMISSION_CHECKS', 'WAYLAND_SOCKET'): env.pop(key, None)
        # Explicit software composition; never use the host's live Wayland socket.
        compositor_env = dict(env, KWIN_COMPOSE='O2', QT_QUICK_BACKEND='software', LIBGL_ALWAYS_SOFTWARE='1',
                              QT_LOGGING_RULES='kwin_core.debug=true;KWIN_UTILS.debug=true')
        for key in ('LD_LIBRARY_PATH', 'QT_QPA_PLATFORM'): compositor_env.pop(key, None)
        def launch(argv, name, environment=env):
            log = (root / (name + '.log')).open('w+'); logs.append(log)
            process = subprocess.Popen(argv, env=environment, stdout=log, stderr=subprocess.STDOUT)
            processes.append(process); return process
        def nodes():
            return json.loads(subprocess.check_output(['pw-dump'], env=env, text=True, timeout=5))
        def native_nodes():
            return [v for v in nodes() if any(v.get('info', {}).get('props', {}).get(key, '').replace('kwin-screencast-Virtual-', 'kwin-screencast-').startswith('kwin-screencast-RemoteOS-HP2140-') for key in ('node.name', 'media.name'))]
        def fixture(method, params=None):
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            result = bus.call_sync('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop', 'org.remoteos.PortalTest',
                                   method, params, None, Gio.DBusCallFlags.NONE, 3000, None)
            return json.loads(result.unpack()[0]) if method == 'State' else None
        def run(mode, expected=None):
            fixture('Mode', GLib.Variant('(s)', (mode,)))
            try:
                result = subprocess.run([probe, mode], env=env, text=True, capture_output=True, timeout=25)
            except subprocess.TimeoutExpired as error:
                print('Private probe output:', error.stdout, error.stderr, file=sys.stderr)
                raise
            print(result.stdout, end='')
            if expected is None:
                assert result.returncode == 0, (mode, result.stdout, result.stderr)
                observed = fixture('State')
                assert not observed['keys'] and not observed['buttons'], observed
            else:
                assert result.returncode != 0 and expected in result.stderr, (mode, result.stdout, result.stderr)
                print(mode + ': ' + expected + ' rejected before capture')
            wait_for(lambda: not native_nodes(), 5)
        def session(mode, width, height):
            fixture('Mode', GLib.Variant('(s)', (mode,)))
            identity_dir = root / ('host-' + mode)
            identity = json.loads(subprocess.check_output([host, '--identity', str(identity_dir), '--init'], env=env, text=True, timeout=5))
            config = root / (mode + '.json')
            config.write_text(json.dumps({'identity_dir': str(identity_dir), 'pairing_code_file': identity['pairing_code_file'],
                'application_mode': True, 'desktop_source': 'kde-native', 'virtual_width': width, 'virtual_height': height,
                'bind': '127.0.0.1', 'port': 0, 'discovery_enabled': False, 'usb_enabled': False,
                'audio_source': 'none', 'bandwidth_limit_bps': 40000000, 'mpeg2_max_fps': 60 if width == 1366 else 30,
                'media_transport': 'rtp-udp', 'jitter_ms': 10}))
            server = launch([host, '--config', str(config), '--desktop'], mode + '-host')
            host_log = logs[-1]
            def listening():
                if server.poll() is not None: raise AssertionError('Native application host exited before listening')
                events = [json.loads(line) for line in Path(host_log.name).read_text().splitlines() if line.startswith('{')]
                failed = next((e for e in events if e.get('event') == 'sharing-failed'), None)
                if failed: raise AssertionError(failed)
                return next((e for e in events if e.get('event') == 'listening'), None)
            event = wait_for(listening, 15)
            owned_nodes = [v['id'] for v in native_nodes()]
            result = subprocess.run([session_client, str(event['port']), str(root / ('client-' + mode)),
                identity['fingerprint'], identity['pairing_code_file'], mode], env=env, capture_output=True, text=True, timeout=30)
            print(result.stdout, end='')
            assert result.returncode == 0, (mode, result.stdout, result.stderr)
            observed = fixture('State')
            assert not observed['keys'] and not observed['buttons'], observed
            assert observed['absolute'][-1][1:] == [width - 1, height - 1], observed
            assert owned_nodes == [v['id'] for v in native_nodes()], 'Native display was replaced on client reconnect'
            server.terminate(); assert server.wait(timeout=5) == 0
            wait_for(lambda: not native_nodes(), 5)
            print(mode + ': native MPEG-2 at negotiated 30/60 fps, trusted UDP, TLS pairing, adaptation and saved reconnect passed')
        try:
            launch(['pipewire'], 'pipewire')
            wait_for(lambda: (runtime / 'pipewire-0').exists())
            wp_env = dict(env); wp_env.pop('LD_LIBRARY_PATH', None)
            launch(['wireplumber', '--profile', 'policy'], 'wireplumber', wp_env)
            compositor = launch(['kwin_wayland', '--virtual', '--width', '800', '--height', '600', '--no-lockscreen',
                                 '--no-global-shortcuts', '--no-kactivities', '--socket', 'remoteos-native-test', '-platform', 'offscreen'],
                                'kwin', compositor_env)
            wait_for(lambda: (runtime / 'remoteos-native-test').exists() and compositor.poll() is None)
            for i, (w, h, color) in enumerate(((640, 480, 'red'), (800, 600, 'blue'))):
                launch(['gst-launch-1.0', '-q', 'videotestsrc', 'is-live=true', 'pattern=' + color, '!',
                        f'video/x-raw,format=BGRA,width={w},height={h},framerate=30/1', '!', 'pipewiresink', 'mode=provide',
                        f'stream-properties=props,node.name=remoteos-decoy-{i},media.class=Video/Source'], 'decoy-' + str(i),
                       env)
            def find_decoys():
                found = [v for v in nodes() if v.get('info', {}).get('props', {}).get('node.name', '').startswith('remoteos-decoy-')]
                return [{'id': v['id'], 'serial': v['info']['props']['object.serial']} for v in found] if len(found) == 2 else None
            (root / 'nodes.json').write_text(json.dumps(wait_for(find_decoys)))
            launch(['/usr/bin/python3', str(Path(__file__).with_name('portal_fixture.py')), str(root / 'nodes.json'), str(root / 'ready')], 'portal')
            wait_for(lambda: (root / 'ready').exists())
            run('kde-native-1024'); run('kde-native-1366')
            run('kde-native-foreign', 'did not grant')
            run('kde-native-wrong-size', 'geometry does not match')
            session('kde-native-1024', 1024, 576)
            session('kde-native-1366', 1366, 768)
            assert not any(v.get('type') == 'PipeWire:Interface:Device' for v in nodes()), 'Private policy opened a hardware device'
            print('Private KWin permission checks remained enabled; owned virtual outputs were removed after every success and failure')
        except Exception:
            print('Private test process states:', [(p.args, p.poll()) for p in processes], file=sys.stderr)
            for file in (root / 'data').rglob('*.desktop'):
                print('Private application declaration:', file.read_text(), file=sys.stderr)
            for file in (root / 'cache').rglob('ksycoca*'):
                if file.is_file(): print('Private service cache:', file.name, 'contains probe:', b'remoteos-wayland-probe' in file.read_bytes(), file=sys.stderr)
            for log in logs:
                log.flush(); log.seek(0); print(log.name + '\n' + log.read(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=4)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
            for log in logs: log.close()
    return 0


if __name__ == '__main__': sys.exit(main())
