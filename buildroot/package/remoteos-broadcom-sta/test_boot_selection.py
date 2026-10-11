#!/usr/bin/env python3
"""Fixture-only tests. Never executes modprobe or inspects the real PCI bus."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PACKAGE = Path(__file__).resolve().parent


class DriverSelection(unittest.TestCase):
    def run_boot(self, choice="", device=True, bound=None, fail=False, no_bind=False):
        with tempfile.TemporaryDirectory(prefix="remoteos-wl-fixture-") as temporary:
            root = Path(temporary)
            for part in ["proc", "run", "state/reports", "sys/bus/pci/devices", "bin"]:
                (root / part).mkdir(parents=True, exist_ok=True)
            (root / "proc/cmdline").write_text("quiet " + choice)
            card = root / "sys/bus/pci/devices/0000:08:00.0"
            if device:
                card.mkdir()
                (card / "vendor").write_text("0x14e4\n")
                (card / "device").write_text("0x432b\n")
                if bound:
                    (card / "driver").symlink_to("/fixture/drivers/" + bound)
            # Rewrite only filesystem roots in a temporary copy of the script;
            # the installed script has no environment-based path override.
            script = (PACKAGE / "S08remoteos-wifi-driver").read_text()
            for prefix in ["/proc/", "/sys/", "/run/", "/state/"]:
                script = script.replace(prefix, str(root) + prefix)
            (root / "boot.sh").write_text(script)
            fake = root / "bin/modprobe"
            fake.write_text("""#!/usr/bin/env python3
import os, pathlib, sys
root = pathlib.Path(os.environ['FIXTURE_ROOT'])
with (root / 'calls').open('a') as f: f.write(' '.join(sys.argv[1:]) + '\\n')
if os.environ.get('FIXTURE_FAIL') == '1':
    print('fixture: invalid module format', file=sys.stderr)
    sys.exit(1)
driver = root / 'sys/bus/pci/devices/0000:08:00.0/driver'
if os.environ.get('FIXTURE_NO_BIND') != '1' and not driver.is_symlink():
    driver.symlink_to('/fixture/drivers/' + ('wl' if sys.argv[1] == 'wl' else 'b43-pci-bridge'))
