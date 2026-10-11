#!/bin/sh
set -eu
board=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Buildroot does not uninstall disabled packages during an incremental build.
# Remove only the optional wl package's owned files when returning to the base
# profile. The normal GRUB menu below replaces its diagnostic boot entries.
if ! grep -qx BR2_PACKAGE_REMOTEOS_BROADCOM_STA=y "$BR2_CONFIG"; then
    rm -f "$TARGET_DIR/etc/init.d/S08remoteos-wifi-driver" \
        "$TARGET_DIR/usr/share/remoteos/wifi-driver-profile"
    rm -rf "$TARGET_DIR/usr/share/licenses/remoteos-broadcom-sta" \
        "$TARGET_DIR/usr/share/doc/remoteos-broadcom-sta"
    if [ -f "$TARGET_DIR/lib/modules/6.12.109/extra/wl.ko" ]; then
        rm -f "$TARGET_DIR/lib/modules/6.12.109/extra/wl.ko"
        "$HOST_DIR/sbin/depmod" -a -b "$TARGET_DIR" 6.12.109
    fi
fi
python3 "$BR2_EXTERNAL_REMOTEOS_PATH/../scripts/check-buildroot-config.py" \
    "$board/linux.config" "$BUILD_DIR/linux-6.12.109/.config"
python3 "$BR2_EXTERNAL_REMOTEOS_PATH/../scripts/check-hp2140-wireless.py" \
    "$TARGET_DIR" "$BUILD_DIR/linux-6.12.109/.config"
python3 "$BR2_EXTERNAL_REMOTEOS_PATH/../scripts/check-xorg-driver-symbols.py" "$TARGET_DIR"
python3 "$BR2_EXTERNAL_REMOTEOS_PATH/../scripts/check-intel-cursor.py" "$TARGET_DIR"
for driver in evdev libinput; do
    python3 "$BR2_EXTERNAL_REMOTEOS_PATH/../scripts/check-xorg-driver-symbols.py" \
        "$TARGET_DIR" --module "/usr/lib/xorg/modules/input/${driver}_drv.so"
done
# libinput's upstream rules also replace the keyboard/mouse drivers. This
# appliance enables it only for touchpads through 30-remoteos-touchpad.conf;
# evdev continues to own keyboards and ordinary mice, including hotplugged USB.
rm -f "$TARGET_DIR/usr/share/X11/xorg.conf.d/40-libinput.conf"
test -s "$TARGET_DIR/etc/X11/xorg.conf.d/30-remoteos-touchpad.conf"
test -x "$TARGET_DIR/usr/bin/xinput"
test -x "$TARGET_DIR/usr/bin/remoteos-cursor-info"
# Keep native setup indicators available on an otherwise minimal GTK image.
for asset in usr/share/mime/mime.cache usr/share/icons/Adwaita/index.theme \
    usr/share/icons/Adwaita/cursors/left_ptr \
    usr/share/icons/hicolor/index.theme \
    usr/share/icons/Adwaita/16x16/ui/pan-end-symbolic.symbolic.png \
    usr/share/icons/Adwaita/16x16/ui/pan-down-symbolic.symbolic.png \
    usr/share/icons/Adwaita/16x16/ui/checkbox-checked-symbolic.symbolic.png; do
    [ -s "$TARGET_DIR/$asset" ] || {
        echo "Native setup runtime asset is missing: $asset" >&2
        exit 1
    }
done
install -D -m 0644 "$board/grub.cfg" "$TARGET_DIR/boot/grub/grub.cfg"
cp "$TARGET_DIR/lib/grub/i386-pc/boot.img" "$BINARIES_DIR/boot.img"
ln -sfn /state "$TARGET_DIR/data"
mkdir -p "$TARGET_DIR/state" "$TARGET_DIR/etc/remoteos" "$TARGET_DIR/run"
# Internal application libraries are static; Buildroot otherwise defaults CMake
# libraries to shared, while this product installs its executable entry points.
for executable in "$TARGET_DIR"/usr/bin/remoteos-* "$TARGET_DIR/usr/bin/remoteosctl" "$TARGET_DIR/usr/sbin/remoteos-supervisor"; do
    [ -f "$executable" ] || continue
    if readelf -d "$executable" 2>/dev/null | grep -E '\(NEEDED\).*libremoteos_' >/dev/null; then
        echo "Application was built with uninstalled shared libraries: $executable" >&2
        exit 1
    fi
