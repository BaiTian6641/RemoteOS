#!/usr/bin/env python3
"""Verify DHCP DNS handoff without changing host interfaces or resolv.conf."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HOOK = Path(__file__).resolve().parents[1] / 'buildroot/board/hp2140/overlay/usr/share/udhcpc/default.script.d/50-openresolv'


class DnsHookTests(unittest.TestCase):
    def invoke(self, action, **values):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool = root / 'resolvconf'
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TEST_ROOT/args"\ncat > "$TEST_ROOT/stdin"\n')
            tool.chmod(0o755)
            env = {**os.environ, 'PATH': f'{root}:{os.environ["PATH"]}', 'TEST_ROOT': str(root), 'interface': 'wlan0', **values}
            for name in ('search', 'domain', 'dns'):
                if name not in values:
                    env.pop(name, None)
            subprocess.run([str(HOOK), action], check=True, env=env, stdin=subprocess.DEVNULL)
            return ((root / 'args').read_text().splitlines(), (root / 'stdin').read_text())

    def test_lease_search_and_servers(self):
        args, data = self.invoke('bound', search='example.test local.test', domain='ignored.test', dns='192.0.2.1 192.0.2.2')
        self.assertEqual(args, ['-a', 'wlan0'])
        self.assertEqual(data, 'search example.test local.test\nnameserver 192.0.2.1\nnameserver 192.0.2.2\n')

    def test_renew_replaces_interface_configuration(self):
        args, data = self.invoke('renew', domain='example.test', dns='192.0.2.3')
        self.assertEqual(args, ['-a', 'wlan0'])
        self.assertIn('search example.test\n', data)

    def test_deconfigure_removes_only_that_interface(self):
        args, data = self.invoke('deconfig')
        self.assertEqual(args, ['-d', 'wlan0', '-f'])
        self.assertEqual(data, '')


if __name__ == '__main__':
    unittest.main()
