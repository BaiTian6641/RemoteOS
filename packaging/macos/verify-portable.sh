#!/bin/bash
set -euo pipefail
app=$1
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
python3 "$repo/packaging/macos/bundle-runtime.py" "$app" --verify
"$app/Contents/MacOS/remoteos-runtime-check"
"$app/Contents/MacOS/remoteos-pacing-check"
python3 "$repo/scripts/check-native-host.py" "$app/Contents/MacOS/remoteos-host"
"$app/Contents/MacOS/remoteos-host" --help
codesign --verify --deep --strict "$app"
# Launch Services follows the same bundle launch path as Finder. A receipt
# proves applicationDidFinishLaunching created a visible window, not merely
# that a background process survived. No privacy prompt or capture is started.
receipt=$(mktemp -d)/startup.json
open -W -n "$app" --args --startup-check "$receipt"
python3 - "$receipt" <<'PY'
import json,sys
from pathlib import Path
x=json.loads(Path(sys.argv[1]).read_text())
assert x['status']=='passed' and x['visible_window'] and not x['permissions_requested'],x
print(json.dumps(x))
PY
rm -rf "$(dirname "$receipt")"
echo 'Universal dependency closure, MPEG-2, TLS, generated host sharing and Launch Services UI passed.'
