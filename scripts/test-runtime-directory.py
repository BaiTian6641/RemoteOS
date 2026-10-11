#!/usr/bin/env python3
"""Check session runtime traversal under the supervisor's restrictive umask.

Only a temporary fixture is modified. With root privileges the final child
really switches to UID/GID600. Otherwise its effective permission classes are
projected onto the fixture owner's bits, and the normal unprivileged child
performs an actual create/unlink through both directories.
"""
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'buildroot/board/hp2140/overlay/usr/bin/remoteos-xsession'
SHELL = shlex.split(os.environ.get('REMOTEOS_TEST_SHELL', '/bin/sh'))


class RuntimeDirectoryTests(unittest.TestCase):
    def check_fixture(self, preparation, expected_success):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            base.chmod(0o755)
            (base / 'run').mkdir(mode=0o755)
            tools = base / 'bin'
            tools.mkdir(mode=0o755)
            owners = base / 'owners.jsonl'
            chown = tools / 'chown'
            chown.write_text('''#!/usr/bin/python3
import json, os, sys
uid, gid = map(int, sys.argv[1].split(':'))
with open(os.environ['RUNTIME_OWNERS'], 'a') as out:
    for path in sys.argv[2:]:
        out.write(json.dumps([path, uid, gid]) + '\\n')
        if os.geteuid() == 0:
            os.chown(path, uid, gid)
''')
            chown.chmod(0o755)
            script = base / 'prepare.sh'
            script.write_text('umask 077\n' + preparation.replace('/run/', str(base / 'run') + '/'))
            environment = dict(os.environ, PATH=str(tools) + ':' + os.environ['PATH'],
                               RUNTIME_OWNERS=str(owners))
            result = subprocess.run(SHELL + [str(script)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            parent, child = base / 'run/user', base / 'run/user/600'
            identities = {str(parent): (0, 0)}
            identities.update({path: (uid, gid) for path, uid, gid in
                               map(json.loads, owners.read_text().splitlines())})
            modes = {path: stat.S_IMODE(path.stat().st_mode) for path in (parent, child)}
            if expected_success:
                self.assertEqual(identities[str(parent)], (0, 0))
                self.assertEqual(modes[parent], 0o755)
                self.assertEqual(identities[str(child)], (600, 600))
                self.assertEqual(modes[child], 0o700)
            try:
                if os.geteuid() != 0:
                    for path in (child, parent):
                        uid, gid = identities[str(path)]
                        shift = 6 if uid == 600 else 3 if gid == 600 else 0
                        permissions = (modes[path] >> shift) & 7
                        path.chmod(permissions << 6)
                probe = '''import os, pathlib, sys
if os.geteuid() == 0:
    os.setgroups([])
    os.setgid(600)
    os.setuid(600)
p = pathlib.Path(sys.argv[1]) / 'private-probe'
p.write_text('runtime works')
p.unlink()
'''
                completed = subprocess.run(['/usr/bin/python3', '-c', probe, str(child)],
                                           capture_output=True, text=True, timeout=5)
                if expected_success:
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                else:
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn('PermissionError', completed.stderr)
            finally:
                parent.chmod(0o755)
                if os.geteuid() == 0:
                    os.chown(child, 0, 0)
                child.chmod(0o700)

    def test_installed_preparation_allows_user_runtime_access(self):
        # Execute the exact installed preparation commands before the unrelated
        # status/Xorg/su handoff. No test-only path or option enters the image.
        preparation = SCRIPT.read_text().split("printf '%s\\n' 'x-session-started'")[0]
        self.assertIn('export XDG_RUNTIME_DIR=', preparation)
        self.check_fixture(preparation, True)

    def test_old_parent_umask_denies_access_despite_owned_child(self):
        self.check_fixture('mkdir -p /run/user/600\nchown 600:600 /run/user/600\n'
                           'chmod 700 /run/user/600\n', False)


if __name__ == '__main__':
    unittest.main()
