#!/usr/bin/env python3
"""Own a separate userspace desktop for the RemoteOS application host.

No root, login manager, physical display, input device or installed service is
used. Xvnc is only a resizable local X server: RemoteOS still sends MPEG-2/RTP.
Its required VNC socket is private, and both VNC TCP and X11 TCP are disabled.
"""
import argparse
import ctypes
import ctypes.util
import configparser
import json
import os
from pathlib import Path
import secrets
import shlex
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback

STOPPING = False
RUNTIME_KEYS = ('LD_LIBRARY_PATH', 'GST_PLUGIN_PATH', 'GST_PLUGIN_PATH_1_0', 'GST_PLUGIN_SYSTEM_PATH_1_0',
                'GST_REGISTRY', 'GST_REGISTRY_1_0', 'GST_REGISTRY_FORK', 'FONTCONFIG_FILE',
                'PIPEWIRE_MODULE_DIR', 'SPA_PLUGIN_DIR', 'WIREPLUMBER_MODULE_DIR', 'WIREPLUMBER_DATA_DIR')


def stop(signum, frame):
    global STOPPING
    STOPPING = True


def resource(name):
    return Path(__file__).resolve().parent / name


def command(name):
    bundled = resource(name)
    result = str(bundled) if bundled.is_file() else shutil.which(name)
    if not result:
        raise RuntimeError('Independent desktop requires ' + name + '; use the complete Linux host bundle')
    return result


def authority(path, number, cookie):
    fields = (b'', str(number).encode(), b'MIT-MAGIC-COOKIE-1', cookie)
    path.write_bytes(struct.pack('!H', 65535) + b''.join(struct.pack('!H', len(value)) + value for value in fields))
    path.chmod(0o600)


def presentation_clock(path):
    """Keep Xvnc's Present clock awake through its private, passive RFB socket.

    Xvnc slows Present to 1 Hz with no authenticated RFB clients. We capture
    through X11, so authenticate locally but never request framebuffer updates
    or send input. This avoids a second capture, codec or video transport.
    """
    client = socket.socket(socket.AF_UNIX)
    client.settimeout(3)
    try:
        client.connect(str(path))

        def receive(size):
            data = b''
            while len(data) < size:
                part = client.recv(size - len(data))
                if not part:
                    raise RuntimeError('Private presentation-clock connection closed')
                data += part
            return data

        if receive(12) != b'RFB 003.008\n':
            raise RuntimeError('Private presentation clock requires RFB 3.8')
        client.sendall(b'RFB 003.008\n')
        if 1 not in receive(receive(1)[0]):
            raise RuntimeError('Private presentation clock has unexpected authentication')
        client.sendall(b'\x01')
        if receive(4) != b'\0\0\0\0':
            raise RuntimeError('Private presentation-clock authentication failed')
        client.sendall(b'\x01')  # Shared session; no input or video requests.
        header = receive(24)
        length = struct.unpack('!I', header[20:24])[0]
        if length > 65536:
            raise RuntimeError('Invalid private presentation-clock server name')
        receive(length)
        # Advertise resize support so VGA hotplug does not cause Xvnc to drop
        # this passive client. These are capabilities, not update requests.
        client.sendall(struct.pack('!BBHiii', 2, 0, 3, 0, -223, -308))
        client.setblocking(False)
        return client
    except Exception:
        client.close()
        raise


def shutdown_group(process):
    # Children may outlive their direct parent (including launched desktop
    # applications). Signal only the process group created for this session.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=2)


