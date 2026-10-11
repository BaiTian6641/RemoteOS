#!/usr/bin/env python3
"""Exercise cursor policy against a patched copy of the pinned Intel source.

The native C harness uses the actual patched initialization, reload and cleanup
statements with Xorg calls stubbed. It does not claim to test the physical cursor
plane. The source test skips when Buildroot's downloaded source is unavailable.
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'xdriver_xf86-video-intel-31486f40f8e8f8923ca0799aea84b58799754564'
PATCH = ROOT / 'buildroot/patches/packages/xdriver_xf86-video-intel/0001-sna-software-cursor-option.patch'
spec = importlib.util.spec_from_file_location('intel_cursor', ROOT / 'scripts/check-intel-cursor.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class CursorPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.target = Path(self.temp.name)
        self.config = self.target / checker.CONFIG
        self.config.parent.mkdir(parents=True)
        self.config.write_text((ROOT / 'buildroot/board/hp2140/overlay' / checker.CONFIG).read_text())
        driver = self.target / checker.DRIVER
        driver.parent.mkdir(parents=True)
        driver.write_bytes(b'fixture\0' + checker.MARKER + b'\n\0')

    def tearDown(self):
        self.temp.cleanup()

    def test_selected_feature_is_present(self):
        self.assertEqual(checker.check(self.target)['status'], 'passed')

    def test_old_driver_with_new_configuration_is_rejected(self):
        (self.target / checker.DRIVER).write_bytes(b'old driver\0HW Cursor enabled\n\0')
        result = checker.check(self.target)
        self.assertEqual(result['status'], 'failed')
        self.assertIn('clean-rebuild', result['errors'][0])

    def test_comment_or_other_device_cannot_select_the_policy(self):
        self.config.write_text('''Section "Device"
    Driver "intel"
    Option "AccelMethod" "sna"
    # Option "SWCursor" "true"
EndSection
Section "Device"
    Driver "other"
    Option "SWCursor" "true"
EndSection
''')
        self.assertEqual(checker.check(self.target)['status'], 'failed')

    def test_acceleration_must_remain_sna(self):
        self.config.write_text(self.config.read_text().replace('"sna"', '"uxa"'))
        self.assertEqual(checker.check(self.target)['status'], 'failed')

    def test_conflicting_option_is_rejected(self):
        self.config.write_text(self.config.read_text().replace(
            'Option "SWCursor" "true"',
            'Option "SWCursor" "true"\n    Option "SWCursor" "false"'))
        self.assertEqual(checker.check(self.target)['status'], 'failed')


class CursorSourceTests(unittest.TestCase):
    def test_patched_initialization_and_shutdown(self):
        archive = Path(os.environ.get('REMOTEOS_INTEL_SOURCE_ARCHIVE',
                       str(ROOT / 'output/hp2140/legal-info/sources' / PACKAGE /
                           (PACKAGE + '-git4.tar.gz'))))
        if not archive.is_file() or not shutil.which('cc') or not shutil.which('patch'):
            self.skipTest('pinned Intel source archive or native C/patch tools unavailable')
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            names = ('src/intel_options.h', 'src/intel_options.c',
                     'src/sna/sna_driver.c', 'src/sna/sna_display.c', 'man/intel.man')
            # Extract only named text files; archive paths never choose a destination.
            with tarfile.open(archive) as source:
                for name in names:
                    member = next(item for item in source.getmembers()
                                  if item.name.endswith('/' + name))
                    destination = work / name
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(source.extractfile(member).read())
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(PATCH)],
                           cwd=work, check=True, capture_output=True)
            options = (work / 'src/intel_options.c').read_text()
            enum = (work / 'src/intel_options.h').read_text()
            self.assertRegex(options, r'OPTION_SW_CURSOR,\s*"SWCursor",\s*OPTV_BOOLEAN')
            self.assertRegex(enum, r'#ifdef USE_SNA\b[^#]*\bOPTION_SW_CURSOR,')
            driver = (work / 'src/sna/sna_driver.c').read_text()
            start = driver.index('\tif (!miDCInitialize(')
            end = driver.index('\n\t/* Must force it before EnterVT', start)
            init = driver[start:end]
            display = (work / 'src/sna/sna_display.c').read_text()

            def function(name):
                start = display.index('static void\n' + name + '(')
                opening = display.index('{', start)
                depth = 1
                for end in range(opening + 1, len(display)):
                    depth += (display[end] == '{') - (display[end] == '}')
                    if depth == 0:
                        return display[start:end + 1]
                self.fail('unterminated source function: ' + name)

            harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define FALSE 0
#define TRUE 1
#define X_CONFIG 1
#define X_INFO 2
#define OPTION_SW_CURSOR 3
#define None 0
#define DBG(args) do {} while (0)
typedef int Bool;
typedef void *ScreenPtr;
struct scrn { int scrnIndex; };
struct sna { struct scrn *scrn; void *Options; struct {
    void *info, *ref; int active, last_x, last_y;
} cursor; };
static int mi_ok, hw_ok, sw_option, mi_calls, hw_calls, moves, frees, destroys;
static const char *last_log;
static void *xf86GetPointerScreenFuncs(void) { return NULL; }
static Bool miDCInitialize(ScreenPtr screen, void *funcs) {
    (void)screen; (void)funcs; ++mi_calls; return mi_ok;
}
static Bool xf86ReturnOptValBool(void *options, int option, Bool fallback) {
    (void)options; assert(option == OPTION_SW_CURSOR); assert(fallback == FALSE);
    return sw_option < 0 ? fallback : sw_option;
}
static Bool sna_cursors_init(ScreenPtr screen, struct sna *sna) {
    (void)screen; ++hw_calls; if (hw_ok) sna->cursor.info = sna; return hw_ok;
}
static void xf86DrvMsg(int index, int type, const char *message, ...) {
    (void)index; (void)type; last_log = message;
}
static void sna_set_cursor_position(struct scrn *scrn, int x, int y) {
    (void)scrn; assert(x == 17 && y == 29); ++moves;
}
static void xf86DestroyCursorInfoRec(void *info) { assert(info); ++destroys; }
static void FreeCursor(void *ref, int resource) { assert(ref); (void)resource; ++frees; }
static Bool initialize(ScreenPtr screen, struct sna *sna) {
    struct scrn *scrn = sna->scrn;
''' + init + r'''
    return TRUE;
}
''' + function('sna_cursors_reload') + '\n' + function('sna_cursors_fini') + r'''
static void reset(void) {
    mi_ok = hw_ok = TRUE; sw_option = -1;
    mi_calls = hw_calls = moves = frees = destroys = 0; last_log = "";
}
int main(void) {
    struct scrn scrn = {0};
    struct sna sna = {.scrn = &scrn};
    reset(); sw_option = TRUE;
    for (int generation = 0; generation < 3; ++generation) {
        assert(initialize(NULL, &sna));
        assert(!sna.cursor.info && !sna.cursor.ref);
        sna_cursors_reload(&sna); sna_cursors_fini(&sna);
    }
    assert(mi_calls == 3 && hw_calls == 0 && moves == 0 && destroys == 0 && frees == 0);
    assert(strcmp(last_log, "Software cursor enabled (SWCursor)\n") == 0);
    reset();
    assert(initialize(NULL, &sna));
    assert(mi_calls == 1 && hw_calls == 1 && sna.cursor.info);
    assert(strcmp(last_log, "HW Cursor enabled\n") == 0);
    sna.cursor.active = TRUE; sna.cursor.last_x = 17; sna.cursor.last_y = 29;
    sna.cursor.ref = &sna; sna_cursors_reload(&sna); sna_cursors_fini(&sna);
    assert(moves == 1 && destroys == 1 && frees == 1);
    assert(!sna.cursor.info && !sna.cursor.ref); sna.cursor.active = FALSE;
    reset(); hw_ok = FALSE;
    assert(initialize(NULL, &sna));
    assert(hw_calls == 1 && !sna.cursor.info);
    sna_cursors_reload(&sna); sna_cursors_fini(&sna);
    assert(moves == 0 && destroys == 0 && frees == 0);
    reset(); mi_ok = FALSE; sw_option = TRUE;
    assert(!initialize(NULL, &sna)); assert(mi_calls == 1 && hw_calls == 0);
    reset(); sw_option = FALSE;
    assert(initialize(NULL, &sna)); assert(hw_calls == 1); sna_cursors_fini(&sna);
    puts("cursor initialization and cleanup regression passed");
    return 0;
}
'''
            (work / 'cursor-test.c').write_text(harness)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(work / 'cursor-test.c'), '-o', str(work / 'cursor-test')],
                           check=True, capture_output=True)
            subprocess.run([str(work / 'cursor-test')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
