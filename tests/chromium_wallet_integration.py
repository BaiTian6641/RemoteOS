#!/usr/bin/env python3
"""Opt-in private KDE/Chromium encrypted-wallet integration proof.

Run inside the validation user/mount/network namespace with loopback enabled and
REMOTEOS_PRIVATE_BROWSER_TEST=1. This is deliberately outside default CTest:
Breeze wallet-wizard coordinates qualify the installed KDE 6 UI, not arbitrary
themes. All profiles/wallets/passwords are temporary; only sanitized evidence is
exported. Chromium sandbox and its default password backend remain enabled.
"""
import argparse
import ctypes
import ctypes.util
import http.server
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import select
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time


def run(argv, env, timeout=5):
    result = subprocess.run(argv, env=env, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(str(argv[:5]) + ': ' + result.stderr.strip())
    return result.stdout


def stop(process):
    if process is None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)


class PrivateInput:
    def __init__(self, display):
        self.x = ctypes.CDLL(ctypes.util.find_library('X11'))
        self.t = ctypes.CDLL(ctypes.util.find_library('Xtst'))
        ptr, uint, ulong, integer = ctypes.c_void_p, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]; self.x.XOpenDisplay.restype = ptr
        self.x.XFlush.argtypes = [ptr]; self.x.XCloseDisplay.argtypes = [ptr]
        self.x.XStringToKeysym.argtypes = [ctypes.c_char_p]; self.x.XStringToKeysym.restype = ulong
        self.x.XKeysymToKeycode.argtypes = [ptr, ulong]; self.x.XKeysymToKeycode.restype = uint
        self.t.XTestFakeMotionEvent.argtypes = [ptr, integer, integer, integer, ulong]
        self.t.XTestFakeButtonEvent.argtypes = [ptr, uint, integer, ulong]
        self.t.XTestFakeKeyEvent.argtypes = [ptr, uint, integer, ulong]
        self.display = self.x.XOpenDisplay(display.encode())
        if not self.display:
            raise AssertionError('Cannot open launcher-owned test display')

    def click(self, x, y):
        self.t.XTestFakeMotionEvent(self.display, 0, x, y, 0)
        self.t.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.t.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x.XFlush(self.display); time.sleep(.3)

    def key(self, name):
        code = self.x.XKeysymToKeycode(self.display, self.x.XStringToKeysym(name.encode()))
        self.t.XTestFakeKeyEvent(self.display, code, 1, 0)
        self.t.XTestFakeKeyEvent(self.display, code, 0, 0)
        self.x.XFlush(self.display); time.sleep(.025)

    def type(self, value):
        for letter in value:
            self.key(letter)

    def close(self):
        self.x.XCloseDisplay(self.display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('launcher', type=Path)
    parser.add_argument('--browser', default=shutil.which('google-chrome-stable'))
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--bridge', type=Path, help='Diagnostic-only private D-Bus override')
    parser.add_argument('--extended', action='store_true', help='Also test browser restart with a locked encrypted wallet')
    parser.add_argument('--cancel-create', action='store_true', help='Only verify cancellation of a new-wallet openAsync request')
    args = parser.parse_args()
    if os.environ.get('REMOTEOS_PRIVATE_BROWSER_TEST') != '1' or os.geteuid() == 0:
        raise SystemExit('Opt-in private namespace and non-root test user are required')
    if not args.browser:
        raise SystemExit('A sandbox-capable Chrome/Chromium installation is required')
    launcher_path = args.launcher.resolve()
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    os.umask(0o077)
    with tempfile.TemporaryDirectory(prefix='remoteos-browser-proof-') as temporary:
        root = Path(temporary)
        env = {key: value for key, value in os.environ.items() if key in ('PATH', 'LD_LIBRARY_PATH', 'XDG_DATA_DIRS', 'LANG', 'LC_ALL', 'GST_PLUGIN_PATH', 'GST_PLUGIN_PATH_1_0', 'GST_PLUGIN_SYSTEM_PATH_1_0', 'GST_REGISTRY', 'GST_REGISTRY_FORK')}
        env.update(HOME=str(root / 'home'), XDG_CONFIG_HOME=str(root / 'original-config'), XDG_DATA_HOME=str(root / 'original-data'), XDG_CACHE_HOME=str(root / 'original-cache'), XDG_STATE_HOME=str(root / 'state'), XDG_CURRENT_DESKTOP='KDE', DESKTOP_SESSION='plasma', LIBGL_ALWAYS_SOFTWARE='1', LANG='C.UTF-8', LC_ALL='C.UTF-8')
        Path(env['HOME']).mkdir()
        bundle = launcher_path.parent.parent
        if launcher_path.parent.name == 'bin' and (bundle / 'lib').is_dir():
            env.update(LD_LIBRARY_PATH=str(bundle / 'lib'), PATH=str(bundle / 'bin') + ':' + env['PATH'], GST_PLUGIN_SYSTEM_PATH_1_0='', GST_PLUGIN_PATH_1_0=str(bundle / 'lib/gstreamer-1.0'), GST_REGISTRY_FORK='no', FONTCONFIG_FILE=str(bundle / 'fonts.conf'))
        config = root / 'host.json'
        config.write_text(json.dumps(dict(desktop_source='independent', independent_environment='plasma', virtual_width=1024, virtual_height=576, audio_source='none')))
        stub = root / 'stub.py'; stub.write_text('#!/usr/bin/python3\nimport time\ntime.sleep(180)\n'); stub.chmod(0o700)
        if args.bridge:
            services = root / 'independent-desktop/data/dbus-1/services'; services.mkdir(parents=True)
            for name in ('org.kde.kwalletd6', 'org.kde.kwalletd5'):
                (services / (name + '.service')).write_text('[D-BUS Service]\nName=' + name + '\nExec=' + shlex.join([str(args.bridge.resolve())]) + '\n')
        events = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *unused):
                pass
            def do_GET(self):
                events.append(self.path)
                self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers()
                self.wfile.write(b'<html><title>RemoteOS loading</title><body>Page loaded<script>fetch("/complete");document.title="REMOTEOS_READY"</script></body></html>' if self.path != '/complete' else b'ok')
        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        desktop = browser = monitor = pointer = None
        report = {'desktop_helper_sha256': hashlib.sha256(launcher_path.read_bytes()).hexdigest(), 'first_start_http': False, 'javascript_callback': False, 'sandbox_enabled': True, 'first_start': False, 'encrypted_item': False, 'locked_restart': None, 'cancellation_completion': None, 'browser_sandbox_disabled': False, 'password_backend_override': False}
        with (root / 'launcher.log').open('w+') as log, (root / 'chrome.log').open('w+') as browser_log, (root / 'signals.log').open('w+') as signal_log:
            try:
                desktop = subprocess.Popen(['/usr/bin/python3', str(launcher_path), '--config', str(config), '--host', str(stub)], env=env, stdout=subprocess.PIPE, stderr=log, text=True, start_new_session=True)
                if not select.select([desktop.stdout], [], [], 25)[0]:
                    raise AssertionError('Private desktop did not start')
                session = json.loads(desktop.stdout.readline()); runtime = Path(session['runtime'])
                deadline = time.monotonic() + 25
                while not (runtime / 'plasma-ready').exists():
                    if desktop.poll() is not None or time.monotonic() > deadline:
                        raise AssertionError('Private Plasma did not become ready')
                    time.sleep(.05)
                benv = json.loads((runtime / 'plasma-desktop-environment.json').read_text())
                outer = dict(benv, DISPLAY=session['display'], XAUTHORITY=str(runtime / 'Xauthority'))
                # XOpenDisplay reads XAUTHORITY from this test process, never
                # the interactive user's display or authorization file.
                os.environ['XAUTHORITY'] = outer['XAUTHORITY']
                pointer = PrivateInput(session['display'])
                monitor = subprocess.Popen(['dbus-monitor', '--session', "type='signal',interface='org.freedesktop.Secret.Prompt',member='Completed'", "type='signal',interface='org.freedesktop.Secret.Service',member='CollectionCreated'", "type='signal',interface='org.kde.KWallet',member='walletAsyncOpened'"], env=benv, stdout=signal_log, stderr=signal_log, start_new_session=True)
                def bus(service, path, method, *arguments):
                    return run(['gdbus', 'call', '--session', '--dest', service, '--object-path', path, '--method', method, *arguments], benv)
                def identify_bridge():
                    report['activated_bridge_binary'] = None
                    pid_text = bus('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'org.kde.kwalletd6')
                    match = re.search(r'uint32 ([0-9]+)', pid_text)
                    if match:
                        report['activated_bridge_binary'] = os.readlink('/proc/' + match[1] + '/exe')
                        activated = Path(report['activated_bridge_binary'])
                        owned_runtime = bundle / 'libexec/remoteos/kwallet-bridge'
                        if owned_runtime.is_dir() and activated != owned_runtime / 'kwalletd6':
                            raise AssertionError('The packaged private bridge was not activated')
                        report['runtime_sha256'] = {'runtime/' + name: hashlib.sha256((activated.parent / name).read_bytes()).hexdigest() for name in ('kwalletd6', 'libKF6WalletBackend.so.6', 'run-bridge')}
                def snapshot(name):
                    if args.evidence_dir:
                        run(['scrot', str(args.evidence_dir / (name + '.png'))], outer)
                def focus_wallet(stage):
                    script = root / (stage + '.js')
                    script.write_text("for (const w of workspace.windowList()) { if (w.caption.indexOf('Wallet') >= 0) { w.minimized=false; workspace.activeWindow=w; } }")
                    script_id = run(['qdbus6', 'org.kde.KWin', '/Scripting', 'org.kde.kwin.Scripting.loadScript', str(script), stage], benv).strip()
                    run(['qdbus6', 'org.kde.KWin', '/Scripting/Script' + script_id, 'org.kde.kwin.Script.run'], benv)
                    time.sleep(.5)
                    snapshot(stage)
                if args.cancel_create:
                    transaction = bus('org.kde.kwalletd6', '/modules/kwalletd6', 'org.kde.KWallet.openAsync', 'remoteos-cancel-fixture', '0', 'RemoteOS private validation', 'false')
                    tid = int(re.search(r'\((-?[0-9]+),?\)', transaction).group(1))
                    identify_bridge()
                    time.sleep(5); focus_wallet('cancel-create-prompt'); pointer.key('Escape')
                    deadline = time.monotonic() + 8; matched = False
                    while time.monotonic() < deadline:
                        observed = (root / 'signals.log').read_text()
                        matched = bool(re.search(r'path=/modules/kwalletd6;[^\n]*member=walletAsyncOpened\n\s+int32 ' + str(tid) + r'\n\s+int32 -1', observed))
                        if matched: break
                        time.sleep(.1)
                    report['new_wallet_cancellation'] = matched
                    report['wallet_file_created'] = any(root.rglob('*.kwl'))
                    print(json.dumps(report, indent=2))
                    if not matched or report['wallet_file_created']:
                        raise AssertionError('New-wallet Cancel did not complete negatively without creating a wallet')
                    print('CHROMIUM_PRIVATE_CREATE_CANCELLATION_PASS')
                    return
                def wait_wallet_closed(stage):
                    # HTTP/JS can finish before KWin presents the closing
                    # dialog animation. Verify that no live wallet window
                    # remains, then give its bounded close animation a frame.
                    for attempt in range(20):
                        token = 'REMOTEOS_WALLET_COUNT_' + stage + '_' + str(attempt)
                        script = root / (token + '.js')
                        script.write_text("print('" + token + "', workspace.windowList().filter(w => w.caption.indexOf('Wallet') >= 0).length);")
                        sid = run(['qdbus6', 'org.kde.KWin', '/Scripting', 'org.kde.kwin.Scripting.loadScript', str(script), token], benv).strip()
                        run(['qdbus6', 'org.kde.KWin', '/Scripting/Script' + sid, 'org.kde.kwin.Script.run'], benv)
                        time.sleep(.1)
                        component = root / 'independent-desktop/independent-desktop.log'
                        match = re.search(token + r' (\d+)', component.read_text())
                        if match and match[1] == '0':
                            time.sleep(.3)
                            report[stage + '_wallet_dialog_closed'] = True
                            return
                    snapshot(stage + '-pending-wallet')
                    raise AssertionError('A live wallet dialog remains after page loading')
                argv = [args.browser, '--user-data-dir=' + str(root / 'chrome-profile'), '--no-first-run', '--no-default-browser-check', 'http://127.0.0.1:' + str(httpd.server_port)]
                def start_browser():
                    return subprocess.Popen(argv, env=benv, stdout=browser_log, stderr=browser_log, start_new_session=True)
                def wait_page():
                    deadline = time.monotonic() + 15
                    while '/complete' not in events and time.monotonic() < deadline:
                        time.sleep(.1)
                    if '/complete' not in events:
                        raise AssertionError('Browser did not fetch the page and execute JavaScript')
                browser = start_browser(); time.sleep(15); focus_wallet('first-wallet-prompt')
                password = secrets.token_hex(16)  # Temporary value is never logged or exported.
                pointer.click(383, 268); pointer.click(620, 459); time.sleep(.5)
                pointer.type(password); pointer.key('Tab'); pointer.type(password); pointer.key('Return')
                wait_page(); report['first_start'] = True; report['first_start_http'] = '/' in events; report['javascript_callback'] = '/complete' in events; wait_wallet_closed('first'); snapshot('first-page-loaded')
                collection = '/org/freedesktop/secrets/collection/kdewallet'
                items = bus('org.freedesktop.secrets', collection, 'org.freedesktop.DBus.Properties.Get', 'org.freedesktop.Secret.Collection', 'Items')
                labels = []
                for item in re.findall(r"'(/org/freedesktop/secrets/collection/[^']+)'", items):
                    labels.append(bus('org.freedesktop.secrets', item, 'org.freedesktop.DBus.Properties.Get', 'org.freedesktop.Secret.Item', 'Label'))
                report['encrypted_item'] = any('Chrome Safe Storage' in value or 'Chromium Safe Storage' in value for value in labels)
                report['encrypted_wallet_file'] = any(root.rglob('*.kwl'))
                report['encrypted_wallet'] = report['encrypted_item'] and report['encrypted_wallet_file']
                if not report['encrypted_item'] or not report['encrypted_wallet_file']:
                    raise AssertionError('Encrypted Safe Storage item or wallet file is missing')
                identify_bridge()
                if args.extended:
                    stop(browser); browser = None
                    time.sleep(.5)
                    current_lock = bus('org.freedesktop.secrets', collection, 'org.freedesktop.DBus.Properties.Get', 'org.freedesktop.Secret.Collection', 'Locked')
                    # KSecretD rejects Lock on an already closed wallet. Chrome
                    # may close its last handle while the collection persists.
                    if 'true' not in current_lock:
                        bus('org.freedesktop.secrets', '/org/freedesktop/secrets', 'org.freedesktop.Secret.Service.Lock', "['" + collection + "']")
                    time.sleep(.5)
                    locked = bus('org.freedesktop.secrets', collection, 'org.freedesktop.DBus.Properties.Get', 'org.freedesktop.Secret.Collection', 'Locked')
                    if 'true' not in locked:
                        raise AssertionError('Fixture did not lock encrypted wallet')
                    events.clear(); browser = start_browser(); time.sleep(3); focus_wallet('locked-wallet-prompt')
                    if '/complete' in events:
                        raise AssertionError('Browser loaded before encrypted-wallet unlock')
                    pointer.type(password); pointer.key('Return'); wait_page(); report['locked_restart'] = True; wait_wallet_closed('restart'); snapshot('restart-page-loaded')
                print(json.dumps(report, indent=2)); print('CHROMIUM_PRIVATE_WALLET_INTEGRATION_PASS')
            finally:
                if pointer: pointer.close()
                stop(browser); stop(monitor); stop(desktop); httpd.shutdown()
                if args.evidence_dir:
                    (args.evidence_dir / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
                    signal_log.flush(); signal_log.seek(0)
                    (args.evidence_dir / 'wallet-signals.log').write_text(signal_log.read())
                    component = root / 'independent-desktop/independent-desktop.log'
                    if component.exists():
                        selected = [line for line in component.read_text().splitlines() if any(word in line for word in ('REMOTEOS_DIAG', 'kf.wallet', 'kwalletd6', 'secretservicecompat', 'unexpected result type', 'g_task_return_error'))]
                        (args.evidence_dir / 'wallet-diagnostics.log').write_text('\n'.join(selected) + '\n')


if __name__ == '__main__':
    main()
