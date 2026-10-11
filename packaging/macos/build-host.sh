#!/bin/bash
# Portable universal app. The SDK is required only on the build Mac.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=$(cat "$repo/VERSION")
build=${REMOTEOS_MAC_BUILD:-"$repo/build-macos"}
output=${REMOTEOS_MAC_OUTPUT:-"$repo/output/RemoteOS-Host-$version.app"}
sdk=${REMOTEOS_GSTREAMER_ROOT:-/Library/Frameworks/GStreamer.framework/Versions/1.0}
[[ $(uname -s) == Darwin ]] || { echo 'Use a Mac with Xcode command-line tools.' >&2; exit 2; }
[[ ! -e "$output" ]] || { echo 'Choose a new output path.' >&2; exit 2; }
export PKG_CONFIG_PATH="$sdk/lib/pkgconfig"
export PATH="$sdk/bin:$PATH"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 '-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64' \
 "-DOPENSSL_ROOT_DIR=$sdk" "-DOPENSSL_INCLUDE_DIR=$sdk/include" \
 "-DOPENSSL_SSL_LIBRARY=$sdk/lib/libssl.dylib" "-DOPENSSL_CRYPTO_LIBRARY=$sdk/lib/libcrypto.dylib" \
 -DREMOTEOS_WITH_MEDIA=ON -DREMOTEOS_WITH_GTK=OFF -DREMOTEOS_WITH_BROWSER=OFF \
 -DREMOTEOS_WITH_SERVICE_HOST=OFF -DREMOTEOS_WITH_WAYLAND_HOST=OFF -DREMOTEOS_WITH_DESKTOP_HOST=ON \
 -DREMOTEOS_BUILD_HOST_APP=ON -DREMOTEOS_BUILD_TESTS=OFF
cmake --build "$build" --target remoteos-host remoteos-host-app remoteos-runtime-check remoteos-pacing-check -j4
mkdir -p "$(dirname "$output")"
cp -R "$build/remoteos-host-app.app" "$output"
cp "$build/remoteos-host" "$build/remoteos-runtime-check" "$build/remoteos-pacing-check" "$output/Contents/MacOS/"
/usr/libexec/PlistBuddy -c 'Add :NSPrincipalClass string NSApplication' "$output/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :NSScreenCaptureUsageDescription string RemoteOS shares your desktop with your paired client.' "$output/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Add :LSMinimumSystemVersion string 13.0' "$output/Contents/Info.plist"
mkdir -p "$output/Contents/Resources"
cp "$repo/packaging/native-sdk.lock.json" "$repo/VERSION" "$repo/docs/portable-builds.md" "$output/Contents/Resources/"
for entry in asio/LICENSE_1_0.txt nlohmann/LICENSE.MIT cpp-httplib/LICENSE; do
 mkdir -p "$output/Contents/Resources/licenses/$(dirname "$entry")"
 cp "$repo/third_party/$entry" "$output/Contents/Resources/licenses/$entry"
done
cp "$repo/third_party/NOTICE.md" "$output/Contents/Resources/licenses/NOTICE.md"
python3 "$repo/packaging/macos/bundle-runtime.py" "$output" --sdk "$sdk"
# Sign relocated binaries before enclosing bundles. CI uses ad-hoc signing.
export REMOTEOS_MAC_SIGN_IDENTITY=${REMOTEOS_MAC_SIGN_IDENTITY:--}
python3 - "$output" "$repo" <<'SIGN'
import os,sys,subprocess
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[2])/'packaging/macos'))
from importlib import import_module
m=import_module('bundle-runtime');app=Path(sys.argv[1]);identity=os.environ['REMOTEOS_MAC_SIGN_IDENTITY']
args=['codesign','--force','--sign',identity]
if identity!='-':args+=['--options','runtime','--timestamp']
for path in sorted((p for p in app.rglob('*') if m.macho(p)),key=lambda p:len(p.parts),reverse=True):subprocess.run(args+[str(path)],check=True)
for path in [app/'Contents/Frameworks/GStreamer.framework',app]:subprocess.run(args+[str(path)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
SIGN
printf 'Portable universal application: %s\n' "$output"
