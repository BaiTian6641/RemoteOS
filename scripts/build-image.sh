#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
defconfig=${REMOTEOS_DEFCONFIG:-hp2140_defconfig}
case "$defconfig" in
  hp2140_defconfig|hp2140_wl_defconfig) ;;
  *) echo 'REMOTEOS_DEFCONFIG must be hp2140_defconfig or hp2140_wl_defconfig' >&2; exit 2 ;;
esac
source_dir=$("$repo/scripts/bootstrap-buildroot.sh")
default_output="$repo/output/hp2140"
[[ "$defconfig" != hp2140_wl_defconfig ]] || default_output="$repo/output/hp2140-wl"
output_dir=${REMOTEOS_OUTPUT_DIR:-"$default_output"}
mkdir -p -- "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd)
# Go's initial C bootstrap otherwise hardcodes /var/tmp, which is unavailable in
# restricted builders. Keep all temporary output inside the selected build tree.
mkdir -p -- "$output_dir/tmp"
export TMPDIR="$output_dir/tmp"
# Some upstream configure scripts autodetect an installed ccache even when
# Buildroot's own ccache option is off. Keep their cache writable and isolated.
export CCACHE_DIR="$output_dir/ccache"
export CCACHE_TEMPDIR="$output_dir/tmp"
exec 9>"$output_dir/.remoteos-build.lock"
flock -n 9 || { echo "Another RemoteOS build/configuration holds $output_dir" >&2; exit 1; }
args=(-C "$source_dir" "O=$output_dir" "BR2_EXTERNAL=$repo/buildroot")
# An explicit profile change requires configure; never label an old config as
# the requested profile simply because a different environment variable is set.
if [[ -f "$output_dir/.config" && ${1:-build} != configure && ${1:-build} != check-config ]]; then
  configured=hp2140_defconfig
  if grep -qx BR2_PACKAGE_REMOTEOS_BROADCOM_STA=y "$output_dir/.config"; then configured=hp2140_wl_defconfig; fi
  if [[ "$configured" != "$defconfig" ]]; then
    echo "Configured profile is $configured; run configure with REMOTEOS_DEFCONFIG=$defconfig first" >&2
    exit 2
  fi
fi
case ${1:-build} in
  configure) exec make "${args[@]}" "$defconfig" ;;
  check-config)
    make "${args[@]}" "$defconfig"
    make "${args[@]}" olddefconfig
    exec python3 "$repo/scripts/check-buildroot-config.py" "$repo/buildroot/configs/$defconfig" "$output_dir/.config"
    ;;
  rebuild-app)
    make "${args[@]}" "BR2_JLEVEL=${REMOTEOS_JOBS:-4}" remoteos-reconfigure
    exec make "${args[@]}" "BR2_JLEVEL=${REMOTEOS_JOBS:-4}" all
    ;;
  source|legal-info|build)
    [[ -f "$output_dir/.config" ]] || make "${args[@]}" "$defconfig"
    target=${1:-build}; [[ "$target" == build ]] && target=all
    exec make "${args[@]}" "BR2_JLEVEL=${REMOTEOS_JOBS:-4}" "$target"
    ;;
  *) echo 'Usage: build-image.sh [configure|check-config|source|legal-info|build|rebuild-app]' >&2; exit 2 ;;
esac
