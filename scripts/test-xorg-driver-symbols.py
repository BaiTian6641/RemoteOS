#!/usr/bin/env python3
"""Compile small ELF fixtures for the Xorg pre-dlopen symbol-scope guard."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().with_name('check-xorg-driver-symbols.py')
spec = importlib.util.spec_from_file_location('xorg_symbols', SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


@unittest.skipUnless(shutil.which('cc') and shutil.which('readelf'), 'native ELF build tools unavailable')
class XorgSymbolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.target = self.root / 'target'
        for folder in ('usr/bin', 'usr/lib/xorg/modules/drivers',
                       'usr/lib/xorg/modules/input', 'lib'):
            (self.target / folder).mkdir(parents=True)
        self.compile('usr/bin/Xorg', 'void server_symbol(void) {}')
        self.compile('usr/lib/libdependent.so', 'void dependency_symbol(void) {}')
        self.compile('usr/lib/libbridge.so',
                     'void dependency_symbol(void); void bridge_symbol(void) { dependency_symbol(); }',
                     '-ldependent')
        # An optional module existing on disk does not put it in dlopen's scope.
        self.compile('usr/lib/xorg/modules/libint10.so', 'void vbeFree(void) {}')

    def tearDown(self):
        self.temp.cleanup()

    def compile(self, relative, source, *libraries):
        source_path = self.root / (Path(relative).name + '.c')
        source_path.write_text(source)
        subprocess.run(['cc', '-shared', '-nostdlib', '-fPIC', '-Wl,-z,now',
                        '-L' + str(self.target / 'usr/lib'), '-o', str(self.target / relative),
                        str(source_path), *libraries], check=True, capture_output=True)

    def driver(self, legacy=False, weak=False):
        text = 'void server_symbol(void); void bridge_symbol(void); '
        if legacy:
            text += 'void vbeFree(void); '
        if weak:
            text += 'void optional_symbol(void) __attribute__((weak)); '
        text += 'void entry(void) { server_symbol(); bridge_symbol(); '
        if legacy:
            text += 'vbeFree(); '
        if weak:
            text += 'if (optional_symbol) optional_symbol(); '
        text += '}'
        self.compile('usr/lib/xorg/modules/drivers/intel_drv.so', text, '-lbridge')

    def check(self):
        return checker.check(self.target, '/usr/bin/Xorg', '/usr/lib/xorg/modules/drivers/intel_drv.so')

    def test_optional_module_cannot_satisfy_immediate_binding(self):
        self.driver(legacy=True)
        result = self.check()
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['unresolved_symbols'], ['vbeFree'])
        self.assertEqual(result['missing_libraries'], [])

    def test_kms_symbols_resolve_through_server_and_transitive_dependencies(self):
        self.driver()
        result = self.check()
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(result['initial_scope_elf_files'], 4)

    def test_missing_transitive_dependency_fails(self):
        self.driver()
        (self.target / 'usr/lib/libdependent.so').unlink()
        result = self.check()
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['missing_libraries'], ['libdependent.so'])

    def test_missing_xorg_export_fails(self):
        self.driver()
        self.compile('usr/bin/Xorg', 'void unrelated_symbol(void) {}')
        self.assertEqual(self.check()['unresolved_symbols'], ['server_symbol'])

    def test_unresolved_symbol_inside_transitive_dependency_fails(self):
        self.driver()
        self.compile('usr/lib/libdependent.so',
                     'void missing_dependency_hook(void); '
                     'void dependency_symbol(void) { missing_dependency_hook(); }')
        result = self.check()
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['module_unresolved_symbols'], [])
        self.assertEqual(result['unresolved_symbols'], ['missing_dependency_hook'])
        self.assertEqual(result['unresolved_by_object'], {
            'usr/lib/libdependent.so': ['missing_dependency_hook']})
        self.assertEqual(result['strong_undefined_symbols'], 2)
        self.assertEqual(result['initial_scope_strong_undefined_symbols'], 4)
        self.assertEqual(result['initial_scope_unique_required_symbols'], 4)

    def test_undefined_weak_hook_is_optional(self):
        self.driver(weak=True)
        self.assertEqual(self.check()['status'], 'passed')

    def test_input_module_requires_its_installed_runtime_library(self):
        self.compile('usr/lib/libinput.so.10', 'void libinput_dispatch(void) {}')
        module = '/usr/lib/xorg/modules/input/libinput_drv.so'
        self.compile(module.lstrip('/'),
                     'void server_symbol(void); void libinput_dispatch(void); '
                     'void entry(void) { server_symbol(); libinput_dispatch(); }',
                     '-l:libinput.so.10')
        self.assertEqual(checker.check(self.target, '/usr/bin/Xorg', module)['status'],
                         'passed')
        (self.target / 'usr/lib/libinput.so.10').unlink()
        result = checker.check(self.target, '/usr/bin/Xorg', module)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['missing_libraries'], ['libinput.so.10'])
        self.assertEqual(result['module_unresolved_symbols'], ['libinput_dispatch'])


if __name__ == '__main__':
    unittest.main()
