#!/usr/bin/env python3
"""Ensure qualification logs never attribute stale/malformed media statistics."""
from pathlib import Path
import csv
import json
import os
import shlex
import subprocess
import tempfile
import unittest

FILTER = Path(__file__).resolve().parents[1] / 'buildroot/board/hp2140/overlay/usr/share/remoteos/sample-metrics.awk'
HEADER = 'uptime_seconds,connected,internal_codec,internal_width,internal_height,internal_target_fps,internal_fps,vga_codec,vga_width,vga_height,vga_target_fps,vga_fps,control_rtt_ms,rtp_loss_fraction,queue_ms,usb_exported'
ROW = '100,1,raw-i420,1366,768,60,57.2,mpeg2,1920,1080,30,28.8,12.3,0.01,40,1'
STAGE_HEADER = 'internal_bitrate_bps,internal_decoded_fps,internal_decode_ms,internal_receiver_timeline_delay_ms,internal_presentation_drop_fraction,internal_recovery_drop_fraction,internal_receive_mbps,internal_compressed_queue_drops,vga_bitrate_bps,vga_decoded_fps,vga_decode_ms,vga_receiver_timeline_delay_ms,vga_presentation_drop_fraction,vga_recovery_drop_fraction,vga_receive_mbps,vga_compressed_queue_drops'
STAGE_ROW = '12000000,58.5,11.2,35.3,0.05,0.15,11.8,3,9000000,29.2,23.4,41.8,0.02,0.04,8.5,7'
MISSING = '0' + ',' * 16
OVERLAY = FILTER.parents[3]
SHELL = shlex.split(os.environ.get('REMOTEOS_TEST_SHELL', '/bin/sh'))


class CpuCounterTests(unittest.TestCase):
    def parse(self, value, previous=''):
        result = subprocess.run(['awk', '-v', f'previous={previous}', '-f',
                                 str(FILTER.with_name('sample-cpu.awk'))],
                                input=value, text=True, capture_output=True, check=True)
        fields = result.stdout.split()
        return [float(value) for value in fields[:4]], ' '.join(fields[4:])

    def test_smt_occupancy_and_guest_accounting(self):
        initial = 'cpu 200 0 0 200 0 0 0 0 100 0\ncpu0 100 0 0 100 0 0 0 0 50 0\ncpu1 100 0 0 100 0 0 0 0 50 0\n'
        first, previous = self.parse(initial)
        self.assertEqual(first, [0, 0, 0, 2])
        sample, _ = self.parse('cpu 300 0 0 300 0 0 0 0 150 0\ncpu0 200 0 0 100 0 0 0 0 100 0\ncpu1 100 0 0 200 0 0 0 0 50 0\n', previous)
        self.assertEqual(sample, [1, 50, 100, 2])

    def test_migrating_worker_is_not_claimed_to_be_thread_utilization(self):
        _, previous = self.parse('cpu 200 0 0 200\ncpu0 100 0 0 100\ncpu1 100 0 0 100\n')
        sample, _ = self.parse('cpu 300 0 0 300\ncpu0 150 0 0 150\ncpu1 150 0 0 150\n', previous)
        self.assertEqual(sample, [1, 50, 50, 2])

    def test_reset_does_not_emit_negative_utilization(self):
        sample, _ = self.parse('cpu 1 0 0 1\ncpu0 1 0 0 1\n', 'cpu:500:200 cpu0:500:200')
        self.assertEqual(sample, [0, 0, 0, 1])


