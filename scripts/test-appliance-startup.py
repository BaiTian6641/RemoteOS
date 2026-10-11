#!/usr/bin/env python3
"""Exercise appliance service loops against fake devices and child daemons.

No host interfaces, display servers, devices, or init services are changed.
REMOTEOS_TEST_SHELL may select the built target loader + BusyBox for cross checks.
"""
import json
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'buildroot/board/hp2140/overlay'
SHELL = shlex.split(os.environ.get('REMOTEOS_TEST_SHELL', '/bin/sh'))
SPEC = importlib.util.spec_from_file_location('remoteos_qemu_smoke', ROOT / 'scripts/qemu-smoke.py')
QEMU_SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QEMU_SMOKE)
WIFI_SPEC = importlib.util.spec_from_file_location('remoteos_wifi_packaging', ROOT / 'scripts/check-hp2140-wireless.py')
WIFI_PACKAGING = importlib.util.module_from_spec(WIFI_SPEC)
WIFI_SPEC.loader.exec_module(WIFI_PACKAGING)

FAKE = r'''#!/usr/bin/python3
import atexit, json, os, pathlib, signal, socket, sys, time
root = pathlib.Path(os.environ['STARTUP_FIXTURE'])
name = pathlib.Path(sys.argv[0]).name
with open(root / 'calls.jsonl', 'a') as out:
    out.write(json.dumps({'name': name, 'args': sys.argv[1:], 'pid': os.getpid()}) + '\n')
if name == 'ip':
    sys.exit(1 if sys.argv[3] == 'wlan0' and (root / 'blocked').exists() else 0)
if name == 'iw':
    sys.exit(0)
if name == 'start-stop-daemon':
    sys.exit(0)
if name == 'logread':
    path = root / 'syslog'
    if path.exists():
        sys.stdout.write(path.read_text())
    sys.exit(0)
if name == 'wpa_cli':
    status = root / 'association'
    if status.exists():
        sys.stdout.write('wpa_state=COMPLETED\nbssid=' + status.read_text() + '\nid=0\n')
    sys.exit(0)
if name == 'wpa_supplicant' and (root / 'wpa-fails').exists():
    sys.exit(1)
if name == 'wpa_supplicant':
    # Model the socket's actual lifecycle, not merely a surviving fake process.
    control = root / 'control' / sys.argv[sys.argv.index('-i') + 1]
    control.unlink(missing_ok=True)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    listener.bind(str(control))
    atexit.register(lambda: control.unlink(missing_ok=True))
if name == 'xinit' and (root / 'x-orphan-output').exists():
    child = os.fork()
    if child == 0:
        os.setpgid(0, 0)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        while True:
            time.sleep(1)
    with open(root / 'orphans', 'a') as out:
        out.write(str(child) + '\n')
if name == 'xinit' and not (root / 'x-stable').exists():
    custom = root / 'x-log'
    sys.stdout.write(custom.read_text() if custom.exists() else 'X' * (100 if (root / 'x-short-log').exists() else 100000))
    sys.stdout.flush()
    sys.exit(2)
def stopped(signum, frame):
    (root / ('stopped-' + str(os.getpid()))).touch()
    sys.exit(0)
signal.signal(signal.SIGTERM, stopped)
while True:
    time.sleep(0.05)
'''


