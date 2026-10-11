#!/usr/bin/env python3
"""Stage a Linux application host with its current userspace runtime.

This is a native development bundle, not a cross-distribution release. glibc
and graphics/audio servers remain supplied by the system. No drivers/services
are packaged or installed. Existing destinations are never replaced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def private_wallet_runtime(directory):
    """Validate a qualified bridge payload without executing it.

    Its Qt/KDE dependencies remain supplied by the qualified host distribution.
    Only the matching wallet backend is private; it must never enter the main
    bundle library path, where its SONAME could affect other desktop programs.
    """
    directory = directory.resolve()
    manifest_path = directory / 'manifest.json'
    if manifest_path.is_symlink():
        raise RuntimeError('Private wallet manifest must be a regular file')
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('schema_version') != 1 or
            manifest.get('kind') != 'remoteos-private-kwallet-bridge' or
            manifest.get('status') != 'qualified' or
            manifest.get('distributable') is not True or
            manifest.get('instrumented') is not False or
            manifest.get('build_options', {}).get('gpg_enabled') is not True):
        raise RuntimeError('Private wallet runtime must be a qualified, distributable, non-instrumented, GPG-enabled build')

    def relative(value, prefix=None):
        if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_.+/-]+', value):
            raise RuntimeError('Invalid private wallet payload path')
        path = PurePosixPath(value)
        if path.is_absolute() or '..' in path.parts or str(path) != value:
            raise RuntimeError('Private wallet payload paths must be canonical and relative')
        if prefix and (not path.parts or path.parts[0] != prefix):
            raise RuntimeError('Private wallet payload belongs outside ' + prefix + ': ' + value)
        return value

    files = {}
    for row in manifest.get('files', []):
        name = relative(row['path'])
        if (name in files or len(PurePosixPath(name).parts) < 2 or
                PurePosixPath(name).parts[0] not in ('runtime', 'sources', 'licenses', 'provenance')):
            raise RuntimeError('Duplicate or misplaced private wallet payload: ' + name)
        sha = row['sha256']
        if not isinstance(sha, str) or not re.fullmatch(r'[0-9a-f]{64}', sha):
            raise RuntimeError('Invalid private wallet SHA-256: ' + name)
        files[name] = sha
    actual = set()
    for path in directory.rglob('*'):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise RuntimeError('Private wallet payload cannot contain links or special files: ' + str(path))
        if path.is_file() and path != manifest_path:
            actual.add(str(path.relative_to(directory)))
    if not files or actual != set(files):
        raise RuntimeError('Private wallet manifest must hash every payload file exactly once')
    for name, sha in files.items():
        if digest(directory / name) != sha:
            raise RuntimeError('Private wallet payload hash mismatch: ' + name)

    def listed(value, prefix):
        name = relative(value, prefix)
        if name not in files:
            raise RuntimeError('Unhashed private wallet metadata reference: ' + name)
        return name

    runtime = manifest.get('runtime', {})
    entrypoint = listed(runtime.get('entrypoint'), 'runtime')
    executable = listed(runtime.get('executable'), 'runtime')
    backend = listed(runtime.get('backend'), 'runtime')
    if entrypoint != 'runtime/run-bridge' or executable != 'runtime/kwalletd6' or backend != 'runtime/libKF6WalletBackend.so.6':
        raise RuntimeError('Private wallet runtime does not match the launcher entrypoint contract')
    libraries = [listed(name, 'runtime') for name in runtime.get('libraries', [])]
    if len(set(libraries)) != len(libraries) or backend not in libraries:
        raise RuntimeError('Private wallet runtime must include its exact matching backend once')
    runtime_files = {entrypoint, executable, *libraries}
    if runtime_files != {name for name in files if name.startswith('runtime/')}:
        raise RuntimeError('Private wallet runtime has undeclared executable/library files')
    if runtime.get('runpath') != '$ORIGIN':
        raise RuntimeError('Private wallet runtime must use only the relative $ORIGIN RUNPATH')
    if not {'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT'}.issubset(runtime.get('clear_environment', [])):
        raise RuntimeError('Private wallet wrapper must clear inherited dynamic-loader overrides')
    wrapper = directory / entrypoint
    if not wrapper.read_bytes().startswith(b'#!') or not os.access(wrapper, os.X_OK) or not os.access(directory / executable, os.X_OK):
        raise RuntimeError('Private wallet wrapper and bridge must be executable')

    for name in [executable, *libraries]:
        path = directory / name
        header = subprocess.check_output(['readelf', '-h', str(path)], text=True, timeout=10)
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True, timeout=10)
        if 'ELF64' not in header or 'Advanced Micro Devices X86-64' not in header:
            raise RuntimeError('Private wallet runtime must be native Linux x86_64: ' + name)
        runpaths = re.findall(r'\(RUNPATH\).*\[(.*?)\]', dynamic)
        if runpaths != ['$ORIGIN'] or '(RPATH)' in dynamic:
            raise RuntimeError('Private wallet runtime contains a non-relocatable library path: ' + name)
        needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
        if any('/' in dependency for dependency in needed):
            raise RuntimeError('Private wallet dependency contains an embedded path: ' + name)
        if name == executable and 'libKF6WalletBackend.so.6' not in needed:
            raise RuntimeError('Private wallet bridge does not link its declared backend')
        if name in libraries:
            sonames = re.findall(r'\(SONAME\).*\[(.*?)\]', dynamic)
            if sonames != [PurePosixPath(name).name] or len(PurePosixPath(name).parts) != 2:
                raise RuntimeError('Private wallet library must use regular SONAME bytes beside its executable: ' + name)

    if not isinstance(manifest.get('source_version'), str) or not manifest['source_version'].strip():
        raise RuntimeError('Private wallet source version is required')
    for field, prefix in [('corresponding_source', 'sources'), ('patches', 'sources'), ('license_files', 'licenses')]:
        entries = manifest.get(field)
        if not isinstance(entries, list) or not entries:
            raise RuntimeError('Private wallet runtime requires ' + field)
        for name in entries:
            listed(name, prefix)
    listed(manifest.get('build_recipe'), 'sources')
    expected_runtime = {name: files[name] for name in sorted(runtime_files)}
    for test in ('browser', 'relocation'):
        validation = manifest.get('validation', {}).get(test, {})
        if validation.get('status') != 'passed':
            raise RuntimeError('Private wallet runtime lacks a passing ' + test + ' qualification')
        receipt_path = listed(validation.get('receipt'), 'provenance')
        receipt = json.loads((directory / receipt_path).read_text())
        if receipt.get('status') != 'passed' or receipt.get('runtime_sha256') != expected_runtime:
            raise RuntimeError('Private wallet ' + test + ' receipt does not bind the exact runtime files')

    # Keep all associated sources, notices and proof with the optional runtime.
    destinations = {'runtime': 'libexec/remoteos/kwallet-bridge',
                    'sources': 'sources/kwallet-bridge',
                    'licenses': 'licenses/kwallet-bridge',
                    'provenance': 'sources/kwallet-bridge/provenance'}
    payload = []
    for name, sha in sorted(files.items()):
        first, rest = name.split('/', 1)
        payload.append((directory / name, destinations[first] + '/' + rest, sha))
    payload.append((manifest_path, 'sources/kwallet-bridge/manifest.json', digest(manifest_path)))
    if len({row[1] for row in payload}) != len(payload):
        raise RuntimeError('Private wallet payload has conflicting package destinations')
    return manifest, payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--private-wallet-runtime', type=Path,
                        help='Include a prebuilt, qualified private KWallet bridge with complete source and provenance')
    args = parser.parse_args()
    wallet = private_wallet_runtime(args.private_wallet_runtime) if args.private_wallet_runtime else None
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    for directory in ('bin', 'lib', 'lib/gstreamer-1.0', 'share/fonts', 'licenses'):
        (output / directory).mkdir(parents=True, exist_ok=True)
    files = []
    origins = set()

    def copy(source, relative):
        source = Path(source)
        destination = output / relative
        if destination.exists():
            if digest(source) != digest(destination): raise RuntimeError('Conflicting runtime library: ' + relative)
            return
        shutil.copy2(source, destination)
        files.append({'path': relative, 'sha256': digest(destination), 'source': str(source)})
        origins.add(source.resolve())

    def libraries(binary):
        result = subprocess.run(['ldd', str(binary)], capture_output=True, text=True, check=True, timeout=10)
        if 'not found' in result.stdout: raise RuntimeError(result.stdout)
        for line in result.stdout.splitlines():
            match = re.search(r'^\s*(\S+) => (/\S+) ', line)
            if not match: continue
            name, path = match.groups()
            if name.startswith('libevdi'): raise RuntimeError('Application build must not link EVDI')
            if re.match(r'^(libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl)\.so', name): continue
            copy(path, 'lib/' + name)

    for name in ('remoteos-host', 'remoteos-host-app', 'remoteos-desktop-shell'):
        source = (args.build / name).resolve()
        copy(source, 'bin/' + name); libraries(source)
    if wallet:
        for source, relative, expected in wallet[1]:
            (output / relative).parent.mkdir(parents=True, exist_ok=True)
            copy(source, relative)
            if digest(output / relative) != expected:
                raise RuntimeError('Private wallet payload changed during packaging: ' + relative)
        # Deliberately do not call libraries(): the private backend must not be
        # copied into the desktop-wide lib/ directory or replace installed Qt.
    copy(root / 'scripts/remoteos-independent-desktop.py', 'bin/remoteos-independent-desktop.py')
    for name in ('Xtigervnc', 'openbox', 'xterm', 'dbus-run-session', 'dbus-update-activation-environment', 'pipewire', 'pipewire-pulse', 'wireplumber'):
        source = shutil.which(name)
        if not source: raise RuntimeError('Missing independent desktop runtime: ' + name)
        copy(source, 'bin/' + name); libraries(source)
    for directory in ('pipewire-0.3', 'spa-0.2', 'wireplumber-0.5'):
        source = Path('/usr/lib/x86_64-linux-gnu') / directory
        if not source.is_dir(): raise RuntimeError('Missing audio runtime: ' + str(source))
        for plugin in source.rglob('*.so'):
            relative = 'lib/' + directory + '/' + str(plugin.relative_to(source))
            (output / relative).parent.mkdir(parents=True, exist_ok=True)
            copy(plugin, relative); libraries(plugin)
    for directory in ('pipewire', 'wireplumber'):
        source = Path('/usr/share') / directory
        shutil.copytree(source, output / 'share' / directory, symlinks=False)
    themes = root / '.deps/usr/share/themes/Clearlooks'
    if not themes.is_dir(): themes = Path('/usr/share/themes/Clearlooks')
    shutil.copytree(themes, output / 'share/themes/Clearlooks', symlinks=False)
    for package in ('tigervnc-standalone-server', 'openbox', 'libobrender32', 'libobt2'):
        source = root / '.deps/usr/share/doc' / package / 'copyright'
        if source.is_file(): shutil.copy2(source, output / 'licenses' / package)

    candidates = []
    for entry in os.environ.get('GST_PLUGIN_PATH', '').split(':'):
        if entry: candidates.append(Path(entry))
    standard = subprocess.check_output(['pkg-config', '--variable=pluginsdir', 'gstreamer-1.0'], text=True).strip()
    candidates.append(Path(standard))
    # An extracted dev SDK's .pc file may reference runtime plugins still
    # installed under the distribution's normal libdir.
    candidates.append(Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0'))
    plugins = ['jpeg','coreelements','app','udp','rtp','rtpmanager','dtls','srtp','libav',
               'videoparsersbad','videoconvertscale','opus','audioconvert','audioresample','xvimagesink']
    for name in plugins + ['pipewire']:
        filename = 'libgst' + name + '.so'
        source = next((directory / filename for directory in candidates if (directory / filename).is_file()), None)
        if source is None:
            raise RuntimeError('Required sender plugin not found: ' + filename)
        copy(source, 'lib/gstreamer-1.0/' + filename); libraries(source)

    fonts = root / 'output/hp2140/target/usr/share/fonts/ibm-plex'
    if fonts.is_dir():
        for font in fonts.glob('*.ttf'): copy(font, 'share/fonts/' + font.name)
        license_file = root / 'output/hp2140/target/usr/share/licenses/ibm-plex/LICENSE.txt'
        shutil.copy2(license_file, output / 'licenses/IBM-Plex-OFL.txt')
    (output / 'fonts.conf').write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><include>/etc/fonts/fonts.conf</include><dir prefix="relative">share/fonts</dir></fontconfig>
''')
    for source in sorted(origins):
        lookup = str(source)
        extracted = root / '.deps'
        if source.is_relative_to(extracted): lookup = '/' + str(source.relative_to(extracted))
        owner = subprocess.run(['dpkg-query', '-S', lookup], capture_output=True, text=True, timeout=5)
        if owner.returncode: continue
        package = owner.stdout.split(': ', 1)[0].split(':', 1)[0]
        target = output / 'licenses' / package
        for base in (root / '.deps', Path('/')):
            copyright_file = base / 'usr/share/doc' / package / 'copyright'
            if copyright_file.is_file() and not target.exists(): shutil.copy2(copyright_file, target)
    common = Path('/usr/share/common-licenses')
    if common.is_dir(): shutil.copytree(common, output / 'licenses/common-licenses', symlinks=False)
    shutil.copytree(root / 'third_party/kde', output / 'licenses/kde-virtual-output-protocol', symlinks=False)
    shutil.copytree(root / 'third_party/cpp-httplib', output / 'licenses/cpp-httplib', ignore=shutil.ignore_patterns('httplib.h'))
    shutil.copy2(root / 'docs/application-host.md', output / 'README.md')
    shutil.copy2(root / 'docs/native-display-0.1.10.md', output / 'native-display-0.1.10.md')
    shutil.copy2(root / 'docs/low-latency-0.1.11.md', output / 'low-latency-0.1.11.md')
    shutil.copy2(root / 'docs/smooth-udp-0.1.12.md', output / 'smooth-udp-0.1.12.md')
    shutil.copy2(root / 'docs/independent-desktop-0.1.13.md', output / 'independent-desktop-0.1.13.md')
    shutil.copy2(root / 'docs/fullscreen-plasma-0.1.14.md', output / 'fullscreen-plasma-0.1.14.md')
    shutil.copy2(root / 'docs/motion-udp-0.1.15.md', output / 'motion-udp-0.1.15.md')
    shutil.copy2(root / 'docs/desktop-session-0.1.16.md', output / 'desktop-session-0.1.16.md')
    launcher = output / 'Start-RemoteOS-Host.sh'
    launcher.write_text('''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$base/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$base/bin:$PATH"
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$base/lib/gstreamer-1.0"
export GST_REGISTRY_FORK=no
export FONTCONFIG_FILE="$base/fonts.conf"
exec "$base/bin/remoteos-host-app" "$@"
''')
    launcher.chmod(0o755)
    indexed = {entry['path']: entry for entry in files}
    files = [dict(indexed.get(str(path.relative_to(output)), {'path':str(path.relative_to(output)), 'source':'generated or license resource'}), sha256=digest(path))
             for path in sorted(output.rglob('*')) if path.is_file()]
    record = {'kind':'native-linux-application-host', 'build_system':Path('/etc/os-release').read_text(),
              'capture_backends':['independent-xvnc-nested-plasma-wayland-shm','independent-xvnc-openbox-shm',
                                  'kde-native-virtual-output-with-portal-grant','wayland-portals-pipewire','xorg-xtest'],
              'service_installed':False, 'custom_drivers_required':False, 'kernel_modules_packaged':False,
              'files':files, 'limitations':['Built for the recorded native distribution; build locally for Ubuntu 24.04.',
                                          'Independent desktop uses the same Unix account, private session bus/configuration and bundled userspace servers. Python3, system XKB/fonts and native applications are required.',
                                          'Default matching desktop requires installed KDE Plasma 6: kwin_wayland, plasmashell, kded6 and kscreen-doctor. Minimal Openbox is an explicit alternative; other desktop matching is not implemented.',
                                          'Existing desktop sharing needs a logged-in Xorg session or Wayland RemoteDesktop/ScreenCast portals.',
                                          'Native virtual output requires KDE Wayland, OpenGL compositing, kbuildsycoca6 and a PipeWire policy manager.',
                                          'Windows and sustained HP throughput are not qualified by this bundle.']}
    if wallet:
        record['private_wallet_bridge'] = {
            'kind': wallet[0]['kind'], 'source_version': wallet[0]['source_version'],
            'entrypoint': 'libexec/remoteos/kwallet-bridge/run-bridge',
            'manifest': 'sources/kwallet-bridge/manifest.json',
            'manifest_sha256': wallet[1][-1][2],
            'activation_scope': 'independent Plasma private session bus only',
            'system_packages_changed': False, 'browser_and_relocation_qualified': True}
    (output / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'output':str(output), 'files':len(files), 'status':'staged'}, indent=2))


if __name__ == '__main__':
    main()
