#!/usr/bin/env python3
"""Offline signed RemoteOS update preserving the existing state partition.

Run from recovery media, with the target disk and every child unmounted.
Only the verified image prefix before the final state partition is written.
Layout changes need a separate migration; this tool never resizes partitions.
"""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile

SECTOR_SIZE = 512
DISK_SIGNATURE = 0x524F5331
STATE_LABEL = b'REMOTEOS_STATE'
BLKGETSIZE64 = 0x80081272
BLKSSZGET = 0x1268


@dataclass(frozen=True)
class Partition:
    index: int
    boot: int
    type: int
    start: int
    sectors: int
    raw: bytes

    @property
    def end(self):
        return self.start + self.sectors


def parse_layout(mbr, disk_size):
    """Validate the supported v1 MBR format without writing anything."""
    if len(mbr) != SECTOR_SIZE or mbr[510:512] != b'\x55\xaa':
        raise ValueError('Invalid MBR signature')
    if struct.unpack_from('<I', mbr, 440)[0] != DISK_SIGNATURE:
        raise ValueError('Disk is not a recognized RemoteOS v1 image')
    partitions = []
    for index in range(4):
        raw = mbr[446 + 16 * index:462 + 16 * index]
        boot, kind = raw[0], raw[4]
        start, sectors = struct.unpack_from('<II', raw, 8)
        if kind == 0xEE:
            raise ValueError('GPT images and targets are not supported')
        if index >= 2:
            if raw != bytes(16):
                raise ValueError('Only the two-partition RemoteOS v1 layout is supported')
            continue
        if kind != 0x83 or not start or not sectors or boot not in (0, 0x80):
            raise ValueError('Expected Linux root and state MBR partitions')
        if (index == 0 and boot != 0x80) or (index == 1 and boot != 0):
            raise ValueError('Unexpected root/state boot flags')
        partitions.append(Partition(index + 1, boot, kind, start, sectors, raw))
    root, state = partitions
    if root.start < 2048 or root.end > state.start:
        raise ValueError('Root/state partitions overlap or the boot area is too small')
    if state.sectors * SECTOR_SIZE < 2048 or state.end * SECTOR_SIZE > disk_size:
        raise ValueError('State partition is truncated or outside the disk')
    return root, state


def check_state_superblock(read_at, state):
    superblock = read_at(state.start * SECTOR_SIZE + 1024, 1024)
    if len(superblock) != 1024 or superblock[56:58] != b'\x53\xef':
        raise ValueError('State partition does not contain the expected ext4 filesystem')
    if superblock[120:136].rstrip(b'\0') != STATE_LABEL:
        raise ValueError('State partition label is not REMOTEOS_STATE')


def compatible_prefix(image_mbr, image_size, target_mbr, target_size):
    source = parse_layout(image_mbr, image_size)
    target = parse_layout(target_mbr, target_size)
    if image_size != source[1].end * SECTOR_SIZE:
        raise ValueError('Signed image has data outside the supported final state partition')
    if tuple(p.raw for p in source) != tuple(p.raw for p in target):
        raise ValueError('Partition layout changed; preserving state requires an explicit migration')
    return source[1].start * SECTOR_SIZE, source[1], target[1]


@contextmanager
def verified_snapshot(image_path, signature, public_key, maximum_size):
    """Verify an immutable private copy and yield only a read-only descriptor."""
    source_fd = os.open(image_path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        info = os.fstat(source_fd)
        if not stat.S_ISREG(info.st_mode) or not SECTOR_SIZE <= info.st_size <= maximum_size:
            raise ValueError('Signed image must be a regular file that fits the target disk')
        with tempfile.TemporaryFile() as staging:
            remaining = info.st_size
            while remaining:
                chunk = os.read(source_fd, min(1024 * 1024, remaining))
                if not chunk:
                    raise ValueError('Image changed or was truncated while staging')
                staging.write(chunk)
                remaining -= len(chunk)
            if os.read(source_fd, 1):
                raise ValueError('Image grew while staging')
            staging.flush()
            readonly = os.open(f'/proc/self/fd/{staging.fileno()}', os.O_RDONLY | os.O_CLOEXEC)
    finally:
        os.close(source_fd)
    with os.fdopen(readonly, 'rb') as snapshot:
        fd = snapshot.fileno()
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public_key),
                        '-signature', str(signature), f'/proc/self/fd/{fd}'],
                       pass_fds=(fd,), check=True, capture_output=True, text=True)
        snapshot.seek(0)
        yield snapshot, info.st_size


