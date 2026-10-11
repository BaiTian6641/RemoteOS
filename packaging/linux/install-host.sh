#!/bin/sh
# Explicit installation only. Does not restart LightDM or enable the service.
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run this installer as root.' >&2; exit 1; fi
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || [ ! -x "$1" ]; then echo 'Usage: install-host.sh /path/to/remoteos-host [/path/to/remoteos-audio-session]' >&2; exit 1; fi
audio_helper=${2:-$(dirname -- "$1")/remoteos-audio-session}
if [ ! -x "$audio_helper" ]; then echo 'Build remoteos-audio-session before installing the host.' >&2; exit 1; fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for program in xrandr modprobe python3; do command -v "$program" >/dev/null; done
modinfo evdi >/dev/null
if [ "$(modinfo -F version evdi)" != '1.14.11' ]; then
  echo 'RemoteOS requires the pinned EVDI 1.14.11 module. Follow docs/hosts.md before installation.' >&2
  exit 1
fi
module_kernel=$(modinfo -F vermagic evdi | awk '{print $1}')
if [ "$module_kernel" != "$(uname -r)" ]; then
  echo 'The EVDI module does not match the running host kernel.' >&2
  exit 1
fi
install -d -m 0755 /usr/libexec /etc/remoteos /etc/modules-load.d /etc/modprobe.d /etc/X11/xorg.conf.d /etc/lightdm/lightdm.conf.d
install -d -m 0700 /var/lib/remoteos
install -m 0755 "$1" /usr/bin/remoteos-host
install -m 0755 "$audio_helper" /usr/libexec/remoteos-audio-session
install -m 0755 "$source_dir/remoteos-display-layout" /usr/libexec/remoteos-display-layout
install -m 0644 "$source_dir/remoteos-host.service" /etc/systemd/system/remoteos-host.service
install -m 0644 "$source_dir/remoteos.modules-load.conf" /etc/modules-load.d/remoteos.conf
install -m 0644 "$source_dir/remoteos.modprobe.conf" /etc/modprobe.d/remoteos.conf
install -m 0644 "$source_dir/90-remoteos-xorg.conf" /etc/X11/xorg.conf.d/90-remoteos.conf
install -m 0644 "$source_dir/90-remoteos-lightdm.conf" /etc/lightdm/lightdm.conf.d/90-remoteos.conf
if [ ! -e /etc/remoteos/host.json ]; then install -m 0600 "$source_dir/host.json.example" /etc/remoteos/host.json; fi
if [ ! -e /etc/remoteos/displays.json ]; then install -m 0600 "$source_dir/displays.json.example" /etc/remoteos/displays.json; fi
systemctl daemon-reload
echo 'Installed. Configure /etc/remoteos/displays.json and signed EVDI module, then enable remoteos-host explicitly. A maintenance reboot is required for Xorg and kernel-module changes.'
