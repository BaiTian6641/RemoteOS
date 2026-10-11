#!/bin/sh
set -eu
package=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
grep -qx BR2_PACKAGE_REMOTEOS_BROADCOM_STA=y "$BR2_CONFIG"
test -s "$TARGET_DIR/lib/modules/6.12.109/extra/wl.ko"
test -x "$TARGET_DIR/etc/init.d/S08remoteos-wifi-driver"
test -s "$TARGET_DIR/usr/share/licenses/remoteos-broadcom-sta/Broadcom-STA-LICENSE.txt"
# Buildroot removes /usr/share/doc during target-finalize. Restore this small
# provenance bundle afterwards, so the documentation promised by the package
# actually accompanies the image on both clean and incremental builds.
install -D -m 0644 "$package/README.md" "$TARGET_DIR/usr/share/doc/remoteos-broadcom-sta/README.md"
install -m 0644 "$package/remoteos-broadcom-sta.hash" "$TARGET_DIR/usr/share/doc/remoteos-broadcom-sta/source-hashes"
install -m 0644 "$BUILD_DIR/remoteos-broadcom-sta-6.30.223.271-32/debian/patches/series" \
    "$TARGET_DIR/usr/share/doc/remoteos-broadcom-sta/debian-patch-series"
# Run after the normal board post-build script, in the separate wl output only.
install -D -m 0644 "$package/grub.cfg" "$TARGET_DIR/boot/grub/grub.cfg"
printf '%s\n' 'Optional BCM4322 wl diagnostic; b43 recovery available in GRUB; hardware qualification pending.' \
    > "$TARGET_DIR/usr/share/remoteos/wifi-driver-profile"
