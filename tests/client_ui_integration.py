#!/usr/bin/env python3
"""Real TLS/MPEG-2 session and GTK pointer/cursor/overlay on two private Xvfb servers.

No physical input devices are opened. The test-only client uses a headless
video sink because Xvfb has no XVideo ports. Production shell/session/input
mapping code is otherwise exercised, including rendered cursor pixels.
"""
import ctypes as c
import json
import os
from pathlib import Path
import secrets
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def pixels(path):
    data = path.read_bytes(); header = struct.unpack('>25I', data[:100])
    start = header[0] + header[19] * 12
    return data[start:start + header[12] * header[5]]


def run_case(width, height, managed=False):
    host, client, cursor_tool = sys.argv[1:]
    xvfb = os.environ.get('REMOTEOS_XVFB') or shutil.which('Xvfb')
    if not xvfb or not shutil.which('xauth'): return 77
    x = c.CDLL('libX11.so.6'); xt = c.CDLL('libXtst.so.6'); xf = c.CDLL('libXfixes.so.3')
    x.XOpenDisplay.argtypes = [c.c_char_p]; x.XOpenDisplay.restype = c.c_void_p
    x.XDefaultRootWindow.argtypes = [c.c_void_p]; x.XDefaultRootWindow.restype = c.c_ulong
    x.XSync.argtypes = [c.c_void_p, c.c_int]; x.XCloseDisplay.argtypes = [c.c_void_p]
    x.XQueryPointer.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_uint)]
    x.XQueryKeymap.argtypes = [c.c_void_p, c.c_void_p]
    xt.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
    xt.XTestFakeButtonEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
    xf.XFixesHideCursor.argtypes = [c.c_void_p, c.c_ulong]; xf.XFixesShowCursor.argtypes = [c.c_void_p, c.c_ulong]

    def read(process, timeout=8):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout): raise AssertionError('Private process did not reply')
        value = json.loads(process.stdout.readline())
        if 'error' in value: raise AssertionError(value)
        return value

    with tempfile.TemporaryDirectory(prefix='remoteos-client-session-ui-') as directory:
        root = Path(directory); processes = []; displays = []
        try:
            # Scan only the application's media plugins. A full development
            # SDK can contain GPU/device plugins with unrelated startup probes;
            # those do not belong in a private software-display test. Keep the
            # registry private too, rather than changing another test's cache.
            plugin_roots = []
            for key in ('GST_PLUGIN_PATH_1_0', 'GST_PLUGIN_PATH'):
                plugin_roots.extend(Path(p) for p in os.environ.get(key, '').split(':') if p)
            if shutil.which('pkg-config'):
                plugin_roots.append(Path(subprocess.check_output(
                    ['pkg-config', '--variable=pluginsdir', 'gstreamer-1.0'], text=True, timeout=5).strip()))
            plugin_roots.append(Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0'))
            plugins = root / 'media-plugins'; plugins.mkdir()
            for name in ('coreelements', 'app', 'udp', 'rtp', 'rtpmanager', 'dtls', 'srtp', 'libav',
                         'videoparsersbad', 'videoconvertscale', 'opus', 'audioconvert', 'audioresample', 'xvimagesink', 'ximagesink'):
                filename = 'libgst' + name + '.so'
                source = next((p / filename for p in plugin_roots if (p / filename).is_file()), None)
                if source is None: raise AssertionError('Missing RemoteOS test plugin: ' + filename)
                (plugins / filename).symlink_to(source.resolve())
            environments = []
            for number in (93, 94):
                folder = root / str(number); folder.mkdir(mode=0o700)
                authority = folder / 'Xauthority'; authority.touch(mode=0o600)
                subprocess.run(['xauth', '-f', str(authority), 'add', f':{number}', 'MIT-MAGIC-COOKIE-1', secrets.token_hex(16)], check=True, timeout=5)
                env = dict(os.environ, DISPLAY=f':{number}', XAUTHORITY=str(authority), XDG_SESSION_TYPE='x11', XDG_RUNTIME_DIR=str(folder), REMOTEOS_PRIVATE_X_TEST='1', XCURSOR_THEME='Adwaita', XCURSOR_SIZE='24')
                env.update(GST_PLUGIN_SYSTEM_PATH_1_0='', GST_PLUGIN_PATH_1_0=str(plugins),
                           GST_REGISTRY=str(folder / 'gst-registry.bin'), GST_REGISTRY_FORK='no')
                log = (folder / 'xvfb.log').open('w')
                server = subprocess.Popen([xvfb, f':{number}', '-screen', '0', f'{width}x{height}x24', '-auth', str(authority), '-nolisten', 'tcp', '-fbdir', str(folder)], env=env, stdout=log, stderr=log)
                processes.append(server); environments.append(env)
                os.environ['XAUTHORITY'] = str(authority)
                deadline = time.monotonic() + 5
                display = None
                while time.monotonic() < deadline and not display:
                    display = x.XOpenDisplay(f':{number}'.encode()); time.sleep(.02)
                if not display: raise AssertionError('Private Xvfb did not open')
                displays.append(display)
                subprocess.check_output([cursor_tool, '--initialize'], env=env, text=True, timeout=5)
            host_env, client_env = environments
            if os.environ.get("REMOTEOS_DESKTOP_UI_TEST"):
                client_env.pop("REMOTEOS_TEST_VIDEO_SINK", None)
            if managed:
                # A private WM checks the normal desktop fullscreen path too;
                # the appliance cases intentionally run without any WM.
                wm_config = root / 'openbox.xml'
                wm_config.write_text('<openbox_config xmlns="http://openbox.org/3.4/rc"><keyboard/><mouse/></openbox_config>')
                wm_log = (root / 'openbox.log').open('w')
                wm = subprocess.Popen([shutil.which('openbox'), '--config-file', str(wm_config)],
                                      env=client_env, stdout=wm_log, stderr=wm_log)
                processes.append(wm)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    check = subprocess.check_output(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'],
                                                    env=client_env, text=True, timeout=2)
                    if 'window id' in check: break
                    if wm.poll() is not None: raise AssertionError('Private WM exited')
                    time.sleep(.05)
                else: raise AssertionError('Private WM did not claim the test display')
            initialized = json.loads(subprocess.check_output([host, '--identity', str(root / 'host'), '--init'], env=host_env, text=True, timeout=10))
            code = Path(initialized['pairing_code_file']).read_text().strip()
            assert len(code) == 6 and code.isdigit(), 'Host did not generate a numeric PIN'
            configuration = root / 'host.json'
            configuration.write_text(json.dumps({'identity_dir':str(root / 'host'), 'pairing_code_file':initialized['pairing_code_file'], 'bind':'127.0.0.1', 'port':0, 'application_mode':True, 'audio_source':'none', 'discovery_enabled':True}))
            host_log = (root / 'host.log').open('w')
            server = subprocess.Popen([host, '--config', str(configuration)], env=host_env, stdout=subprocess.PIPE, stderr=host_log, text=True)
            processes.append(server)
            while True:
                listening = read(server)
                if listening.get('event') == 'listening': break
            state = root / 'client'; state.mkdir(mode=0o700)
            (state / 'profile.json').write_text(json.dumps({'host':'127.0.0.1', 'port':listening['port'], 'fingerprint':initialized['fingerprint'], 'auto_connect':False, 'match_host_resolution':True}))
            (state / 'setup.json').write_text(json.dumps({'version':1,'step':3,'direct':True,'complete':False}))
            client_log = (root / 'client.log').open('w')
            app = subprocess.Popen([client, str(state)], env=client_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=client_log, text=True)
            processes.append(app)

            def command(action, **fields):
                app.stdin.write(json.dumps(dict(action=action, **fields)) + '\n'); app.stdin.flush(); return read(app)

            def wait_state(predicate):
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    value = command('state')
                    if predicate(value): return value
                    time.sleep(.05)
                raise AssertionError('State transition failed: ' + str(value))

            client_display, host_display = displays[1], displays[0]
            client_root = x.XDefaultRootWindow(client_display); host_root = x.XDefaultRootWindow(host_display)
            frame = root / '94' / 'Xvfb_screen0'

            def cursor_pixels():
                xt.XTestFakeMotionEvent(client_display, -1, 500, 380, 0); x.XSync(client_display, False); time.sleep(.05)
                before = pixels(frame)
                xf.XFixesHideCursor(client_display, client_root); x.XSync(client_display, False)
                after = pixels(frame)
                xf.XFixesShowCursor(client_display, client_root); x.XSync(client_display, False)
                return sum(before[i:i+4] != after[i:i+4] for i in range(0, len(before), 4))

            command('discover'); wait_state(lambda value: value['hosts'] == 1)
            visible_before = cursor_pixels(); assert visible_before >= 50, visible_before
            prepared = command('prepare_setup', width=width + 160, height=height + 100)
            assert prepared['windowed'] is False, 'Must exercise appliance fullscreen mode'
            # Allow the oversized local requisition and window move to settle
            # before connecting, reproducing a content-dependent setup layout.
            wait_state(lambda value: value['setup_window_geometry']['width'] >= width + 160)
            command('connect', pin=code)
            viewport = dict(x=0, y=0, width=width, height=height)
            connected = wait_state(lambda value: value['connected'] and
                                   value['window_geometry'] == viewport and value['video_geometry'] == viewport)
            assert not connected['setup_window_mapped'], 'Setup toplevel remains mapped around remote video'
            assert connected['viewer_mapped'] and not connected['viewer_decorated'] and not connected['video_toplevel_is_setup'], 'Remote video needs its own borderless toplevel'
            assert not connected['header_visible'] and not connected['status_visible'], 'Local chrome consumes remote pixels'
            assert cursor_pixels() == 0, 'Local cursor remained visible during the remote session'
            assert connected['saved_hosts'] == 1, 'Paired host was not saved in the host list'
            for xpos, ypos in ((110,120), (111,120), (width-1,height-1)):
                xt.XTestFakeMotionEvent(client_display, -1, xpos, ypos, 0); x.XSync(client_display, False); time.sleep(.06)
                parent = c.c_ulong(); child_window = c.c_ulong(); px = c.c_int(); py = c.c_int(); wx = c.c_int(); wy = c.c_int(); mask = c.c_uint()
                x.XQueryPointer(host_display, host_root, c.byref(parent), c.byref(child_window), c.byref(px), c.byref(py), c.byref(wx), c.byref(wy), c.byref(mask))
                assert (px.value, py.value) == (xpos,ypos), ('Pointer mapping jumped', xpos,ypos,px.value,py.value)
            # Exercise client-edge motion and the host warps used by Blender.
            # XTEST operates only on these two private Xvfb displays.
            command('power'); wait_state(lambda value: value['overlay'])
            command('relative', enabled=True)
            assert not command('state')['mouse_captured'], 'Overlay captured local pointer'
            command('power'); wait_state(lambda value: value['mouse_captured'])
            xt.XTestFakeRelativeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_ulong]
            xt.XTestFakeButtonEvent(client_display, 2, True, 0); x.XSync(client_display, False); time.sleep(.05)
            for _ in range(3):
                xt.XTestFakeMotionEvent(host_display, -1, 200, 200, 0); x.XSync(host_display, False)
                xt.XTestFakeRelativeMotionEvent(client_display, 9, -4, 0); x.XSync(client_display, False); time.sleep(.1)
                x.XQueryPointer(host_display, host_root, c.byref(parent), c.byref(child_window), c.byref(px), c.byref(py), c.byref(wx), c.byref(wy), c.byref(mask))
                assert (px.value, py.value) == (209, 196), ('Relative motion lost, doubled or overridden after host warp', px.value, py.value)
                assert mask.value & 512, 'Middle drag lost its held button'
            command('power'); wait_state(lambda value: value['overlay'])
            assert not command('state')['mouse_captured'], 'Power overlay failed to release pointer capture'
            x.XQueryPointer(host_display, host_root, c.byref(parent), c.byref(child_window), c.byref(px), c.byref(py), c.byref(wx), c.byref(wy), c.byref(mask))
            assert not (mask.value & 512), 'Overlay left middle button held'
            xt.XTestFakeButtonEvent(client_display, 2, False, 0); x.XSync(client_display, False)
            command('relative', enabled=False)
            command('power'); wait_state(lambda value: not value['overlay'])
            assert not command('state')['mouse_captured'], 'Absolute mode retained a pointer grab'
            command('key')
            xt.XTestFakeButtonEvent(client_display,1,True,0); x.XSync(client_display,False); time.sleep(.05)
            command('power'); wait_state(lambda value: value['overlay'])
            assert command('state')['overlay_parent_is_viewer'], 'Overlay belongs to hidden setup instead of active viewer'
            assert command('state')['video_geometry'] == viewport, 'Overlay resized the remote viewport'
            overlay_pixels = cursor_pixels(); assert overlay_pixels >= 50, 'Overlay cursor did not return'
            keymap = c.create_string_buffer(32); x.XQueryKeymap(host_display,keymap)
            assert not (keymap.raw[(29+8)//8] & (1 << ((29+8)%8))), 'Overlay left the host Ctrl key held'
            x.XQueryPointer(host_display,host_root,c.byref(parent),c.byref(child_window),c.byref(px),c.byref(py),c.byref(wx),c.byref(wy),c.byref(mask))
            assert not (mask.value & 256), 'Overlay left the host mouse button held'
            xt.XTestFakeButtonEvent(client_display,1,False,0); x.XSync(client_display,False)
            command('power'); wait_state(lambda value: not value['overlay'] and value['video_geometry'] == viewport)
            assert cursor_pixels() == 0, 'Resume did not hide the local cursor'
            assert not command('state')['setup_window_mapped'], 'Resume remapped setup controls'
            command('reset_setup')
            command('disconnect'); wait_state(lambda value: not value['connected'])
            local = command('state')
            assert local['setup_window_mapped'] and not local['viewer_mapped'], 'Disconnect must restore setup and unmap remote viewer'
            assert local['header_visible'] and local['status_visible'], 'Disconnect did not restore local setup controls'
            disconnected_pixels = cursor_pixels(); assert disconnected_pixels >= 50, 'Disconnect did not restore the local cursor'
            assert len(json.loads((state/'hosts.json').read_text())) == 1
            command('connect')
            again = wait_state(lambda value: value['connected'] and value['window_geometry'] == viewport and value['video_geometry'] == viewport)
            assert not again['setup_window_mapped'] and again['viewer_mapped'], 'Reconnect did not isolate the viewer'
            command('disconnect'); wait_state(lambda value: not value['connected'] and value['setup_window_mapped'] and not value['viewer_mapped'])
            command('quit'); app.wait(timeout=5)
            receipt = {'status':'passed','discovered_and_saved_hosts':1,'pin_digits':6,'cursor_visible_before':visible_before,'cursor_visible_in_overlay':overlay_pixels,'cursor_visible_after_disconnect':disconnected_pixels,'remote_cursor_pixels':0,'pointer':'one-to-one native positions including last pixel','held_keys_and_buttons_released':True,'host_resolution':f'{width}x{height} exact native matching','fullscreen_viewport':viewport,'oversized_setup_requisition':[width+160,height+100],'window_manager':'Openbox' if managed else 'none','physical_input_opened':False,'setup_toplevel_unmapped':True,'viewer_borderless':True,'overlay_parent_is_viewer':True,'reconnect_fullscreen':True}
        except Exception:
            if 'server' in locals() and server.stdout:
                os.set_blocking(server.stdout.fileno(), False)
                try: print('Private host stdout:', os.read(server.stdout.fileno(), 16384).decode(), file=sys.stderr)
                except BlockingIOError: pass
            for name in ('host.log', 'client.log'):
                path = root / name
                if path.exists(): print(path.read_text()[-8000:], file=sys.stderr)
            raise
        finally:
            for display in displays: x.XCloseDisplay(display)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
    return receipt


def main():
    cases = [(1024, 576, False), (1366, 768, False)]
    if shutil.which('openbox') and shutil.which('xprop'):
        cases.append((1366, 768, True))
    results = []
    for case in cases:
        result = run_case(*case)
        if result == 77: return 77
        results.append(result)
    print(json.dumps({'status':'passed', 'cases':results}))
    return 0


if __name__ == '__main__': sys.exit(main())
