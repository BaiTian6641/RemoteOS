#!/usr/bin/env python3
"""Exercise HP output selection without touching the development host's X server."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

LAYOUT = Path(__file__).resolve().parents[1] / "buildroot/board/hp2140/overlay/usr/bin/remoteos-display-layout"


class DisplayLayoutTests(unittest.TestCase):
    def layout(self, outputs):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "query").write_text(outputs)
            (root / "xrandr").write_text("#!/bin/sh\nif [ \"$1\" = --query ]; then cat \"$TEST_ROOT/query\"; else printf '%s\\n' \"$@\" > \"$TEST_ROOT/args\"; fi\n")
            (root / "logger").write_text("#!/bin/sh\nexit 0\n")
            for name in ("xrandr", "logger"):
                (root / name).chmod(0o755)
            subprocess.run([str(LAYOUT)], check=True, env={**os.environ, "PATH": f"{root}:{os.environ['PATH']}", "TEST_ROOT": str(root)})
            return (root / "args").read_text().splitlines() if (root / "args").exists() else []

    def test_full_hd_and_native_panel_are_distinct(self):
        args = self.layout("LVDS1 connected primary 1366x768+0+0\n   1366x768 60.0*+\nVGA1 connected\n   2048x1536 75.0+\n   1280x720 60.0\n   1920x1080 60.0\n")
        self.assertEqual(args, ["--output", "LVDS1", "--auto", "--primary", "--output", "VGA1", "--mode", "1920x1080", "--below", "LVDS1"])

    def test_lower_mode_excludes_interlaced_and_other_outputs(self):
        args = self.layout("LVDS-1 connected\n   1366x768 60.0\nVGA-1 connected\n   1920x1080i 60.0\n   1280x1024 60.0+\n   1024x768 60.0\nHDMI-1 connected\n   1920x1080 60.0\n")
        self.assertEqual(args[7], "1280x1024")

    def test_disconnect_disables_old_crtc(self):
        args = self.layout("LVDS1 connected\nVGA1 disconnected 1920x1080+0+768\n")
        self.assertEqual(args[-3:], ["--output", "VGA1", "--off"])

    def test_no_usable_external_mode(self):
        args = self.layout("LVDS1 connected\nVGA1 connected\n   2048x1536 75.0\n   1920x1080i 60.0\n")
        self.assertEqual(args[-1], "--off")

    def test_no_internal_panel_does_not_mutate_unknown_hardware(self):
        self.assertEqual(self.layout("Virtual-1 connected\n   1920x1080 60.0\n"), [])


if __name__ == "__main__":
    unittest.main()