@unittest.skipUnless(shutil.which(SHELL[0]), 'test shell unavailable')
class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for name in ('ip', 'iw', 'wpa_supplicant', 'wpa_cli', 'udhcpc', 'ntpd', 'xinit', 'start-stop-daemon', 'logread'):
            script = self.bin / name
            script.write_text(FAKE)
            script.chmod(0o755)
        (self.root / 'net').mkdir()
        (self.root / 'reports').mkdir()
        self.env = {
            **os.environ, 'PATH': f'{self.bin}:{os.environ["PATH"]}',
            'STARTUP_FIXTURE': str(self.root),
            'REMOTEOS_NETWORK_SYSFS': str(self.root / 'net'),
            'REMOTEOS_NETWORK_RUNTIME': str(self.root / 'run'),
            'REMOTEOS_NETWORK_PROFILES': str(self.root / 'profiles'),
            'REMOTEOS_NETWORK_CONTROL': str(self.root / 'control'),
            'REMOTEOS_NETWORK_REPORTS': str(self.root / 'reports'),
            'REMOTEOS_NETWORK_INTERVAL': '0.1',
            'REMOTEOS_GRAPHICAL_RUNTIME': str(self.root / 'run'),
            'REMOTEOS_GRAPHICAL_REPORTS': str(self.root / 'reports'),
            'REMOTEOS_GRAPHICAL_XINIT': str(self.bin / 'xinit'),
            'REMOTEOS_GRAPHICAL_CONSOLE': str(self.root / 'console'),
            'REMOTEOS_GRAPHICAL_RETRY_DELAY': '0.05',
            'REMOTEOS_GRAPHICAL_CMDLINE': str(self.root / 'cmdline'),
        }
        (self.root / 'cmdline').write_text('console=tty1 quiet\n')
        self.processes = []

    def tearDown(self):
        orphans = self.root / 'orphans'
        if orphans.exists():
            for pid in orphans.read_text().splitlines():
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
        self.temp.cleanup()

    def device(self, name, wireless=False):
        device = self.root / 'net' / name
        device.mkdir()
        (device / 'device').mkdir()
        if wireless:
            (device / 'wireless').mkdir()

    def start(self, program):
        process = subprocess.Popen(SHELL + [str(OVERLAY / 'usr/sbin' / program)],
                                   env=self.env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        self.processes.append(process)
        return process

    def calls(self, name):
        path = self.root / 'calls.jsonl'
        if not path.exists():
            return []
        return [item for line in path.read_text().splitlines()
                if (item := json.loads(line))['name'] == name]

    def until(self, predicate):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.02)
        self.fail('service condition did not become true')

    def test_network_init_returns_without_waiting_for_dhcp(self):
        script = (OVERLAY / 'etc/init.d/S41remoteos-network').read_text()
        script = script.replace('/run/remoteos', str(self.root / 'run')).replace(
            '/run/wpa_supplicant', str(self.root / 'control'))
        path = self.root / 'init'
        path.write_text(script)
        result = subprocess.run(SHELL + [str(path), 'start'], env=self.env,
                                capture_output=True, timeout=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls('start-stop-daemon')), 1)
        self.assertFalse(self.calls('ip'))
        self.assertFalse(self.calls('udhcpc'))

    def test_blocked_radio_does_not_stop_ethernet_and_recovers_without_reboot(self):
        self.device('wlan0', wireless=True)
        self.device('eth0')
        (self.root / 'blocked').touch()
        process = self.start('remoteos-network')
        self.until(lambda: len(self.calls('udhcpc')) == 1)
        self.assertIn('eth0', self.calls('udhcpc')[0]['args'])
        self.assertFalse(self.calls('wpa_supplicant'))
        (self.root / 'blocked').unlink()
        self.until(lambda: len(self.calls('wpa_supplicant')) == 1 and len(self.calls('udhcpc')) == 2)
        time.sleep(0.35)
        self.assertEqual(len(self.calls('wpa_supplicant')), 1)
        self.assertEqual(len(self.calls('udhcpc')), 2)
        self.assertIsNone(process.poll())
        self.assertEqual((self.root / 'profiles/wlan0.conf').stat().st_mode & 0o777, 0o600)
        self.assertTrue(self.calls('iw'))
        self.assertTrue(all(call['args'] == ['dev', 'wlan0', 'set', 'power_save', 'off'] for call in self.calls('iw')))

    def test_late_device_and_daemon_failure_do_not_require_reboot(self):
        process = self.start('remoteos-network')
        status = self.root / 'run/network-status.txt'
        self.until(lambda: status.exists() and 'waiting-for-hardware' in status.read_text())
        (self.root / 'wpa-fails').touch()
        self.device('wlan0', wireless=True)
        self.device('eth0')
        self.until(lambda: len(self.calls('wpa_supplicant')) >= 2)
        self.assertEqual(len(self.calls('udhcpc')), 2)
        (self.root / 'wpa-fails').unlink()
        # A retry starts after the service loop finishes its other interfaces;
        # observing a call does not establish a ready supplicant socket yet.
        self.until(lambda: (self.root / 'control/wlan0').is_socket()
                   and 'wlan0 wifi ready' in status.read_text())
        count = len(self.calls('wpa_supplicant'))
        time.sleep(0.3)
        self.assertEqual(len(self.calls('wpa_supplicant')), count)
        self.assertIsNone(process.poll())

    def test_network_lock_prevents_duplicate_workers_and_stops_owned_children(self):
        self.device('eth0')
        process = self.start('remoteos-network')
        self.until(lambda: len(self.calls('udhcpc')) == 1)
        duplicate = self.start('remoteos-network')
        self.assertEqual(duplicate.wait(timeout=2), 0)
        self.assertEqual(len(self.calls('udhcpc')), 1)
        child = self.calls('udhcpc')[0]['pid']
        shutil.rmtree(self.root / 'net/eth0')
        self.until(lambda: (self.root / f'stopped-{child}').exists())
        process.terminate()
        self.assertEqual(process.wait(timeout=3), 0)

    def test_supplicant_failure_report_is_private_bounded_and_once_per_outage(self):
        self.device('wlan0', wireless=True)
        (self.root / 'wpa-fails').touch()
        (self.root / 'syslog').write_text(
            'unrelated daemon private information\n'
            'wpa_supplicant: psk=DO_NOT_COPY\n'
            'wpa_supplicant: password=DO_NOT_COPY\n'
            'wpa_supplicant: ssid=PRIVATE_NETWORK\n'
            + 'wpa_supplicant: diagnostic ' * 2000 + '\n'
            'wpa_supplicant: Failed to initialize driver interface\n')
        self.start('remoteos-network')
        report = self.root / 'reports/latest-wifi-failure.txt'
        self.until(report.exists)
        data = report.read_text()
        self.assertIn('interface=wlan0 code=1', data)
        self.assertIn('Failed to initialize driver interface', data)
        self.assertNotIn('DO_NOT_COPY', data)
        self.assertNotIn('PRIVATE_NETWORK', data)
        self.assertNotIn('unrelated daemon', data)
        self.assertLess(report.stat().st_size, 16512)
        self.assertEqual(report.stat().st_mode & 0o777, 0o600)
        saved = report.stat().st_mtime_ns
        self.until(lambda: len(self.calls('wpa_supplicant')) >= 4)
        self.assertEqual(report.stat().st_mtime_ns, saved)
        (self.root / 'wpa-fails').unlink()
        status = self.root / 'run/network-status.txt'
        self.until(lambda: status.exists() and 'wlan0 wifi ready' in status.read_text())
        marker = self.root / 'run/network/reported-failure-wlan0'
        self.assertFalse(marker.exists())
        self.assertTrue(all('-s' in call['args'] for call in self.calls('wpa_supplicant')))

    def test_ephemeral_network_failure_does_not_write_persistent_report(self):
        self.device('wlan0', wireless=True)
        (self.root / 'run').mkdir()
        (self.root / 'run/ephemeral-state').touch()
        (self.root / 'wpa-fails').touch()
        self.start('remoteos-network')
        self.until(lambda: len(self.calls('wpa_supplicant')) >= 3)
        self.assertFalse((self.root / 'reports/latest-wifi-failure.txt').exists())

    def test_switching_access_points_replaces_old_dhcp_lease(self):
        self.device('wlan0', wireless=True)
        self.start('remoteos-network')
        self.until(lambda: len(self.calls('udhcpc')) == 1)
        (self.root / 'association').write_text('00:11:22:33:44:55')
        self.until(lambda: len(self.calls('udhcpc')) == 2)
        old = self.calls('udhcpc')[-1]['pid']
        (self.root / 'association').write_text('00:11:22:33:44:66')
        self.until(lambda: len(self.calls('udhcpc')) == 3)
        self.assertTrue((self.root / f'stopped-{old}').exists())
        self.assertTrue(all('-R' in item['args'] for item in self.calls('udhcpc')))
        time.sleep(0.25)
        self.assertEqual(len(self.calls('udhcpc')), 3)

    def test_graphics_retry_is_bounded_and_log_cannot_fill_ram(self):
        process = self.start('remoteos-graphical')
        self.assertEqual(process.wait(timeout=4), 1)
        self.assertEqual(len(self.calls('xinit')), 3)
        self.assertEqual((self.root / 'run/graphics/latest.log').stat().st_size, 65536)
        self.assertIn('failed code=2 attempts=3', (self.root / 'run/graphical-status.txt').read_text())
        report = self.root / 'reports/latest-graphical-failure.txt'
        self.assertLess(report.stat().st_size, 65664)
        self.assertEqual(report.stat().st_mode & 0o777, 0o600)
        for call in self.calls('xinit'):
            args = call['args']
            self.assertEqual(args[args.index('-nolisten') + 1], 'tcp')
            self.assertIn('-auth', args)
        console = (self.root / 'console').read_text()
        self.assertIn('recovery instructions', console)
        self.assertLess(len(console.encode()), 8704)
        self.assertIn('Recent graphical error context', console)

    def test_graphics_failure_console_keeps_recent_error_context(self):
        log = 'EARLY_PRIVATE_DIAGNOSTICS\n' + ('earlier startup detail ' * 5 + '\n') * 300
        log += '(EE) Failed to load /usr/lib/xorg/modules/drivers/intel_drv.so: undefined symbol: vbeFree\n'
        (self.root / 'x-log').write_text(log)
        process = self.start('remoteos-graphical')
        self.assertEqual(process.wait(timeout=4), 1)
        console = (self.root / 'console').read_text()
        self.assertIn('undefined symbol: vbeFree', console)
        self.assertNotIn('EARLY_PRIVATE_DIAGNOSTICS', console)
        self.assertLess(len(console.encode()), 8704)
        self.assertIn('EARLY_PRIVATE_DIAGNOSTICS',
                      (self.root / 'reports/latest-graphical-failure.txt').read_text())

    def test_diagnostic_boot_prints_complete_bounded_capture(self):
        log = 'EARLY_DRIVER_LOAD\n' + 'diagnostic output\n' * 5000
        (self.root / 'x-log').write_text(log)
        (self.root / 'cmdline').write_text('console=ttyS0,115200 remoteos.diagnostic=1 loglevel=7\n')
        process = self.start('remoteos-graphical')
        self.assertEqual(process.wait(timeout=4), 1)
        captured = (self.root / 'run/graphics/latest.log').read_text()
        console = (self.root / 'console').read_text()
        self.assertEqual(len(captured.encode()), 65536)
        self.assertIn(captured, console)
        self.assertLess(len(console.encode()), 66560)
        self.assertIn('diagnostic mode', console)
        self.assertEqual((self.root / 'reports/latest-graphical-failure.txt').stat().st_mode & 0o777, 0o600)

    def test_diagnostic_boot_token_must_match_exactly(self):
        (self.root / 'cmdline').write_text('remoteos.diagnostic=10 xremoteos.diagnostic=1 remoteos.diagnostic=1suffix\n')
        process = self.start('remoteos-graphical')
        self.assertEqual(process.wait(timeout=4), 1)
        console = (self.root / 'console').read_text()
        self.assertNotIn('diagnostic mode', console)
        self.assertLess(len(console.encode()), 8704)

    def test_graphics_stop_terminates_child_without_restarting(self):
        (self.root / 'x-stable').touch()
        process = self.start('remoteos-graphical')
        self.until(lambda: len(self.calls('xinit')) == 1)
        child = self.calls('xinit')[0]['pid']
        process.terminate()
        self.assertEqual(process.wait(timeout=3), 0)
        self.assertTrue((self.root / f'stopped-{child}').exists())
        self.assertEqual(len(self.calls('xinit')), 1)
        self.assertEqual((self.root / 'run/graphical-status.txt').read_text(), 'stopped\n')

    def test_graphics_orphan_cannot_hold_log_pipe_and_block_retries(self):
        (self.root / 'x-orphan-output').touch()
        process = self.start('remoteos-graphical')
        self.assertEqual(process.wait(timeout=4), 1)
        self.assertEqual(len(self.calls('xinit')), 3)
        for pid in (self.root / 'orphans').read_text().splitlines():
            status = Path('/proc') / pid / 'status'
            self.assertTrue(not status.exists() or '\nState:\tZ ' in status.read_text())
        self.assert_no_live_fixture_descendants()

    def assert_no_live_fixture_descendants(self):
        # Includes the reader's head/cat child, not only the xinit mock.
        survivors = []
        for entry in Path('/proc').glob('[0-9]*/cmdline'):
            try:
                matches = str(self.root).encode() in entry.read_bytes()
                for descriptor in ('0', '1'):
                    try:
                        matches |= str(self.root) in os.readlink(entry.parent / 'fd' / descriptor)
                    except (FileNotFoundError, PermissionError, ProcessLookupError, OSError):
                        pass
                if matches:
                    survivors.append(entry.parent.name)
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                pass
        self.assertEqual(survivors, [])

    def test_graphics_orphan_before_log_limit_cleans_head_child(self):
        (self.root / 'x-short-log').touch()
        self.test_graphics_orphan_cannot_hold_log_pipe_and_block_retries()

    def test_graphics_shutdown_is_bounded_with_stubborn_output_descendant(self):
        (self.root / 'x-orphan-output').touch()
        (self.root / 'x-stable').touch()
        process = self.start('remoteos-graphical')
        self.until(lambda: (self.root / 'orphans').exists())
        process.terminate()
        self.assertEqual(process.wait(timeout=5), 0)
        self.assert_no_live_fixture_descendants()