done
# ALSA mixer state is persisted separately from the immutable image.
rm -rf "$TARGET_DIR/var/lib/alsa"
ln -s /state/alsa "$TARGET_DIR/var/lib/alsa"
# Mutable identifiers and resolver data must never require a writable root.
rm -f "$TARGET_DIR/etc/resolv.conf" "$TARGET_DIR/etc/machine-id"
ln -s /run/resolv.conf "$TARGET_DIR/etc/resolv.conf"
ln -s /state/machine-id "$TARGET_DIR/etc/machine-id"
# BusyBox keeps DHCP address/route handling, but its temporary resolver output
# must not overwrite openresolv's combined network/Tailscale configuration.
dhcp_script="$TARGET_DIR/usr/share/udhcpc/default.script"
if grep -F 'RESOLV_CONF="/etc/resolv.conf"' "$dhcp_script" >/dev/null; then
    sed -i 's|RESOLV_CONF="/etc/resolv.conf"|RESOLV_CONF="/run/udhcpc-$interface.resolv.conf"|' "$dhcp_script"
else
    grep -F 'RESOLV_CONF="/run/udhcpc-$interface.resolv.conf"' "$dhcp_script" >/dev/null
fi
# Remove obsolete optional DVB converters after an incremental libv4l rebuild.
# Fresh builds disable those modules through the external-tree Meson option.
rm -f "$TARGET_DIR/usr/lib/gconv/ARIB-STD-B24.so" "$TARGET_DIR/usr/lib/gconv/EN300-468-TAB00.so"
if ! grep -q '^BR2_TOOLCHAIN_GLIBC_GCONV_LIBS_COPY=y$' "$BR2_CONFIG"; then
    rm -f "$TARGET_DIR/usr/lib/gconv/gconv-modules"
fi
# The stock Xorg service would race our cookie-authenticated xinit launcher.
# Only S91remoteos owns display :0 and its unprivileged client session.
rm -f "$TARGET_DIR/etc/init.d/S40xorg"
# Control starts from our authenticated application; do not export USB/IP on boot.
rm -f "$TARGET_DIR/etc/init.d/S50usbip" "$TARGET_DIR/etc/init.d/S50usbipd"
# Provision the supervisor capability marker only for the verified local-only build.
usbip_source="$BUILD_DIR/usbip-6.12.6/tools/usb/usbip/src/usbipd.c"
if ! grep -F 'do_getaddrinfo("127.0.0.1", family)' "$usbip_source" >/dev/null || \
   ! test -x "$TARGET_DIR/usr/sbin/usbip" || \
   ! test -x "$TARGET_DIR/usr/sbin/usbipd" || \
   ! strings "$TARGET_DIR/usr/sbin/usbipd" | grep -F 'Bind to IPv4 loopback (RemoteOS default).' >/dev/null; then
    echo 'USB/IP loopback policy is missing from the installed daemon; refusing image.' >&2
    exit 1
fi
mkdir -p "$TARGET_DIR/usr/share/remoteos"
printf '%s\n' 'usbipd 6.12.6: raw listener restricted to 127.0.0.1 by RemoteOS package patch' \
    > "$TARGET_DIR/usr/share/remoteos/usbip-loopback-only"
# The userspace HTTP bridge is statically included in the client/supervisor.
# Ship its pinned MIT notice and provenance inside every appliance image.
install -D -m 0644 "$BR2_EXTERNAL_REMOTEOS_PATH/../third_party/cpp-httplib/LICENSE" \
    "$TARGET_DIR/usr/share/licenses/remoteos/cpp-httplib/LICENSE"
install -D -m 0644 "$BR2_EXTERNAL_REMOTEOS_PATH/../third_party/cpp-httplib/manifest.json" \
    "$TARGET_DIR/usr/share/licenses/remoteos/cpp-httplib/manifest.json"