class OwnedChildren:
    """Also own desktop apps that deliberately detach from their process group."""
    def __init__(self):
        # Detached apps become children of this launcher, rather than the
        # host's session manager, when their intermediate parent exits.
        if ctypes.CDLL(None).prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
            raise RuntimeError('Cannot supervise detached desktop applications')
        self.processes = {}

    @staticmethod
    def identity(pid):
        try:
            fields = Path('/proc/' + str(pid) + '/stat').read_text().rsplit(')', 1)[1].split()
            return fields[19] if fields[0] != 'Z' else None
        except (OSError, IndexError):
            return None

    def collect(self):
        pending = [os.getpid()]
        seen = set(pending)
        while pending:
            pid = pending.pop()
            try:
                children = Path(f'/proc/{pid}/task/{pid}/children').read_text().split()
            except OSError:
                continue
            for child in map(int, children):
                if child not in seen:
                    seen.add(child); pending.append(child)
                    identity = self.identity(child)
                    if identity: self.processes[child] = identity

    def shutdown(self):
        self.collect()
        for sig in (signal.SIGTERM, signal.SIGKILL):
            for pid, identity in list(self.processes.items()):
                if self.identity(pid) == identity:
                    try: os.kill(pid, sig)
                    except ProcessLookupError: pass
                    except PermissionError:
                        # A setuid helper (e.g. the document portal's mount
                        # helper) can briefly remain a descendant. The normal
                        # user cannot signal it; do not mask the startup error.
                        print('Cannot signal owned privileged helper ' + str(pid), file=sys.stderr)
            if sig == signal.SIGTERM:
                time.sleep(.25)
                self.collect()
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0: break
            except ChildProcessError:
                break


def wait_socket(path, children, component='audio server', timeout=10):
    deadline = time.monotonic() + timeout
    while not path.exists() and time.monotonic() < deadline and not STOPPING:
        exited = [child.returncode for child in children if child.poll() is not None]
        if exited:
            raise RuntimeError('The independent ' + component + ' exited (' + str(exited) + '); see independent-desktop.log')
        time.sleep(0.05)
    if not path.exists():
        raise RuntimeError('The independent ' + component + ' did not start')


def desktop_environment(config, env):
    choice = config.get('independent_environment', 'auto')
    if choice in ('minimal', 'plasma'):
        return choice
    if choice != 'auto':
        raise RuntimeError('Choose the logged-in desktop or the explicit minimal desktop')
    current = (env.get('XDG_CURRENT_DESKTOP', '') + ':' + env.get('DESKTOP_SESSION', '')).lower()
    if 'kde' in current or 'plasma' in current:
        return 'plasma'
    raise RuntimeError('Matching independent desktop currently supports KDE Plasma. Select Minimal desktop explicitly for other environments.')


