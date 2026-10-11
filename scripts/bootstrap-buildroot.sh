#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=2025.02.18
hash=e38ad1df6ea0479fff6419a87a64535d02131674d463be154a763ef725b55321
cache=${REMOTEOS_CACHE_DIR:-"$repo/.cache"}
archive="$cache/buildroot-downloads/buildroot-$version.tar.xz"
source_dir="$cache/buildroot-$version"
mkdir -p -- "$(dirname -- "$archive")"
exec 8>"$cache/.remoteos-bootstrap.lock"
flock 8
if [[ ! -f "$archive" ]]; then
  curl --fail --location --retry 3 "https://buildroot.org/downloads/buildroot-$version.tar.xz" -o "$archive.part"
  mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --status || { echo 'Buildroot checksum mismatch' >&2; exit 1; }
if [[ ! -d "$source_dir" ]]; then
  tar -xf "$archive" -C "$cache"
fi
# Patch stamps are per file so additive, independently pinned backports can be
# applied to an existing cache without silently changing an applied patch.
for patch_file in "$repo"/buildroot/patches/*.patch; do
  name=$(basename -- "$patch_file")
  patch_hash=$(sha256sum "$patch_file" | cut -d' ' -f1)
  stamp="$source_dir/.remoteos-$name.sha256"
  if [[ "$name" == 0001-webkitgtk-2.54.1.patch && -f "$source_dir/.remoteos-patch" && ! -f "$stamp" ]]; then
    cp -- "$source_dir/.remoteos-patch" "$stamp"
  fi
  if [[ -f "$stamp" ]]; then
    [[ $(cat "$stamp") == "$patch_hash" ]] || { echo "Cached $name differs; remove only the extracted Buildroot directory and retry." >&2; exit 1; }
  else
    patch --directory="$source_dir" --strip=1 --batch < "$patch_file" >&2
    printf '%s\n' "$patch_hash" > "$stamp"
  fi
done
printf '%s\n' "$source_dir"