def copy_prefix(source, target_fd, length):
    """Copy exactly length bytes, leaving all remaining target bytes untouched."""
    if length < SECTOR_SIZE or length > os.fstat(source.fileno()).st_size:
        raise ValueError('Invalid verified image prefix length')
    source.seek(0)
    position = 0
    while position < length:
        chunk = source.read(min(1024 * 1024, length - position))
        if not chunk:
            raise ValueError('Verified image unexpectedly ended during update')
        view = memoryview(chunk)
        while view:
            written = os.pwrite(target_fd, view, position)
            if written <= 0:
                raise OSError('Target stopped accepting update data')
            position += written
            view = view[written:]
    os.fsync(target_fd)


def walk(node):
    yield node
    for child in node.get('children', []):
        yield from walk(child)


def inspect_target(target):
    tree = json.loads(subprocess.check_output(
        ['lsblk', '--json', '--bytes', '--output', 'PATH,TYPE,SIZE,MOUNTPOINTS', str(target)], text=True))
    disks = tree.get('blockdevices', [])
    if len(disks) != 1 or disks[0].get('type') != 'disk':
        raise ValueError('Target must be a whole disk, not a partition or mapped device')
    for child in walk(disks[0]):
        if any(child.get('mountpoints') or []):
            raise ValueError(f'Target or child is mounted or used as swap: {child["path"]}')
    return int(disks[0]['size'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--signature', required=True, type=Path)
    parser.add_argument('--trusted-public-key', required=True, type=Path)
    parser.add_argument('--target', required=True, type=Path)
    parser.add_argument('--confirm-update', required=True,
                        help='Repeat the canonical disk path. The OS is replaced; state is preserved.')
    args = parser.parse_args()
    target = args.target.resolve(strict=True)
    if args.confirm_update != str(target):
        raise ValueError('--confirm-update must exactly match the canonical --target path')
    target_stat = target.stat()
    if not stat.S_ISBLK(target_stat.st_mode):
        raise ValueError('The target must be a block device')
    if os.geteuid() != 0:
        raise ValueError('Offline updates require root')
    target_size = inspect_target(target)
    with verified_snapshot(args.image, args.signature, args.trusted_public_key, target_size) as (image, image_size):
        # Refuse mounted filesystems/kernel holders even if they changed since
        # the earlier inventory. No writes occur until every check below passes.
        fd = os.open(target, os.O_RDWR | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            opened = os.fstat(fd)
            if not stat.S_ISBLK(opened.st_mode) or opened.st_rdev != target_stat.st_rdev:
                raise ValueError('Target device identity changed before exclusive open')
            if struct.unpack('I', fcntl.ioctl(fd, BLKSSZGET, bytes(4)))[0] != SECTOR_SIZE:
                raise ValueError('Only 512-byte logical sector disks are supported')
            actual_size = struct.unpack('Q', fcntl.ioctl(fd, BLKGETSIZE64, bytes(8)))[0]
            if actual_size != target_size or inspect_target(target) != target_size:
                raise ValueError('Target size or topology changed during verification')
            prefix, source_state, target_state = compatible_prefix(
                os.pread(image.fileno(), SECTOR_SIZE, 0), image_size,
                os.pread(fd, SECTOR_SIZE, 0), actual_size)
            check_state_superblock(lambda offset, size: os.pread(image.fileno(), size, offset), source_state)
            check_state_superblock(lambda offset, size: os.pread(fd, size, offset), target_state)
            copy_prefix(image, fd, prefix)
        finally:
            os.close(fd)
    print(f'Updated the signed OS on {target}; the existing state partition and trailing disk data were preserved.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        sys.exit(f'Update refused or failed: {exc}')
