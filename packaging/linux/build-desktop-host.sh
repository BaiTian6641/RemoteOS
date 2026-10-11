#!/usr/bin/env bash
# Build the ordinary-user application. Does not install or load host drivers.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${REMOTEOS_DESKTOP_BUILD_DIR:-"$repo/build-desktop"}
cmake -S "$repo" -B "$build" -G Ninja \
  -DREMOTEOS_WITH_GTK=ON -DREMOTEOS_WITH_MEDIA=ON \
  -DREMOTEOS_WITH_BROWSER=OFF -DREMOTEOS_WITH_DESKTOP_HOST=ON \
  -DREMOTEOS_WITH_WAYLAND_HOST=ON \
  -DREMOTEOS_WITH_SERVICE_HOST=OFF -DREMOTEOS_BUILD_HOST_APP=ON
cmake --build "$build" --target remoteos-host remoteos-host-app -j "${REMOTEOS_JOBS:-4}"
printf 'Open %s/remoteos-host-app in your Wayland or Xorg login session.\n' "$build"