def plasma_configuration(state, env):
    """Copy appearance/layout once; subsequent remote changes stay private."""
    original = Path(env.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
    destination = state / 'config'
    marker = state / 'plasma-settings-imported'
    if marker.exists():
        return
    names = ('kdeglobals', 'plasmarc', 'plasmashellrc', 'plasma-org.kde.plasma.desktop-appletsrc',
             'kwinrc', 'kcminputrc', 'kglobalshortcutsrc', 'plasma-localerc', 'konsolerc', 'dolphinrc')
    for name in names:
        source = original / name
        target = destination / name
        if source.is_file() and not target.exists():
            shutil.copyfile(source, target)
            target.chmod(0o600)
    # Physical-monitor assignments are not portable to the private compositor.
    # Preserve the first screen's widgets/panels and let Plasma create a fresh
    # desktop for VGA when it is enabled. Do not alter the original file.
    path = destination / 'plasma-org.kde.plasma.desktop-appletsrc'
    if path.exists():
        groups = configparser.RawConfigParser(strict=False, delimiters=('=',))
        groups.optionxform = str
        groups.read(path)
        roots = [s for s in groups.sections() if s.startswith('Containments][') and s.count('][') == 1]
        secondary = []
        for section in roots:
            value = groups[section].get('lastScreen', '0')
            if value not in ('-1', '0'):
                secondary.append(section)
            else:
                groups[section]['lastScreen'] = '0'
        for section in list(groups.sections()):
            if any(section == root or section.startswith(root + '][') for root in secondary):
                groups.remove_section(section)
        with path.open('w') as output:
            groups.write(output, space_around_delimiters=False)
    marker.write_text('KDE appearance and primary-screen layout copied; local configuration unchanged.\n')
    marker.chmod(0o600)


def nested_layout(displays, initialize=False):
    """Arrange only KWin's windows on the launcher-owned private X server."""
    if os.environ.get('REMOTEOS_INDEPENDENT_DESKTOP') != '1':
        raise RuntimeError('Nested layout requires a launcher-owned independent desktop')
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    os.environ['DISPLAY'] = os.environ['REMOTEOS_CAPTURE_DISPLAY']
    os.environ['XAUTHORITY'] = os.environ['REMOTEOS_CAPTURE_AUTHORITY']
    lib = ctypes.CDLL(ctypes.util.find_library('X11'))
    ptr, ulong, uint, integer = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_int
    signatures = {
        'XOpenDisplay': (ptr, [ctypes.c_char_p]), 'XDefaultRootWindow': (ulong, [ptr]),
        'XQueryTree': (integer, [ptr, ulong, ctypes.POINTER(ulong), ctypes.POINTER(ulong), ctypes.POINTER(ctypes.POINTER(ulong)), ctypes.POINTER(uint)]),
        'XFetchName': (integer, [ptr, ulong, ctypes.POINTER(ptr)]),
        'XGetGeometry': (integer, [ptr, ulong, ctypes.POINTER(ulong), ctypes.POINTER(integer), ctypes.POINTER(integer), ctypes.POINTER(uint), ctypes.POINTER(uint), ctypes.POINTER(uint), ctypes.POINTER(uint)]),
        'XMoveResizeWindow': (integer, [ptr, ulong, integer, integer, uint, uint]),
        'XSetInputFocus': (integer, [ptr, ulong, integer, ulong]),
        'XSync': (integer, [ptr, integer]), 'XFree': (integer, [ptr]), 'XCloseDisplay': (integer, [ptr])}
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name); function.restype = restype; function.argtypes = argtypes
    display = lib.XOpenDisplay(None)
    if not display:
        raise RuntimeError('Cannot open the private desktop for layout')
    try:
        if initialize:
            # No other apps have been started on this private X server. The
            # two full-sized top-level windows were created by our KWin child.
            root, parent, children, count = ulong(), ulong(), ctypes.POINTER(ulong)(), uint()
            lib.XQueryTree(display, lib.XDefaultRootWindow(display), ctypes.byref(root), ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count))
            windows = []
            for i in range(count.value):
                x, y, w, h, border, depth = integer(), integer(), uint(), uint(), uint(), uint()
                if lib.XGetGeometry(display, children[i], ctypes.byref(root), ctypes.byref(x), ctypes.byref(y), ctypes.byref(w), ctypes.byref(h), ctypes.byref(border), ctypes.byref(depth)):
                    if (w.value, h.value) == (displays[0]['width'], displays[0]['height']):
                        windows.append(int(children[i]))
            if children: lib.XFree(children)
            if len(windows) != 2:
                raise RuntimeError('KWin did not create its two private output windows')
            # KWin creates X11-0 and then X11-1 on the same XCB connection.
            # Their allocated window IDs retain that order; stacking order can
            # change during the compositor's initial output configuration.
            windows.sort()
            (runtime / 'plasma-windows.json').write_text(json.dumps(windows))
        else:
            windows = json.loads((runtime / 'plasma-windows.json').read_text())
        for i, window in enumerate(windows):
            if i < len(displays):
                d = displays[i]
                lib.XMoveResizeWindow(display, window, int(d.get('x', 0)), int(d.get('y', 0)), int(d['width']), int(d['height']))
            else:
                lib.XMoveResizeWindow(display, window, 16000, 16000, 320, 200)
        lib.XSetInputFocus(display, windows[0], 1, 0)
        lib.XSync(display, False)
    finally:
        lib.XCloseDisplay(display)
    env = {key: value for key, value in os.environ.items() if key not in RUNTIME_KEYS}
    env.pop('KSCREEN_BACKEND', None)
    env.update(XDG_SESSION_TYPE='wayland', WAYLAND_DISPLAY='remoteos-plasma', QT_QPA_PLATFORM='wayland',
               KSCREEN_BACKEND_INPROCESS='1')
    # A resize notification changes each nested output's native mode. Wait for
    # that event before asking KScreen to arrange/enable its logical outputs.
    time.sleep(.12)
    arguments = []
    for i in range(2):
        prefix = 'output.X11-' + str(i) + '.'
        if i < len(displays):
            d = displays[i]
            arguments.extend((prefix + 'enable', prefix + 'position.' + str(d.get('x', 0)) + ',' + str(d.get('y', 0)), prefix + 'scale.1'))
        else:
            arguments.append(prefix + 'disable')
    # A fresh in-process backend avoids stale output IDs from a persistent
    # KScreen service after nested-output mode changes. The command can report
    # success despite a rejected output ID; validate the compositor's result.
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        result = subprocess.run(['kscreen-doctor', *arguments], env=env, capture_output=True, text=True,
                                timeout=max(.05, min(2, deadline - time.monotonic())))
        inventory = subprocess.run(['kscreen-doctor', '-j'], env=env, capture_output=True, text=True,
                                   timeout=max(.05, min(2, deadline - time.monotonic())))
        observed = json.loads(inventory.stdout) if inventory.returncode == 0 else {}
        (runtime / 'plasma-layout-last.json').write_text(json.dumps({'requested': displays, 'arguments': arguments,
            'exit': result.returncode, 'result': result.stdout + result.stderr, 'inventory': observed}, indent=2))
        outputs = {item['name']: item for item in observed.get('outputs', [])}
        correct = result.returncode == 0 and len(outputs) == 2
        for i in range(2):
            item = outputs.get('X11-' + str(i), {})
            if i >= len(displays):
                correct = correct and item.get('enabled') is False
                continue
            expected = displays[i]
            mode = next((mode for mode in item.get('modes', []) if mode['id'] == item.get('currentModeId')), {})
            correct = correct and item.get('enabled') is True and item.get('scale') == 1 and \
                item.get('pos') == {'x': expected.get('x', 0), 'y': expected.get('y', 0)} and \
                mode.get('size') == {'width': expected['width'], 'height': expected['height']}
        if correct:
            return
        time.sleep(.1)
    summary = [{key: item.get(key) for key in ('name', 'id', 'enabled', 'pos', 'scale')} for item in outputs.values()]
    raise RuntimeError('Private Plasma did not accept the native client layout: ' +
                       (result.stdout + result.stderr)[-1500:] + '; observed ' + json.dumps(summary))


