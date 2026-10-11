#!/bin/sh
set -eu
board=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export E2FSPROGS_FAKE_TIME="${SOURCE_DATE_EPOCH:?Buildroot reproducible timestamp is required}"
./support/scripts/genimage.sh -c "$board/genimage.cfg"
cd "$BINARIES_DIR"
sha256sum remoteos-hp2140.img > remoteos-hp2140.img.sha256
cp "$BR2_CONFIG" remoteos-buildroot.config