class IntelDriverEvidenceTests(unittest.TestCase):
    def test_module_vendor_is_success_even_without_supported_qemu_adapter(self):
        result = QEMU_SMOKE.intel_driver_evidence(
            '[ 8.1] (II) LoadModule: "intel"\n'
            '[ 8.2] (II) Module intel: vendor="X.Org Foundation"\n'
            '[ 8.3] (EE) No devices detected.\n[ 8.4] (EE) no screens found(EE)\n')
        self.assertTrue(result['intel_ddx_load_success'])
        self.assertTrue(result['intel_ddx_load_evidence'])
        self.assertFalse(result['intel_ddx_load_errors'])

    def test_load_attempt_or_missing_output_is_not_success(self):
        for log in ('', '[ 8.1] (II) LoadModule: "intel"\n',
                    '[ 8.1] (II) Loading /usr/lib/xorg/modules/drivers/intel_drv.so\n'):
            self.assertFalse(QEMU_SMOKE.intel_driver_evidence(log)['intel_ddx_load_success'])

    def test_link_failure_is_rejected_even_after_vendor_line(self):
        for error in ('Failed to load module "intel" (loader failed, 0)',
                      'Failed to load /usr/lib/xorg/modules/drivers/intel_drv.so',
                      'undefined symbol: vbeFree'):
            result = QEMU_SMOKE.intel_driver_evidence(
                '[ 8.2] (II) Module intel: vendor="X.Org Foundation"\n(EE) ' + error)
            self.assertFalse(result['intel_ddx_load_success'])
            self.assertTrue(result['intel_ddx_load_errors'])


class SupplicantFeatureTests(unittest.TestCase):
    def validate(self, contents):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory)
            binary = target / 'usr/sbin/wpa_supplicant'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(contents)
            return WIFI_PACKAGING.validate_supplicant(target)

    def test_binary_without_syslog_feature_reproduces_release_failure(self):
        with self.assertRaisesRegex(ValueError, 'lacks -s support'):
            self.validate(b'ctrl_iface bind(PF_UNIX) failed')

    def test_binary_without_unix_control_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'lacks the Unix control interface'):
            self.validate(b'  -s = log output to syslog instead of stdout\n')

    def test_compiled_features_are_accepted(self):
        result = self.validate(b'  -s = log output to syslog instead of stdout\n\0'
                               b'ctrl_iface bind(PF_UNIX) failed: %s\0')
        self.assertTrue(result['syslog_option'])
        self.assertTrue(result['unix_control_interface'])
        self.assertEqual(len(result['sha256']), 64)


if __name__ == '__main__':
    unittest.main()