def plasma_inside(args):
    """KWin supplies this child its own Wayland/Xwayland environment."""
    if os.environ.get('REMOTEOS_INDEPENDENT_DESKTOP') != '1':
        raise RuntimeError('Plasma startup requires a launcher-owned independent desktop')
    config = json.loads(args.config.read_text())
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    width, height = int(config['virtual_width']), int(config['virtual_height'])
    app_environment = {key: os.environ[key] for key in ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY') if key in os.environ}
    (runtime / 'plasma-app-environment.json').write_text(json.dumps(app_environment))
    env = {key: value for key, value in os.environ.items() if key not in RUNTIME_KEYS}
    env.update(app_environment)
    env.update(QT_QPA_PLATFORM='wayland', GDK_BACKEND='wayland,x11', XDG_SESSION_TYPE='wayland')
    # This bus was started before KWin and still has the hidden outer Xvnc
    # display in its activation environment. Browser wallet/portal helpers must
    # instead open on the visible private desktop, or Chrome can wait forever
    # for an inaccessible wallet prompt. Update only our private session bus;
    # never import into systemd's shared user manager or change the real bus.
    subprocess.run([command('dbus-update-activation-environment'),
        'DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'XDG_CURRENT_DESKTOP',
        'DESKTOP_SESSION', 'XDG_SESSION_TYPE', 'KDE_FULL_SESSION',
        'KDE_SESSION_VERSION', 'QT_QPA_PLATFORM', 'GDK_BACKEND',
        'QT_QUICK_BACKEND'], env=env, check=True, timeout=5)
    nested_layout([{'width': width, 'height': height, 'x': 0, 'y': 0}], initialize=True)
    children = []
    with args.log.open('a') as log:
        try:
            (runtime / 'plasma-desktop-environment.json').write_text(json.dumps(env))
            for name in ('kded6', 'plasmashell'):
                children.append(subprocess.Popen([shutil.which(name)], env=env, stdout=log, stderr=log))
            (runtime / 'plasma-ready').write_text('ready\n')
            while not STOPPING and all(child.poll() is None for child in children):
                time.sleep(.1)
            if not STOPPING:
                raise RuntimeError('The private Plasma desktop exited; see independent-desktop.log')
            return 0
        finally:
            for child in reversed(children):
                if child.poll() is None: child.terminate()


def inside(args):
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    config = json.loads(args.config.read_text())
    children = []
    backend_env = dict(os.environ, **json.loads((runtime / 'backends.json').read_text()))
    control_output = int(backend_env.pop('REMOTEOS_CONTROL_OUTPUT_FD', '1'))
    log_path = args.log if args.log else runtime / 'components.log'
    with log_path.open('a') as log:
        try:
            if config.get('audio_source', 'none') != 'none':
                children.append(subprocess.Popen([command('pipewire')], env=backend_env, stdout=log, stderr=log))
                wait_socket(runtime / 'pipewire-0', children)
                children.append(subprocess.Popen([command('wireplumber')], env=backend_env, stdout=log, stderr=log))
                children.append(subprocess.Popen([command('pipewire-pulse')], env=backend_env, stdout=log, stderr=log))
                wait_socket(runtime / 'pulse/native', children)
            if config['independent_environment'] == 'plasma':
                plasma_env = {key: value for key, value in os.environ.items() if key not in RUNTIME_KEYS}
                plasma_env.update(XDG_CURRENT_DESKTOP='KDE', DESKTOP_SESSION='plasma', XDG_SESSION_TYPE='wayland',
                    KDE_FULL_SESSION='true', KDE_SESSION_VERSION='6', QT_QUICK_BACKEND='software',
                    REMOTEOS_CAPTURE_DISPLAY=os.environ['DISPLAY'], REMOTEOS_CAPTURE_AUTHORITY=os.environ['XAUTHORITY'])
                plasma_child = shlex.join([sys.executable, str(Path(__file__).resolve()), '--plasma-inside',
                    '--config', str(args.config), '--host', str(args.host), '--log', str(log_path)])
                children.append(subprocess.Popen([shutil.which('kwin_wayland'), '--x11-display', os.environ['DISPLAY'],
                    '--width', str(config['virtual_width']), '--height', str(config['virtual_height']), '--output-count', '2',
                    '--socket', 'remoteos-plasma', '--xwayland', '--no-lockscreen', '-platform', 'offscreen',
                    '--exit-with-session', plasma_child], env=plasma_env, stdout=log, stderr=log))
                wait_socket(runtime / 'plasma-ready', children, 'KDE Plasma desktop', 20)
                backend_env.update(REMOTEOS_NESTED_LAYOUT_HELPER=str(Path(__file__).resolve()),
                    REMOTEOS_CAPTURE_DISPLAY=os.environ['DISPLAY'], REMOTEOS_CAPTURE_AUTHORITY=os.environ['XAUTHORITY'],
                    REMOTEOS_NESTED_INPUT_WINDOW=str(json.loads((runtime / 'plasma-windows.json').read_text())[0]))
            else:
                children.append(subprocess.Popen([command('openbox'), '--config-file', str(args.config.parent / 'openbox.xml')], env=backend_env, stdout=log, stderr=log))
                children.append(subprocess.Popen([command('remoteos-desktop-shell')], env=backend_env, stdout=log, stderr=log))
            host = subprocess.Popen([str(args.host), '--config', str(args.config), '--desktop'],
                                    env=backend_env, stdout=control_output)
            children.append(host)
            while not STOPPING and host.poll() is None:
                if any(child.poll() is not None for child in children[:-1]):
                    raise RuntimeError('An independent desktop component exited; see independent-desktop.log')
                time.sleep(0.05)
            if STOPPING:
                host.terminate()
                try:
                    host.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    host.kill()
            return host.wait()
        finally:
            # Outer supervisor owns final process-group cleanup, including apps.
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()


def private_wallet_services(runtime):
    """Activate an optional bundled wallet bridge only on this session bus.

    Runtime service declarations have precedence over installed declarations.
    Create them before dbus-run-session; they disappear with this owned runtime
    directory. The installed Secret Service/encrypted wallet implementation
    remains responsible for storage and normal setup/unlock dialogs.
    """
    bridge = resource('../libexec/remoteos/kwallet-bridge/run-bridge').resolve()
    if not bridge.exists():
        return
    if not bridge.is_file() or not os.access(bridge, os.X_OK):
        raise RuntimeError('The bundled private wallet bridge is not executable')
    executable = str(bridge)
    if '\n' in executable or '\r' in executable:
        raise RuntimeError('The private wallet bridge path must not contain newlines')
    executable = executable.replace('\\', '\\\\').replace('"', '\\"')
    services = runtime / 'dbus-1/services'
    services.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name in ('org.kde.kwalletd6', 'org.kde.kwalletd5'):
        declaration = services / (name + '.service')
        declaration.write_text('[D-BUS Service]\nName=' + name +
                               '\nExec="' + executable + '"\n')
        declaration.chmod(0o600)


def outside(args):
    config = json.loads(args.config.read_text())
    if config.get('desktop_source') != 'independent':
        raise RuntimeError('The independent launcher requires desktop_source=independent')
    owned = OwnedChildren()
    width, height = int(config.get('virtual_width', 1024)), int(config.get('virtual_height', 576))
    if not (320 <= width <= 3840 and 200 <= height <= 2160):
        raise RuntimeError('Invalid independent desktop size')
    environment = desktop_environment(config, os.environ)
    xserver_command = command('Xtigervnc')
    if environment == 'plasma':
        for name in ('kwin_wayland', 'plasmashell', 'kded6', 'kscreen-doctor'):
            if not shutil.which(name):
                raise RuntimeError('Matching KDE Plasma requires the host desktop component ' + name)
        command('dbus-update-activation-environment')
    else:
        command('openbox')
        command('remoteos-desktop-shell')
    command('dbus-run-session')
    if config.get('audio_source', 'none') != 'none':
        for name in ('pipewire', 'wireplumber', 'pipewire-pulse'):
            command(name)
    # Configuration/cache/data persist between sessions, but authentication and
    # sockets exist only in the private temporary runtime directory.
    state = args.config.parent / 'independent-desktop'
    state.mkdir(mode=0o700, exist_ok=True)
    for name in ('config', 'cache', 'data'):
        (state / name).mkdir(mode=0o700, exist_ok=True)
    if environment == 'plasma':
        plasma_configuration(state, os.environ)
        # This is a working remote desktop, not a fresh OS installation.
        # Suppress only the private session's first-run welcome popup; keep
        # normal Plasma services and the original user's settings intact.
        (state / 'config/kded5rc').write_text('[Module-kded_plasma-welcome]\nautoload=false\n')
    pw_config = state / 'config/pipewire/pipewire.conf.d'
    wp_config = state / 'config/wireplumber/wireplumber.conf.d'
    pw_config.mkdir(parents=True, exist_ok=True)
    wp_config.mkdir(parents=True, exist_ok=True)
    (pw_config / '90-remoteos.conf').write_text('''context.properties = { default.clock.rate = 48000 default.clock.quantum = 480 default.clock.min-quantum = 128 default.clock.max-quantum = 1024 }
context.objects = [ { factory = adapter args = { factory.name = support.null-audio-sink node.name = remoteos-output node.description = "RemoteOS independent playback" media.class = Audio/Sink audio.position = [ FL FR ] monitor.channel-volumes = true } } ]
''')
    (wp_config / '90-remoteos.conf').write_text('''wireplumber.profiles = { main = { hardware.audio = disabled hardware.bluetooth = disabled hardware.video-capture = disabled } }
''')
    (state / 'openbox.xml').write_text('''<?xml version="1.0"?>
<openbox_config xmlns="http://openbox.org/3.4/rc"><focus><followMouse>no</followMouse><focusNew>yes</focusNew></focus>
<theme><name>Clearlooks</name><titleLayout>NLIMC</titleLayout></theme><desktops><number>1</number></desktops>
<keyboard><keybind key="A-Tab"><action name="NextWindow"/></keybind><keybind key="A-S-Tab"><action name="PreviousWindow"/></keybind><keybind key="A-F4"><action name="Close"/></keybind><keybind key="A-F10"><action name="ToggleMaximize"/></keybind></keyboard>
<mouse><context name="Titlebar"><mousebind button="Left" action="Drag"><action name="Move"/></mousebind><mousebind button="Left" action="Press"><action name="Focus"/><action name="Raise"/></mousebind><mousebind button="Left" action="DoubleClick"><action name="ToggleMaximize"/></mousebind></context>
<context name="Frame"><mousebind button="A-Left" action="Drag"><action name="Move"/></mousebind><mousebind button="A-Right" action="Drag"><action name="Resize"/></mousebind></context>
<context name="Client"><mousebind button="Left" action="Press"><action name="Focus"/><action name="Raise"/></mousebind></context>
<context name="Maximize"><mousebind button="Left" action="Click"><action name="ToggleMaximize"/></mousebind></context>
<context name="Iconify"><mousebind button="Left" action="Click"><action name="Iconify"/></mousebind></context>
<context name="Close"><mousebind button="Left" action="Click"><action name="Close"/></mousebind></context></mouse></openbox_config>
''')
    env = dict(os.environ)
    for name in ('WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER', 'XDG_SESSION_ID', 'XDG_SEAT', 'XDG_VTNR', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR', 'PULSE_SERVER', 'PULSE_RUNTIME_PATH'):
        env.pop(name, None)
    env.update(XDG_SESSION_TYPE='x11', XDG_CURRENT_DESKTOP='RemoteOS', DESKTOP_SESSION='remoteos-independent',
               XDG_CONFIG_HOME=str(state / 'config'), XDG_CACHE_HOME=str(state / 'cache'), XDG_DATA_HOME=str(state / 'data'),
               GDK_BACKEND='x11', QT_QPA_PLATFORM='xcb', REMOTEOS_INDEPENDENT_DESKTOP='1')
    bundle_share = resource('../share').resolve()
    if environment == 'plasma':
        original_data = os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))
        # Read existing installed user themes/widgets/app declarations through
        # a search path; writes still use the separate XDG_DATA_HOME above.
        env['XDG_DATA_DIRS'] = original_data + ':' + env.get('XDG_DATA_DIRS', '/usr/local/share:/usr/share')
    if bundle_share.is_dir():
        env['XDG_DATA_DIRS'] = str(bundle_share) + ':' + env.get('XDG_DATA_DIRS', '/usr/local/share:/usr/share')
        for name in ('pipewire.conf', 'pipewire-pulse.conf'):
            source = bundle_share / 'pipewire' / name
            if source.is_file(): shutil.copyfile(source, state / 'config/pipewire' / name)
        source = bundle_share / 'wireplumber/wireplumber.conf'
        if source.is_file(): shutil.copyfile(source, state / 'config/wireplumber/wireplumber.conf')
    bundle_lib = resource('../lib').resolve()
    if (bundle_lib / 'pipewire-0.3').is_dir():
        env.update(PIPEWIRE_MODULE_DIR=str(bundle_lib / 'pipewire-0.3'), SPA_PLUGIN_DIR=str(bundle_lib / 'spa-0.2'),
                   WIREPLUMBER_MODULE_DIR=str(bundle_lib / 'wireplumber-0.5'), WIREPLUMBER_DATA_DIR=str(bundle_share / 'wireplumber'))
    # AF_UNIX paths are limited to about 108 bytes. A bundle/state directory
    # can be much longer; keep credential-bearing sockets in a short 0700 temp
    # directory while application configuration stays in persistent state.
    with tempfile.TemporaryDirectory(prefix='remoteos-', dir='/tmp') as tmp:
        runtime = Path(tmp)
        env.update(XDG_RUNTIME_DIR=str(runtime), XAUTHORITY=str(runtime / 'Xauthority'),
                   PULSE_SERVER='unix:' + str(runtime / 'pulse/native'), PIPEWIRE_RUNTIME_DIR=str(runtime))
        if environment == 'plasma':
            private_wallet_services(runtime)
        cookie = secrets.token_bytes(16)
        authority(runtime / 'Xauthority', 0, cookie)
        read_fd, write_fd = os.pipe()
        xserver = session = clock = None
        (state / 'independent-desktop.log').write_text('')
        with (state / 'independent-desktop.log').open('a') as log:
            try:
                xserver = subprocess.Popen([xserver_command, '-displayfd', str(write_fd), '-geometry', f'{width}x{height}',
                    '-depth', '24', '-rfbport', '-1', '-rfbunixpath', str(runtime / 'private-vnc.sock'), '-rfbunixmode', '0600',
                    '-SecurityTypes', 'None', '-FrameRate', '60', '-nolisten', 'tcp', '-auth', str(runtime / 'Xauthority'), '-desktop', 'RemoteOS independent desktop'],
                    env=env, pass_fds=(write_fd,), start_new_session=True, stdout=log, stderr=log)
                os.close(write_fd)
                write_fd = -1
                deadline = time.monotonic() + 10
                while not STOPPING and time.monotonic() < deadline:
                    if select.select([read_fd], [], [], 0.05)[0]:
                        break
                if STOPPING:
                    return 0
                if not select.select([read_fd], [], [], 0)[0]:
                    raise RuntimeError('Independent X server startup timed out')
                number = os.read(read_fd, 32).decode().strip()
                if not number.isdecimal():
                    raise RuntimeError('Independent X server did not start; see independent-desktop.log')
                authority(runtime / 'Xauthority', number, cookie)
                env['DISPLAY'] = ':' + number
                if environment == 'plasma':
                    wait_socket(runtime / 'private-vnc.sock', [xserver])
                    clock = presentation_clock(runtime / 'private-vnc.sock')
                runtime_config = dict(config, audio_target='remoteos-output', independent_environment=environment)
                runtime_path = runtime / 'host.json'
                runtime_path.write_text(json.dumps(runtime_config))
                runtime_path.chmod(0o600)
                (runtime / 'backends.json').write_text(json.dumps({name: env[name] for name in RUNTIME_KEYS if name in env}))
                # Window-manager configuration is generated beside this config.
                shutil.copyfile(state / 'openbox.xml', runtime / 'openbox.xml')
                print(json.dumps({'event': 'independent-desktop', 'display': env['DISPLAY'], 'width': width, 'height': height,
                    'environment': environment, 'same_unix_account': True, 'runtime': str(runtime), 'x11_tcp': False, 'vnc_tcp': False,
                    'presentation_clock_fps': 60 if clock else None}), flush=True)
                app_env = {key: value for key, value in env.items() if key not in RUNTIME_KEYS}
                # D-Bus activated desktop helpers can print on their inherited
                # stdout. Give only the host a separate control-output fd so
                # component messages cannot corrupt its JSON event channel.
                control_fd = os.dup(sys.stdout.fileno())
                app_env['REMOTEOS_CONTROL_OUTPUT_FD'] = str(control_fd)
                try:
                    session = subprocess.Popen([command('dbus-run-session'), '--', sys.executable, str(Path(__file__).resolve()),
                        '--inside', '--config', str(runtime_path), '--host', str(args.host), '--log', str(state / 'independent-desktop.log')],
                        env=app_env, stdout=log, stderr=log, pass_fds=(control_fd,), start_new_session=True)
                finally:
                    os.close(control_fd)
                while not STOPPING and session.poll() is None:
                    owned.collect()
                    if xserver.poll() is not None:
                        raise RuntimeError('Independent X server exited; see independent-desktop.log')
                    if clock:
                        try:
                            if not clock.recv(65536):
                                raise RuntimeError('Private presentation clock disconnected')
                        except BlockingIOError:
                            pass
                    time.sleep(0.05)
                if STOPPING:
                    return 0
                return session.wait()
            finally:
                if clock:
                    clock.close()
                for child in (session, xserver):
                    if child:
                        shutdown_group(child)
                owned.shutdown()
                os.close(read_fd)
                if write_fd >= 0:
                    os.close(write_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--host', type=Path)
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--plasma-inside', action='store_true')
    parser.add_argument('--layout')
    parser.add_argument('--log', type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    if os.geteuid() == 0:
        raise RuntimeError('Run the independent desktop as your normal user')
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, stop)
    if args.layout:
        nested_layout(json.loads(args.layout))
        return 0
    if not args.config or not args.host:
        parser.error('--config and --host are required')
    args.config, args.host = args.config.resolve(), args.host.resolve()
    if args.plasma_inside:
        return plasma_inside(args)
    return inside(args) if args.inside else outside(args)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        print(json.dumps({'event': 'sharing-failed', 'message': str(error)}), flush=True)
        sys.exit(1)
