#!/usr/bin/env python3
"""Resolve state partitions from root identity; never open or mount real disks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'buildroot/board/hp2140/overlay/usr/sbin/remoteos-state-device'


class StateDeviceTests(unittest.TestCase):
    def resolve(self, root_disk='sda', root_partition='1', state=True, root_id='8:1'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sysblock = root / 'sys/dev/block'
            sysblock.mkdir(parents=True)
            # A second signed-image clone exists but must never supply state.
            for disk in (root_disk, 'sdb'):
                for number in ('1', '2') if state or disk == 'sdb' else ('1',):
                    part = root / 'sys/devices/block' / disk / f'{disk}{number}'
                    part.mkdir(parents=True)
                    (part / 'partition').write_text(number + '\n')
            actual = root / 'sys/devices/block' / root_disk / f'{root_disk}{root_partition}'
            (sysblock / root_id).symlink_to(actual)
            mountinfo = root / 'mountinfo'
            mountinfo.write_text(f'20 1 {root_id} / / ro - squashfs /dev/root ro\n')
            return subprocess.run([str(SCRIPT), str(mountinfo), str(sysblock), '/dev'], text=True, capture_output=True)

    def test_selects_root_disk_not_other_clone(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '/dev/sda2\n')

    def test_does_not_fall_back_to_another_disk(self):
        result = self.resolve(state=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')

    def test_rejects_unexpected_root_partition(self):
        self.assertNotEqual(self.resolve(root_partition='2').returncode, 0)

    def test_handles_nvme_partition_names(self):
        # Fixture helper appends the partition number; nvme's p is part of base.
        result = self.resolve(root_disk='nvme0n1p', root_id='259:1')
        self.assertEqual(result.stdout, '/dev/nvme0n1p2\n')


if __name__ == '__main__':
    unittest.main()
