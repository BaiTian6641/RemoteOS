#!/usr/bin/env python3
"""Host-only tests; never writes block devices or changes networking."""
import subprocess
import importlib.util
import fcntl
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('remoteos_update_image', ROOT/'scripts/update-image.py')
UPDATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = UPDATE
SPEC.loader.exec_module(UPDATE)

def fixture_image(fill=b'N'):
    root_start, root_size, state_start, state_size = 2048, 64, 2112, 16
    image = bytearray(fill * ((state_start + state_size) * 512))
    image[:512] = bytes(512)
    struct.pack_into('<I', image, 440, UPDATE.DISK_SIGNATURE)
    for index, (boot, start, size) in enumerate(((0x80, root_start, root_size), (0, state_start, state_size))):
        struct.pack_into('<B3sB3sII', image, 446 + 16*index, boot, bytes(3), 0x83, bytes(3), start, size)
    image[510:512] = b'\x55\xaa'
    superblock = state_start*512+1024
    image[superblock+56:superblock+58] = b'\x53\xef'
    image[superblock+120:superblock+136] = b'REMOTEOS_STATE'.ljust(16, b'\0')
    return image

class ImageToolsTest(unittest.TestCase):
    def test_update_preserves_state_and_trailing_bytes(self):
        source, target = fixture_image(b'N'), fixture_image(b'O') + b'trailing untouched bytes'
        prefix, source_state, target_state = UPDATE.compatible_prefix(source[:512], len(source), target[:512], len(target))
        UPDATE.check_state_superblock(lambda offset, size: source[offset:offset+size], source_state)
        UPDATE.check_state_superblock(lambda offset, size: target[offset:offset+size], target_state)
        before = bytes(target[prefix:])
        with tempfile.TemporaryFile() as image, tempfile.TemporaryFile() as disk:
            image.write(source); image.flush()
            disk.write(target); disk.flush()
            UPDATE.copy_prefix(image, disk.fileno(), prefix)
            disk.seek(0); result = disk.read()
        self.assertEqual(result[:prefix], source[:prefix])
        self.assertEqual(result[prefix:], before)

    def test_update_refuses_layout_changes_gpt_and_truncation(self):
        image = fixture_image()
        for offset, value in ((462+4, 0x82), (462+8, 1), (462+12, 32), (462, 0x80), (450, 0xEE)):
            changed = bytearray(image[:512]); changed[offset] = value
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                UPDATE.compatible_prefix(image[:512], len(image), changed, len(image))
        with self.assertRaises(ValueError):
            UPDATE.parse_layout(image[:512], len(image)-1)
        with self.assertRaises(ValueError):
            UPDATE.compatible_prefix(image[:512], len(image)+512, image[:512], len(image)+512)
        broken = bytearray(image[:512]); struct.pack_into('<I', broken, 462+8, 2050)
        with self.assertRaises(ValueError):
            UPDATE.parse_layout(broken, len(image))
        _, state = UPDATE.parse_layout(image[:512], len(image))
        with self.assertRaises(ValueError):
            UPDATE.check_state_superblock(lambda offset, size: bytes(size), state)

    def test_update_refuses_regular_file_without_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp)/'ordinary-file'; disk.write_bytes(b'preserve this')
            result = subprocess.run(['python3', str(ROOT/'scripts/update-image.py'), '--image', str(disk),
                                     '--signature', str(disk), '--trusted-public-key', str(disk),
                                     '--target', str(disk), '--confirm-update', str(disk)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('block device', result.stderr)
            self.assertEqual(disk.read_bytes(), b'preserve this')

    def test_update_snapshot_is_verified_readonly_and_private(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            image, key, pub, signature = (directory/name for name in ('image', 'key.pem', 'public.pem', 'image.sig'))
            original = fixture_image(); image.write_bytes(original)
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', str(key)], check=True, capture_output=True)
            subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout', '-out', str(pub)], check=True, capture_output=True)
            subprocess.run([str(ROOT/'scripts/sign-image.sh'), str(image), str(key), str(signature)], check=True, capture_output=True)
            with UPDATE.verified_snapshot(image, signature, pub, len(original)) as (snapshot, size):
                self.assertEqual(size, len(original))
                self.assertEqual(fcntl.fcntl(snapshot.fileno(), fcntl.F_GETFL) & os.O_ACCMODE, os.O_RDONLY)
                with self.assertRaises(OSError):
                    os.write(snapshot.fileno(), b'X')
                image.write_bytes(b'modified after verification')
                self.assertEqual(snapshot.read(), original)
            image.write_bytes(bytearray(b'X') + original[1:])
            with self.assertRaises(subprocess.CalledProcessError):
                with UPDATE.verified_snapshot(image, signature, pub, len(original)):
                    self.fail('Tampered update image was accepted')

    def test_signature_verification_and_tamper_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            image, key, pub, signature = (p / name for name in ('image', 'key.pem', 'public.pem', 'image.sig'))
            image.write_bytes(b'RemoteOS diagnostic image test\0' * 100)
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', str(key)], check=True, capture_output=True)
            subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout', '-out', str(pub)], check=True, capture_output=True)
            subprocess.run([str(ROOT/'scripts/sign-image.sh'), str(image), str(key), str(signature)], check=True, capture_output=True)
            command = [str(ROOT/'scripts/verify-image.sh'), str(image), str(signature), str(pub)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            image.write_bytes(b'tampered')
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertNotEqual(subprocess.run([str(ROOT/'scripts/sign-image.sh'), str(image), str(key), str(signature)], capture_output=True).returncode, 0)

    def test_installer_refuses_a_regular_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp)/'not-a-disk'
            file.write_bytes(b'keep me')
            result = subprocess.run(['python3', str(ROOT/'scripts/install-image.py'), '--image', str(file), '--signature', str(file), '--trusted-public-key', str(file), '--target', str(file), '--confirm-erase', str(file)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('block device', result.stderr)
            self.assertEqual(file.read_bytes(), b'keep me')

    def test_config_validator_detects_dropped_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'input').write_text('BR2_REQUIRED=y\n')
            (p/'output').write_text('# BR2_REQUIRED is not set\n')
            result = subprocess.run(['python3', str(ROOT/'scripts/check-buildroot-config.py'), str(p/'input'), str(p/'output')], capture_output=True)
            self.assertNotEqual(result.returncode, 0)

if __name__ == '__main__':
    unittest.main()
