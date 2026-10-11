#!/usr/bin/env python3
"""Private portal-contract + real PipeWire/TLS/DTLS integration.

No compositor permissions are requested. The fixture and PipeWire instance
use a private D-Bus and temporary runtime, without WirePlumber/device access.
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


def stop(process):
    if not process: return
    process.terminate()
    try: process.wait(timeout=6)
    except subprocess.TimeoutExpired: process.kill(); process.wait()


def wait_for(check, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result: return result
        time.sleep(0.03)
    raise AssertionError('Private fixture readiness timed out')


def main():
    if '--private' not in sys.argv:
        requirements = ('pipewire', 'pw-dump', 'dbus-run-session', 'gst-launch-1.0')
        if any(not shutil.which(tool) for tool in requirements):
            print('Private PipeWire, D-Bus and GStreamer launch tools required'); return 77
        if subprocess.run(['/usr/bin/python3', '-c', 'import gi'], capture_output=True).returncode:
            print('System Python GIO introspection required'); return 77
        return subprocess.run(['dbus-run-session', '--', '/usr/bin/python3', __file__, '--private', *sys.argv[1:]], timeout=95).returncode

    from gi.repository import Gio, GLib
    host, probe, session_client = map(str, map(Path, sys.argv[2:]))
    with tempfile.TemporaryDirectory(prefix='remoteos-wayland-') as directory:
        root = Path(directory); root.chmod(0o700)
        runtime = root / 'runtime'; runtime.mkdir(mode=0o700)
        config_dir = root / 'config'; config_dir.mkdir()
        pipewire_config = config_dir / 'pipewire' / 'pipewire.conf.d'; pipewire_config.mkdir(parents=True)
        # Only the PRIVATE server: no hardware/session-manager policy. The
        # fixture explicitly links its two generated nodes to requested inputs.
        (pipewire_config / 'remoteos-private.conf').write_text('context.properties = { module.portal = false module.rt = false }\n')
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config_dir),
                   PIPEWIRE_RUNTIME_DIR=str(runtime), XDG_SESSION_TYPE='wayland', WAYLAND_DISPLAY='private-fixture',
                   REMOTEOS_PRIVATE_PORTAL_TEST='1')
        # Never inherit an actual desktop capture target/Xauthority.
        env.pop('DISPLAY', None); env.pop('XAUTHORITY', None)
        processes, logs = [], []
        def launch(command, name, stdout=None, environment=None):
            log = (root / (name + '.log')).open('w+'); logs.append(log)
            process = subprocess.Popen(command, env=environment or env, stdout=stdout or log, stderr=log, text=True)
            processes.append(process); return process
        def run(command, timeout=25):
            process = subprocess.Popen(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                out, err = process.communicate(timeout=timeout)
                result = subprocess.CompletedProcess(command, process.returncode, out, err)
            except subprocess.TimeoutExpired as error:
                snapshot = json.loads(subprocess.check_output(['pw-dump'], env=env, text=True, timeout=5))
                print(json.dumps([{'id': v['id'], 'type': v['type'], 'state': v.get('info', {}).get('state'), 'props': v.get('info', {}).get('props')}
                                  for v in snapshot if v['type'] in ('PipeWire:Interface:Node', 'PipeWire:Interface:Link')], indent=2), file=sys.stderr)
                process.kill(); out, err = process.communicate(); print(out, file=sys.stderr); print(err, file=sys.stderr); raise
            print(result.stdout, end='')
            if result.returncode: raise AssertionError(result.stderr or 'Command failed')
            return result
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        def fixture(method, parameters=None):
            result = bus.call_sync('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                                  'org.remoteos.PortalTest', method, parameters, None, Gio.DBusCallFlags.NONE, 3000, None)
            return json.loads(result.unpack()[0]) if method == 'State' else None
        try:
            launch(['pipewire'], 'pipewire')
            wait_for(lambda: (runtime / 'pipewire-0').exists())
            for i, (w, h, color) in enumerate(((640, 480, 'red'), (800, 600, 'blue'))):
                launch(['gst-launch-1.0', '-q', 'videotestsrc', 'is-live=true', 'pattern=' + color, '!',
                        f'video/x-raw,format=BGRA,width={w},height={h},framerate=60/1', '!', 'pipewiresink', 'mode=provide',
                        f'stream-properties=props,node.name=remoteos-private-{i},media.class=Video/Source'], 'source-' + str(i),
                       environment=dict(env, GST_PLUGIN_SYSTEM_PATH_1_0='/usr/lib/x86_64-linux-gnu/gstreamer-1.0'))
            def find_nodes():
                values = json.loads(subprocess.check_output(['pw-dump'], env=env, text=True, timeout=5))
                found = []
                for i in range(2):
                    node = next((v for v in values if v.get('info', {}).get('props', {}).get('node.name') == 'remoteos-private-' + str(i)), None)
                    if not node: return None
                    found.append({'id': node['id'], 'serial': node['info']['props']['object.serial']})
                return found
            nodes = wait_for(find_nodes); (root / 'nodes.json').write_text(json.dumps(nodes))
            launch(['/usr/bin/python3', str(Path(__file__).with_name('portal_fixture.py')), str(root / 'nodes.json'), str(root / 'ready')], 'portal')
            wait_for(lambda: (root / 'ready').exists())
            for mode in ('normal', 'revoke'):
                fixture('Mode', GLib.Variant('(s)', ('normal',))); run([probe, mode])
            fixture('Mode', GLib.Variant('(s)', ('slow-ack',))); run([probe, 'slow-ack'])
            for mode in ('physical-match', 'virtual-match', 'virtual-only', 'virtual-fixed'):
                fixture('Mode', GLib.Variant('(s)', (mode,))); run([probe, mode])
            identity = json.loads(run([host, '--identity', str(root / 'host'), '--init']).stdout)
            config = root / 'host.json'
            config.write_text(json.dumps({'identity_dir': str(root / 'host'), 'pairing_code_file': identity['pairing_code_file'],
                                          'application_mode': True, 'bind': '127.0.0.1', 'port': 0, 'audio_source': 'none', 'usb_enabled': False,
                                          'discovery_enabled': False, 'bandwidth_limit_bps': 1000000000}))
            fixture('Mode', GLib.Variant('(s)', ('normal',)))
            server = launch([host, '--config', str(config), '--desktop'], 'host', stdout=subprocess.PIPE)
            with selectors.DefaultSelector() as selector:
                selector.register(server.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 25
                while True:
                    if not selector.select(timeout=max(0, deadline - time.monotonic())): raise AssertionError('Host permission/capture did not complete')
                    line = server.stdout.readline()
                    if not line: raise AssertionError('Host exited while preparing')
                    event = json.loads(line)
                    if event.get('event') == 'sharing-failed': raise AssertionError(event)
                    if event.get('event') == 'listening': break
            run([session_client, str(event['port']), str(root / 'client'), identity['fingerprint'], identity['pairing_code_file']], timeout=35)
            state = fixture('State')
            assert not state['keys'] and not state['buttons'] and [29, 1] in state['key_calls'] and [29, 0] in state['key_calls'], state
            fixture('Revoke'); wait_for(lambda: server.poll() is not None)
            assert server.returncode == 0
            server.stdout.close()
            print('Application listener closed after permission revocation; input released on TLS disconnect')
            virtual_config = root / 'virtual-host.json'
            virtual_config.write_text(json.dumps(dict(json.loads(config.read_text()), desktop_source='virtual')))
            for mode, expected in (('no-virtual', 'does not offer virtual'), ('virtual-physical', 'physical screen'),
                                   ('virtual-count', 'exactly one')):
                fixture('Mode', GLib.Variant('(s)', (mode,)))
                before = fixture('State')['opened']
                failed = subprocess.run([host, '--config', str(virtual_config), '--desktop'], env=env, text=True, capture_output=True, timeout=10)
                assert failed.returncode != 0 and expected in failed.stdout + failed.stderr and '"listening"' not in failed.stdout, (mode, failed)
                assert fixture('State')['opened'] == before, 'Unexpected screen capture for a rejected virtual request'
                print('Rejected ' + mode + ' without physical fallback, PipeWire access or listener')
            for mode, expected in (('cancel', 'canceled'), ('denied', 'keyboard and pointer'), ('unsupported', 'portal backend'),
                                   ('cursorless', 'embedded cursor'), ('too-many', 'one or two'), ('bad-size', 'screen size')):
                fixture('Mode', GLib.Variant('(s)', (mode,)))
                failed = subprocess.run([host, '--config', str(config), '--desktop'], env=env, text=True, capture_output=True, timeout=10)
                assert failed.returncode != 0 and expected in failed.stdout + failed.stderr and '"listening"' not in failed.stdout, (mode, failed)
                print('Rejected ' + mode + ' before opening a listener')
            fixture('Mode', GLib.Variant('(s)', ('pending',)))
            before = fixture('State')
            pending = launch([host, '--config', str(config), '--desktop'], 'pending', stdout=subprocess.PIPE)
            wait_for(lambda: fixture('State')['created'] > before['created'])
            time.sleep(0.2); pending.send_signal(signal.SIGTERM); pending.wait(timeout=5)
            assert pending.returncode == 0
            state = fixture('State')
            assert state['request_closed'] > before['request_closed'] and state['session_closed'] > before['session_closed'], state
            assert '"listening"' not in pending.stdout.read(); pending.stdout.close()
            print('Stop during a pending permission dialog canceled the request, closed the grant, and opened no listener')
        except Exception:
            print('Private process statuses:', [(p.args, p.poll()) for p in processes], file=sys.stderr)
            snapshot = subprocess.run(['pw-dump'], env=env, text=True, capture_output=True, timeout=5)
            print('Private PipeWire objects:', snapshot.stdout[-24000:], file=sys.stderr)
            for log in logs:
                log.flush(); log.seek(0); print(log.name + '\n' + log.read(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes): stop(process)
            for log in logs: log.close()
    return 0


if __name__ == '__main__': sys.exit(main())
