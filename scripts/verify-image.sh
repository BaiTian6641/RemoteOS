#!/usr/bin/env bash
set -euo pipefail
[[ $# == 3 ]] || { echo 'Usage: verify-image.sh IMAGE SIGNATURE TRUSTED_PUBLIC_KEY' >&2; exit 2; }
[[ -f "$1" && -f "$2" && -f "$3" ]] || { echo 'Missing image, signature or trusted public key' >&2; exit 1; }
exec openssl dgst -sha256 -verify "$3" -signature "$2" "$1"
