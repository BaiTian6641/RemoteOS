# Optional HP 2140 BCM4322 `wl` diagnostic

This optional profile tests Broadcom STA `wl` against the actual HP 2140
BCM4322 PCI device `14e4:432b`. It is separate from the ordinary `b43` image.
Successful compilation does not establish throughput, stability, regulatory
behavior, suspend/resume support or hardware qualification.

## Source, compatibility and licensing

The package pins Broadcom 6.30.223.271 and Debian revision 32. Both download
hashes come from Debian's signed source descriptor. It applies the complete
Debian patch series, then builds the maintained `amd64/` wrapper with the
**unmodified i386 binary**, matching Debian's packaging method. That directory
name does not select a 64-bit binary: the kernel's `CONFIG_X86_32` selects
`wlc_hybrid.o_i386`. The final module must report ELF32, Intel 80386, the exact
6.12.109 Atom kernel ABI, and `MIXED/Proprietary` licensing.

The CFG80211 variant was cross-compiled against the image's existing Linux
6.12.109 configuration. Its actual module dependency is `cfg80211`; it does not
require enabling LIB80211 or changing the kernel. The package refuses other
kernel versions and configurations with built-in competing drivers. No DKMS,
compiler or downloaded kernel module runs on the laptop. It never bypasses
module version checks, relabels the binary GPL, or changes kernel security
settings. The proprietary/out-of-tree module taints the kernel in the normal
way. The HP image boots using legacy BIOS and has no Secure Boot enrollment
step; signing the distributed image is a separate integrity mechanism.

The Broadcom binary license permits use/integration and redistribution of the
complete, unmodified binary only for Broadcom hardware. Distribution must carry
the agreement and copyright notices, and its terms include distributor
indemnification. The original binary license and Debian copyright file are
installed under `/usr/share/licenses/remoteos-broadcom-sta/`. This permission is
not an open-source license for the binary. Wrapper sources retain their
ISC-style notices; Debian packaging is MIT/public-domain; Debian's compatibility
header retains its GPL-2.0 notice. `make legal-info` retains the pinned source
archives and license files. This package records those terms rather than
claiming general legal clearance for a larger product distribution.

Debian's series includes compatibility code for kernels newer than this image,
including a per-object objtool workaround for the old binary. RemoteOS does
not use that as authorization to disable kernel-wide mitigations or to qualify
a newer kernel. The qualified build remains the existing 32-bit 6.12.109 target.

Primary sources:

- [Broadcom supported devices and integration instructions](https://sources.debian.org/src/broadcom-sta/6.30.223.271-32/README.txt/)
- [Pinned Debian descriptor and source hashes](https://ftp.debian.org/debian/pool/non-free/b/broadcom-sta/broadcom-sta_6.30.223.271-32.dsc)
- [Debian shared-wrapper/i386-binary packaging](https://sources.debian.org/src/broadcom-sta/6.30.223.271-32/debian/rules/)
- [Broadcom and Debian license terms](https://sources.debian.org/src/broadcom-sta/6.30.223.271-32/debian/copyright/)
- [Kernel b43 status and missing 802.11n HT features](https://wireless.docs.kernel.org/en/latest/en/users/drivers/b43.html)
- [Kernel taint flags](https://docs.kernel.org/admin-guide/tainted-kernels.html)

## Build and boot selection

Use a separate output directory for a clean diagnostic build:

```sh
REMOTEOS_OUTPUT_DIR="$PWD/output/hp2140-wl" \
REMOTEOS_DEFCONFIG=hp2140_wl_defconfig scripts/build-image.sh configure
REMOTEOS_OUTPUT_DIR="$PWD/output/hp2140-wl" \
REMOTEOS_DEFCONFIG=hp2140_wl_defconfig scripts/build-image.sh build
```

The ordinary `hp2140_defconfig` remains the default. Without an explicit output
override, the wl profile uses `output/hp2140-wl` and the base profile uses
`output/hp2140`, so selecting wl does not reconfigure the base build. Preserve any previous
release image before reconfiguring a reused output tree. The build wrapper
rejects a profile/config mismatch until `configure` is run. Returning a reused
tree to the base profile removes the optional module, init script, notices and
profile marker during post-build; its normal GRUB menu is restored. Switching
back to the wl profile in the same tree requires
`make remoteos-broadcom-sta-reinstall` as well as a normal image build, because
Buildroot does not notice manual removal of installed package files. A fresh
output directory avoids this incremental-package caveat.

The wl image offers:

1. **Broadcom wl diagnostic** (default): `remoteos.wifi_driver=wl`.
2. **b43 Wi-Fi recovery**: `remoteos.wifi_driver=b43`.
3. **wl serial diagnostic** with verbose console and 115200-baud serial output.

Before udev coldplug, `S08remoteos-wifi-driver` creates only a volatile
`/run/modprobe.d/` rule. An explicit wl selection blocks `b43`, `b43legacy`,
`brcmsmac`, `ssb` and `bcma` for that boot; b43 recovery blocks wl. With no
selection, the optional image also defaults to b43. These blacklists are never
installed in the base profile. An unsupported device keeps wl blocked. A failed
wl load leaves an explicit report and continues into local setup/Ethernet; it
does not silently fall back or unload an already bound driver. Reboot and choose
the recovery entry to change drivers safely.

The HP's wired adapter uses `sky2`, so suppressing the conflicting Broadcom bus
modules in a wl boot preserves that Ethernet recovery route. This profile is
not a general blacklist recipe for other laptops.

## Evidence to collect on the HP

Driver selection, result, bound PCI driver, kernel and package revision are
written to `/run/remoteos/wifi-driver-status` and
`/state/reports/latest-wifi-driver.txt`. Module-load output is preserved in
`latest-wifi-driver-load.txt`. Existing hardware reports identify the actual
network-interface driver and negotiated connection. Compare each driver using
the same AP, band, position and RemoteOS media profile:

- Boot each choice and verify only the chosen driver owns BCM4322.
- Connect through the first-boot UX; verify open/WPA2-Personal, reconnection,
  captive portal and Tailscale behavior.
- Measure LAN throughput independently, then run motion/audio for 30 minutes.
  Record actual bitrate, retries, decode time, useful FPS and late frame drops.
- Confirm b43 recovery still boots and Ethernet remains usable after a failed
  wl association. Do not interpret a successful module load as a WLAN speedup.

No real network interface is manipulated by the package's fixture tests.