""")
            fake.chmod(0o755)
            env = dict(os.environ, PATH=str(root / "bin") + ":" + os.environ["PATH"],
                       FIXTURE_ROOT=str(root), FIXTURE_FAIL=str(int(fail)),
                       FIXTURE_NO_BIND=str(int(no_bind)))
            result = subprocess.run(["/bin/sh", str(root / "boot.sh"), "start"],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            status = (root / "run/remoteos/wifi-driver-status").read_text()
            self.assertEqual(status, (root / "state/reports/latest-wifi-driver.txt").read_text())
            return {
                "status": dict(line.split("=", 1) for line in status.splitlines()),
                "rules": (root / "run/modprobe.d/remoteos-wifi-driver.conf").read_text(),
                "calls": (root / "calls").read_text().splitlines() if (root / "calls").exists() else [],
                "log": (root / "state/reports/latest-wifi-driver-load.txt").read_text(),
            }

    def test_explicit_wl_binds_and_only_blocks_competitors(self):
        r = self.run_boot("remoteos.wifi_driver=wl")
        self.assertEqual(r["calls"], ["wl"])
        self.assertEqual(r["status"]["pci_driver"], "wl")
        self.assertEqual(r["status"]["result"], "loaded")
        self.assertEqual(set(r["rules"].splitlines()), {
            "blacklist b43", "blacklist b43legacy", "blacklist brcmsmac",
            "blacklist ssb", "blacklist bcma"})

    def test_recovery_and_no_selection_block_wl(self):
        for choice in ["", "remoteos.wifi_driver=b43"]:
            with self.subTest(choice=choice):
                r = self.run_boot(choice)
                self.assertEqual(r["calls"], ["b43"])
                self.assertEqual(r["status"]["pci_driver"], "b43-pci-bridge")
                self.assertEqual(r["rules"], "blacklist wl\n")

    def test_unsupported_device_does_not_load_broad_wl_alias(self):
        r = self.run_boot("remoteos.wifi_driver=wl", device=False)
        self.assertEqual(r["calls"], [])
        self.assertEqual(r["status"]["result"], "unavailable")
        self.assertEqual(r["rules"], "blacklist wl\n")

    def test_invalid_choice_is_not_executed(self):
        r = self.run_boot("remoteos.wifi_driver=evil")
        self.assertEqual(r["calls"], [])
        self.assertEqual(r["status"]["selected"], "invalid")
        self.assertEqual(r["rules"], "blacklist wl\n")

    def test_load_failure_never_falls_back(self):
        r = self.run_boot("remoteos.wifi_driver=wl", fail=True)
        self.assertEqual(r["calls"], ["wl"])
        self.assertEqual(r["status"]["result"], "failed")
        self.assertIn("invalid module format", r["log"])
        self.assertNotIn("blacklist wl", r["rules"])

    def test_preexisting_b43_is_never_forcibly_unbound(self):
        r = self.run_boot("remoteos.wifi_driver=wl", bound="b43-pci-bridge")
        self.assertEqual(r["calls"], [])
        self.assertEqual(r["status"]["pci_driver"], "b43-pci-bridge")
        self.assertEqual(r["status"]["result"], "failed")

    def test_loaded_module_without_device_binding_is_not_success(self):
        r = self.run_boot("remoteos.wifi_driver=wl", no_bind=True)
        self.assertEqual(r["calls"], ["wl"])
        self.assertEqual(r["status"]["result"], "failed")

    def test_recovery_does_not_claim_or_replace_preexisting_wl(self):
        r = self.run_boot("remoteos.wifi_driver=b43", bound="wl")
        self.assertEqual(r["calls"], [])
        self.assertEqual(r["status"]["pci_driver"], "wl")
        self.assertEqual(r["status"]["result"], "failed")


class ProfileCleanup(unittest.TestCase):
    def test_base_removes_only_optional_files_and_refreshes_dependencies(self):
        self.check_cleanup(enabled=False)

    def test_wl_profile_retains_optional_files(self):
        self.check_cleanup(enabled=True)

    def check_cleanup(self, enabled):
        # Run the actual isolated cleanup prefix, before unrelated board checks.
        board = PACKAGE.parents[1] / "board/hp2140/post-build.sh"
        prefix = board.read_text().split('python3 "$BR2_EXTERNAL_REMOTEOS_PATH', 1)[0]
        with tempfile.TemporaryDirectory(prefix="remoteos-wl-cleanup-") as temporary:
            root = Path(temporary)
            target = root / "target"
            owned = ["etc/init.d/S08remoteos-wifi-driver",
                     "usr/share/remoteos/wifi-driver-profile",
                     "usr/share/licenses/remoteos-broadcom-sta/license",
                     "usr/share/doc/remoteos-broadcom-sta/README.md",
                     "lib/modules/6.12.109/extra/wl.ko"]
            other = "lib/modules/6.12.109/kernel/b43.ko"
            for name in owned + [other]:
                path = target / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n")
            config = root / "config"
            config.write_text("BR2_PACKAGE_REMOTEOS_BROADCOM_STA=y\n" if enabled else "")
            host = root / "host/sbin"
            host.mkdir(parents=True)
            depmod = host / "depmod"
            depmod.write_text('#!/bin/sh\nprintf "%s\\n" "$*" > "$TEST_DEPMOD_LOG"\n')
            depmod.chmod(0o755)
            script = root / "cleanup.sh"
            script.write_text(prefix)
            env = dict(os.environ, TARGET_DIR=str(target), BR2_CONFIG=str(config),
                       HOST_DIR=str(root / "host"), TEST_DEPMOD_LOG=str(root / "depmod.log"))
            subprocess.run(["/bin/sh", str(script)], env=env, check=True, timeout=5)
            for name in owned:
                self.assertEqual((target / name).exists(), enabled, name)
            self.assertTrue((target / other).exists())
            self.assertEqual((root / "depmod.log").exists(), not enabled)
            if not enabled:
                self.assertEqual((root / "depmod.log").read_text().strip(),
                                 f"-a -b {target} 6.12.109")


if __name__ == "__main__":
    unittest.main()
