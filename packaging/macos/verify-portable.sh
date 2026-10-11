#!/bin/bash
set -euo pipefail
app=$1
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
python3 "$repo/packaging/macos/bundle-runtime.py" "$app" --verify
"$app/Contents/MacOS/remoteos-runtime-check"
"$app/Contents/MacOS/remoteos-host" --help
codesign --verify --deep --strict "$app"
# Start only the setup GUI; sharing and privacy permissions are not requested.
"$app/Contents/MacOS/remoteos-host-app" &
remoteos_ui_pid=$!
trap 'kill "$remoteos_ui_pid" 2>/dev/null || true' EXIT
sleep 3
kill -0 "$remoteos_ui_pid"
kill "$remoteos_ui_pid"
wait "$remoteos_ui_pid" || [[ $? == 143 ]]
trap - EXIT
echo 'Universal dependency closure, MPEG-2, TLS and host GUI startup passed.'
