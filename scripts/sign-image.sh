#!/usr/bin/env bash
# Offline release step. The signing key is supplied by the release operator.
set -euo pipefail
[[ $# == 3 ]] || { echo 'Usage: sign-image.sh IMAGE PRIVATE_KEY OUTPUT_SIGNATURE' >&2; exit 2; }
[[ -f "$1" && -f "$2" ]] || { echo 'Image or signing key does not exist' >&2; exit 1; }
[[ ! -e "$3" ]] || { echo 'Refusing to overwrite an existing signature' >&2; exit 1; }
umask 077
openssl dgst -sha256 -sign "$2" -out "$3" "$1"
echo 'Detached signature created; distribute the public key through a trusted channel.'
