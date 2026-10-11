#!/usr/bin/env python3
"""Verify appliance operator repair offline in a snapshot-only QEMU guest.

The actual image's UID 600 calls the actual supervisor. No host Tailscale,
network interface, account enrollment, writable disk mount or real USB is used.
This component test uses init=/bin/sh; a separate BIOS boot test covers init.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import selectors
import subprocess
import tempfile
import time


PROBE = r'''
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 int fd = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a = {0};
 a.sun_family = AF_UNIX; strcpy(a.sun_path, "/run/remoteos/supervisor.sock");
 if (fd < 0 || connect(fd, (void*)&a, sizeof(a))) return 2;
 const char q[] = "{\"action\":\"tailscale_prepare\"}\n";
 if (write(fd, q, sizeof(q)-1) != sizeof(q)-1) return 3;
 char response[1024] = {0}; int n = read(fd, response, sizeof(response)-1); close(fd);
 if(n <= 0) return 4; printf("UID=%u %s", (unsigned)getuid(), response);
 return strstr(response, "\"ok\":true") ? 0 : 5;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='remoteos-tailscale-probe-') as temp:
        temporary = Path(temp); source = temporary / 'probe.c'; binary = temporary / 'probe'
        source.write_text(PROBE)
        # Preserve the GCC wrapper's invoked basename; resolving its symlink
        # would make Buildroot search for toolchain-wrapper.br_real.
        subprocess.run([str(args.cc.absolute()), '-Os', '-s', str(source), '-o', str(binary)], check=True, timeout=20)
        command = ['qemu-system-i386', '-accel', 'tcg', '-cpu', 'n270', '-smp', '2', '-m', '2048',
                   '-kernel', str(args.kernel.resolve()), '-append', 'root=/dev/sda1 rootfstype=squashfs ro init=/bin/sh console=ttyS0 loglevel=3',
                   '-drive', f'file={args.image.resolve()},format=raw,if=ide,snapshot=on', '-nic', 'none', '-display', 'none',
                   '-monitor', 'none', '-serial', 'stdio']
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        selector = selectors.DefaultSelector(); selector.register(process.stdout, selectors.EVENT_READ)
        full = bytearray(); pending = bytearray()
        def until(marker, seconds=30):
            end = time.monotonic() + seconds
            while marker not in pending:
                if process.poll() is not None: raise RuntimeError('QEMU exited')
                if time.monotonic() >= end: raise RuntimeError('Guest command timed out')
                if selector.select(timeout=0.25):
                    data = process.stdout.read1(65536); full.extend(data); pending.extend(data)
            end = pending.index(marker) + len(marker)
            data = bytes(pending[:end]); del pending[:end]; return data.decode(errors='replace')
        counter = 0
        def run(text, seconds=30):
            nonlocal counter
            counter += 1; mark = f'REMOTEOS_PROBE_DONE_{counter}'
            process.stdin.write((text + f"\nprintf '\\n{mark}\\n'\n").encode()); process.stdin.flush()
            return until((mark + '\r\n').encode(), seconds)
        try:
            until(b"can't access tty; job control turned off", 30)
            run('mount -t proc proc /proc\nmount -t sysfs sysfs /sys\nmount -t tmpfs tmpfs /run\nmount -t tmpfs tmpfs /tmp\nmount -t tmpfs tmpfs /state\nmkdir -p /run/tailscale /state/home /state/tailscale\nchown remoteos:remoteos /state/home\nid remoteos')
            encoded = base64.b64encode(binary.read_bytes()).decode()
            run("stty -echo\ncat >/tmp/probe.b64 <<'REMOTEOS_PROBE_BINARY'\n" + '\n'.join(encoded[i:i+76] for i in range(0,len(encoded),76)) +
                '\nREMOTEOS_PROBE_BINARY\nbase64 -d /tmp/probe.b64 >/tmp/repair-probe\nchmod 755 /tmp/repair-probe')
            run('/usr/sbin/tailscaled --tun=userspace-networking --port=0 --socket=/run/tailscale/tailscaled.sock --state=/state/tailscale/test.state --statedir=/state/tailscale > /tmp/tailscaled.log 2>&1 &\nfor n in 1 2 3 4 5 6 7 8 9 10; do [ -S /run/tailscale/tailscaled.sock ] && break; sleep 0.2; done')
            up = 'env USER=remoteos LOGNAME=remoteos HOME=/state/home tailscale --socket=/run/tailscale/tailscaled.sock up --operator=remoteos --json --timeout=5s'
            before = run("su -p -s /bin/sh remoteos -c '" + up + "' 2>&1\n", 15)
            if 'checkprefs access denied' not in before: raise AssertionError('Expected denial was not reproduced')
            repair = run("/usr/sbin/remoteos-supervisor >/tmp/supervisor.log 2>&1 &\nsleep 0.5\nsu -p -s /bin/sh remoteos -c /tmp/repair-probe\n", 12)
            if 'UID=600 {"ok":true}' not in repair: raise AssertionError('UID 600 could not repair enrollment through the supervisor: ' + repair)
            permission = run("su -p -s /bin/sh remoteos -c 'env USER=remoteos LOGNAME=remoteos HOME=/state/home tailscale --socket=/run/tailscale/tailscaled.sock set --operator=remoteos'; printf 'SET_RESULT=%s\\n' $?\n")
            if 'SET_RESULT=0' not in permission: raise AssertionError('Operator still lacks preference write access')
            after = run("su -p -s /bin/sh remoteos -c '" + up + "' 2>&1\n", 15)
            if 'access denied' in after.lower(): raise AssertionError('Enrollment is still denied: ' + after)
            if 'timeout' not in after.lower() and 'timed out' not in after.lower(): raise AssertionError('Offline enrollment should time out: ' + after)
            receipt = dict(status='passed', image_sha256=hashlib.file_digest(args.image.open('rb'), 'sha256').hexdigest(),
                           reproduced_denial=True, supervisor_caller_uid=600, supervisor_repair_ok=True, operator_write_result=0,
                           enrollment_after_repair='Offline timeout without access denied',
                           command=command, hardware_qualified=False, google_enrollment_qualified=False,
                           before=before, repair=repair, write_permission=permission, after=after)
            (args.output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
            print(json.dumps({k:v for k,v in receipt.items() if k not in ('command','before','repair','write_permission','after')}, indent=2))
        finally:
            (args.output / 'serial.log').write_bytes(full)
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            selector.close(); process.stdin.close(); process.stdout.close()


if __name__ == '__main__':
    main()