class KernelCounterTests(unittest.TestCase):
    def test_named_udp_fields_hex_softnet_and_frequency(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {
                'proc/net/snmp': 'Ip: Forwarding DefaultTTL\nIp: 1 64\nUdp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors\nUdp: 1000 0 37 800 31 4 6\n',
                'proc/net/softnet_stat': '000000aa 0000000f 00000002 00000000\n000000bb 00000001 0000000a 00000000\n',
                'sys/class/net/wlan0/statistics/rx_dropped': '9\n',
                'sys/class/net/wlan0/statistics/rx_errors': '7\n',
                'sys/class/net/eth0/statistics/rx_dropped': '3\n',
                'sys/class/net/eth0/statistics/rx_errors': '2\n',
                'sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq': '1600000\n',
                'sys/devices/system/cpu/cpufreq/policy1/scaling_cur_freq': '800000\n',
            }
            for name, value in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            result = subprocess.run(['awk', '-f', str(FILTER.with_name('sample-kernel.awk')),
                                     *[str(root / name) for name in files]],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(result.stdout, '1000,37,31,4,12,9,16,12,800000,1600000')

    def test_missing_is_unknown_not_zero(self):
        result = subprocess.run(['awk', '-f', str(FILTER.with_name('sample-kernel.awk')), '/dev/null'],
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, ',' * 9)


class PerformanceReportTests(unittest.TestCase):
    def test_topology_station_and_overflow_without_network_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative, value in {
                'proc/cpuinfo': 'processor : 0\nphysical id : 0\ncore id : 0\nmodel name : Intel Atom N270\n\nprocessor : 1\nphysical id : 0\ncore id : 0\n',
                'proc/uptime': '100.0 120.0\n',
                'proc/net/snmp': 'Udp: InDatagrams NoPorts InErrors RcvbufErrors\nUdp: 100 0 6 5\n',
                'proc/net/softnet_stat': '000000aa 0000000f 00000002\n',
                'proc/sys/net/core/rmem_default': '212992\n',
                'proc/sys/net/core/rmem_max': '4194304\n',
                'sys/devices/system/cpu/cpufreq/policy0/scaling_governor': 'schedutil\n',
                'sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq': '1600000\n',
                'sys/class/net/wlan0/device/marker': '',
                'sys/class/net/wlan0/statistics/rx_dropped': '3\n',
                'sys/class/net/tailscale0/statistics/rx_dropped': '999\n',
            }.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            (root / 'sys/class/net/wlan0/wireless').mkdir()
            (root / 'bin').mkdir()
            fake_iw = root / 'bin/iw'
            fake_iw.write_text('''#!/usr/bin/python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['REMOTEOS_DIAGNOSTIC_ROOT'])
with (root / 'iw-calls').open('a') as out:
    out.write(json.dumps(sys.argv[1:]) + '\\n')
if sys.argv[3:] == ['link']:
    print('Connected to secret-MAC\\nSSID: private-network\\nfreq: 5180\\nsignal: -48 dBm\\ntx bitrate: 54.0 MBit/s')
elif sys.argv[3:] == ['station', 'dump']:
    print('Station secret-MAC\\nrx bytes: 1234\\ntx retries: 17\\ntx failed: 2\\nrx bitrate: 36.0 MBit/s\\nsignal avg: -51 dBm')
elif sys.argv[3:] == ['get', 'power_save']:
    print('Power save: off')
else:
    sys.exit('unexpected mutation request')
''')
            fake_iw.chmod(0o755)
            report = root / 'report.txt'
            command = [*SHELL, str(OVERLAY / 'usr/bin/remoteos-performance-report'), str(report)]
            subprocess.run(command, env={**os.environ, 'REMOTEOS_DIAGNOSTIC_ROOT': str(root),
                                         'PATH': f'{root / "bin"}:{os.environ["PATH"]}'},
                           text=True, capture_output=True, check=True)
            data = report.read_text()
            self.assertIn('logical_cpus=2\nphysical_cores=1', data)
            self.assertIn('scaling_cur_freq=1600000', data)
            self.assertIn('tx retries: 17', data)
            self.assertIn('rx bitrate: 36.0 MBit/s', data)
            self.assertIn('Power save: off', data)
            self.assertIn('Udp: 100 0 6 5', data)
            self.assertIn('rmem_default=212992', data)
            self.assertNotIn('secret-MAC', data)
            self.assertNotIn('private-network', data)
            self.assertNotIn('tailscale0', data)
            self.assertEqual(report.stat().st_mode & 0o777, 0o600)
            self.assertLessEqual(report.stat().st_size, 65536)
            calls = [json.loads(line) for line in (root / 'iw-calls').read_text().splitlines()]
            self.assertEqual(calls, [['dev', 'wlan0', 'link'], ['dev', 'wlan0', 'station', 'dump'],
                                     ['dev', 'wlan0', 'get', 'power_save']])

    def test_sampler_keeps_application_fields_and_records_kernel_counters(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative, value in {
                'proc/stat': 'cpu 200 0 0 200\ncpu0 100 0 0 100\ncpu1 100 0 0 100\n',
                'proc/uptime': '101.0 120.0\n',
                'proc/meminfo': 'MemAvailable: 123456 kB\n',
                'proc/net/snmp': 'Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors\nUdp: 100 0 6 90 5 2\n',
                'proc/net/softnet_stat': '000000aa 0000000f 00000002\n',
                'proc/net/dev': 'Inter-| Receive | Transmit\n face | bytes | bytes\nwlan0: 700 0 0 0 0 0 0 0 800 0 0 0 0 0 0 0\ntailscale0: 999 0 0 0 0 0 0 0 999 0 0 0 0 0 0 0\n',
                'sys/class/net/wlan0/device/marker': '',
                'sys/class/net/wlan0/statistics/rx_dropped': '3\n',
                'sys/class/net/wlan0/statistics/rx_errors': '1\n',
                'sys/class/thermal/thermal_zone0/temp': '57000\n',
                'metrics.csv': HEADER + ',' + STAGE_HEADER + '\n' + ROW + ',' + STAGE_ROW + '\n',
            }.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            (root / 'bin').mkdir()
            # Only the fixture changes counters. No real timers or devices.
            sleep = root / 'bin/sleep'
            sleep.write_text('''#!/usr/bin/python3
import os, pathlib
root = pathlib.Path(os.environ['REMOTEOS_DIAGNOSTIC_ROOT'])
(root / 'proc/stat').write_text('cpu 300 0 0 300\\ncpu0 200 0 0 100\\ncpu1 100 0 0 200\\n')
''')
            sleep.chmod(0o755)
            report = root / 'bin/remoteos-performance-report'
            report.write_text('''#!/bin/sh
printf '%s\\n' "$1" >> "$REMOTEOS_DIAGNOSTIC_ROOT/report-calls"
''')
            report.chmod(0o755)
            output = root / 'sample.csv'
            env = {**os.environ, 'REMOTEOS_DIAGNOSTIC_ROOT': str(root),
                   'PATH': f'{root / "bin"}:{os.environ["PATH"]}',
                   'REMOTEOS_METRICS_FILE': str(root / 'metrics.csv')}
            for key, filename in [('METRICS', 'sample-metrics.awk'), ('NETWORK', 'sample-network.awk'),
                                  ('CPU', 'sample-cpu.awk'), ('KERNEL', 'sample-kernel.awk')]:
                env[f'REMOTEOS_{key}_FILTER'] = str(FILTER.with_name(filename))
            subprocess.run([*SHELL, str(OVERLAY / 'usr/bin/remoteos-sample'), '1', str(output)],
                           env=env, capture_output=True, text=True, check=True, timeout=10)
            with output.open() as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(len(rows), 1)
            row = rows[0]
            self.assertEqual(len(row), 52)
            self.assertNotIn(None, row)
            self.assertEqual(float(row['cpu_busy_percent']), 50)
            self.assertEqual(float(row['busiest_logical_cpu_percent']), 100)
            self.assertEqual(row['logical_cpu_count'], '2')
            self.assertEqual(row['physical_rx_bytes'], '700')
            self.assertEqual(row['internal_codec'], 'raw-i420')
            self.assertEqual(row['internal_fps'], '57.2')
            self.assertEqual(row['metrics_fresh'], '1')
            self.assertEqual(row['udp_receive_buffer_errors'], '5')
            self.assertEqual(row['softnet_dropped'], '15')
            self.assertEqual(row['cpu_min_current_khz'], '')
            self.assertEqual(row['media_details_fresh'], '1')
            self.assertEqual(row['internal_bitrate_bps'], '12000000')
            self.assertEqual(row['internal_presentation_drop_fraction'], '0.05')
            self.assertEqual(row['internal_recovery_drop_fraction'], '0.15')
            self.assertEqual(row['vga_receive_mbps'], '8.5')
            self.assertEqual(row['vga_compressed_queue_drops'], '7')
            self.assertEqual((root / 'report-calls').read_text().splitlines(),
                             [str(output) + '.start.txt', str(output) + '.end.txt'])


class NetworkCounterTests(unittest.TestCase):
    def test_hardware_only_without_tunnel_double_counting(self):
        data = 'Inter-| Receive | Transmit\n face | bytes | bytes\n'
        for name, rx, tx in [('lo', 999, 999), ('tailscale0', 500, 600), ('wlan0', 700, 800), ('enp2s0', 100, 200)]:
            data += f'{name:>6}: {rx} 1 0 0 0 0 0 0 {tx} 1 0 0 0 0 0 0\n'
        result = subprocess.run(['awk', '-v', 'physical=wlan0 enp2s0 ', '-f', str(FILTER.with_name('sample-network.awk'))], input=data, text=True, capture_output=True, check=True)
        self.assertEqual(result.stdout, '800,1000')


class MetricsTests(unittest.TestCase):
    def parse(self, value, now=101, section='basic'):
        return subprocess.run(['awk', '-v', f'now={now}', '-v', f'section={section}', '-f', str(FILTER)], input=value, text=True, capture_output=True, check=True).stdout.strip('\n')

    def test_fresh_and_boundary(self):
        for now in (100, 101, 105):
            self.assertEqual(self.parse(HEADER + '\n' + ROW + '\n', now), '1,' + ROW)

    def test_missing_stale_and_future(self):
        self.assertEqual(self.parse(''), MISSING)
        self.assertEqual(self.parse(HEADER + '\n' + ROW + '\n', 106), MISSING)
        self.assertEqual(self.parse(HEADER + '\n' + ROW + '\n', 99), MISSING)

    def test_format_and_values(self):
        for data in (HEADER + '\n' + ROW + '\n' + ROW, HEADER.replace('uptime', 'time') + '\n' + ROW,
                     HEADER + '\n' + ROW.replace('57.2', 'nan'), HEADER + '\n' + ROW.replace('raw-i420', '=FORMULA()'),
                     HEADER + '\n' + ROW.replace('0.01', '1.5'), HEADER + '\n' + ROW + ',1'):
            self.assertEqual(self.parse(data), MISSING)

    def test_oversize(self):
        self.assertEqual(self.parse(HEADER + '\n' + ROW.replace('12.3', '0' * 4096)), MISSING)

    def test_extended_metrics_and_legacy_details_are_distinguishable(self):
        value = HEADER + ',' + STAGE_HEADER + '\n' + ROW + ',' + STAGE_ROW + '\n'
        self.assertEqual(self.parse(value), '1,' + ROW)
        self.assertEqual(self.parse(value, section='stages'), '1,' + STAGE_ROW)
        self.assertEqual(self.parse(HEADER + '\n' + ROW, section='stages'), MISSING)
        self.assertEqual(self.parse(value, now=106, section='stages'), MISSING)
        self.assertEqual(self.parse(value, now=99, section='stages'), MISSING)

    def test_malformed_stage_fields_reject_whole_snapshot(self):
        header = HEADER + ',' + STAGE_HEADER + '\n'
        for details in (STAGE_ROW.replace('0.05', '1.1'), STAGE_ROW.replace('0.15', '-0.1'),
                        STAGE_ROW.replace('11.2', 'nan'), STAGE_ROW.replace('8.5', '=FORMULA()'),
                        STAGE_ROW + ',1', ','.join(STAGE_ROW.split(',')[:-1])):
            for section in ('basic', 'stages'):
                self.assertEqual(self.parse(header + ROW + ',' + details, section=section), MISSING)


if __name__ == '__main__':
    unittest.main()
