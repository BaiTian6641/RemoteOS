#!/usr/bin/env python3
"""Offline whole-disk installer. Never invoke automatically from a running client."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import sys


def walk(node):
    yield node
    for child in node.get('children', []):
        yield from walk(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--signature', required=True, type=Path)
    parser.add_argument('--trusted-public-key', required=True, type=Path)
    parser.add_argument('--target', required=True, type=Path)
    parser.add_argument('--confirm-erase', required=True,
                        help='Repeat the canonical whole-disk path. All existing data will be erased.')
    args = parser.parse_args()
    target = args.target.resolve(strict=True)
    if args.confirm_erase != str(target):
        raise ValueError('--confirm-erase must exactly match the canonical --target path')
    target_stat = target.stat()
    if not stat.S_ISBLK(target_stat.st_mode):
        raise ValueError('The target must be a block device')
    if os.geteuid() != 0:
        raise ValueError('Whole-disk installation requires root')
    tree = json.loads(subprocess.check_output(
        ['lsblk', '--json', '--bytes', '--output', 'PATH,TYPE,SIZE,MOUNTPOINTS', str(target)], text=True))
    disks = tree['blockdevices']
    if len(disks) != 1 or disks[0]['type'] != 'disk':
        raise ValueError('The target must be a whole disk, not a partition or mapped device')
    for device in walk(disks[0]):
        if any(device.get('mountpoints') or []):
            raise ValueError(f'Target or child is mounted or used as swap: {device["path"]}')
    size = args.image.stat().st_size
    if not stat.S_ISREG(args.image.stat().st_mode) or size == 0 or size > int(disks[0]['size']):
        raise ValueError('Image is empty, not a regular file, or larger than target')
    # Verify the same open file descriptor subsequently copied, avoiding a pathname swap.
    # Private snapshot prevents the caller modifying the bytes between verify and copy.
    with tempfile.TemporaryFile() as image:
        with args.image.open('rb') as source:
            remaining = size
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise ValueError('Source image changed or was truncated while staging')
                image.write(chunk)
                remaining -= len(chunk)
        image.flush()
        image.seek(0)
        fd = image.fileno()
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(args.trusted_public_key),
                        '-signature', str(args.signature), f'/proc/self/fd/{fd}'],
                       pass_fds=(fd,), check=True)
        # O_EXCL fails if Linux sees mounted filesystems/users of this block device.
        outfd = os.open(target, os.O_WRONLY | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            fcntl.flock(outfd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            opened = os.fstat(outfd)
            if not stat.S_ISBLK(opened.st_mode) or opened.st_rdev != target_stat.st_rdev:
                raise ValueError('Target device identity changed before exclusive open')
            if struct.unpack('I', fcntl.ioctl(outfd, 0x1268, bytes(4)))[0] != 512:
                raise ValueError('Only 512-byte logical sector disks are supported')
            actual_size = struct.unpack('Q', fcntl.ioctl(outfd, 0x80081272, bytes(8)))[0]
            if actual_size != int(disks[0]['size']) or size > actual_size:
                raise ValueError('Target size changed during image verification')
            image.seek(0)
            while chunk := image.read(1024 * 1024):
                view = memoryview(chunk)
                while view:
                    written = os.write(outfd, view)
                    if written <= 0:
                        raise OSError('Target stopped accepting image data')
                    view = view[written:]
            os.fsync(outfd)
        finally:
            os.close(outfd)
    print(f'Installed signed image to {target}. All previous partitions, including state, were replaced.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(f'Installation refused or failed: {exc}')
