# HP Mini 2140 factory Wi-Fi and first boot

The HP Mini 2140 does not have one universal default wireless card. HP's QuickSpecs describe factory-configurable WLAN and list **Broadcom 4312G 802.11b/g** and **Broadcom 4322AGN 802.11a/b/g/draft-n** options. HP specifies a physical wireless on/off button with an indicator. The actual installed card must be identified, including replacements made since manufacture. The HP-authored document is available through this [QuickSpecs mirror, pages 1 and 5](https://media.dustin.eu/media/d200001001862644/2140-mini-note-productdatasheetbrochure.pdf).

| Factory option | Bands | HP's maximum advertised link rate |
|---|---|---|
| Broadcom 4322AGN, 802.11a/b/g/draft-n | 2.4 GHz and 5 GHz | 300 Mb/s |
| Broadcom 4312G, 802.11b/g | 2.4 GHz | 54 Mb/s |

These are radio link rates, not measured application throughput. The bands and
rates are listed in HP's [Worldwide QuickSpecs DA-13185, pages 4, 16 and 18](https://datacomp.sk/img.asp?attid=35666).

## Driver selection and limits

Match the PCI ID, chip name, PHY revision and core revision from the diagnostic report. Chip marketing names are not PCI device IDs.

| Observed hardware | Linux driver evidence | RemoteOS qualification status |
|---|---|---|
| `14e4:4315`, BCM4312, LP PHY | Linux Wireless lists b43 support | Required driver and firmware packaged; actual HP association/throughput untested |
| `14e4:432b`, BCM4322, N PHY revision 4 | Linux Wireless lists partial b43 support | Required driver and firmware packaged; actual HP association/throughput untested |
| `14e4:4322` or `14e4:432c`, BCM4322 variants | Linux Wireless lists b43 support | Identify the actual variant and qualify it separately |
| `14e4:4312` | Linux identifies this PCI ID as BCM4311, not BCM4312 | Do not infer the chipset from the final four PCI-ID digits |

The b43 documentation reports no 802.11n HT, aggregation or 40 MHz features, even on N-PHY cards. Therefore the factory 4322AGN's advertised 300 Mb/s rate is **not a RemoteOS performance claim**. N-PHY 5 GHz support requires a sufficiently recent kernel; the included 6.12 kernel satisfies that version requirement, but regulatory, board and interoperability checks remain necessary. These are upstream support classifications, not tests performed on this laptop. [Linux Wireless b43 documentation](https://wireless.docs.kernel.org/en/latest/en/users/drivers/b43.html)

The uncompressed 1366×768 I420 60 fps profile requires about 755 Mb/s before transport overhead and cannot use these factory wireless links. Use the MPEG-2 Wi-Fi profile; qualify the raw profile separately on the Gigabit Ethernet path. Even MPEG-2 plus a webcam requires measured throughput and latency, not just successful association.

## What the image includes

The resolved Linux 6.12.109 configuration and installed target were checked, rather than relying only on defconfig requests:

- `CONFIG_B43=m`, `CONFIG_B43_PHY_LP=y`, `CONFIG_B43_PHY_N=y`, `CONFIG_B43_SSB=y`, SSB PCI host/bridge support, cfg80211/mac80211, rfkill and HP WMI support.
- Installed `ssb`, `b43`, `mac80211`, `cfg80211` and `hp-wmi` modules, with PCI autoload aliases for `14e4:4315` and `14e4:432b`.
- Proprietary firmware extracted by Buildroot's `b43-fwcutter` package from pinned `broadcom-wl-5.100.138`. The b43 loader selects files by detected **core** revision and PHY type. Core 15/LP uses `ucode15.fw`, `lp0initvals15.fw` and `lp0bsinitvals15.fw`; core 16/N uses `ucode16_mimo.fw`, `n0initvals16.fw` and `n0bsinitvals16.fw`. All six files are present with valid headers and microcode lengths.
- Signed `wireless-regdb` 2026.09.03 regulatory data. Regulatory rules and hardware radio blocks remain enforced.

The mappings above follow the included [Linux b43 firmware loader](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/net/wireless/broadcom/b43/main.c?h=v6.12.109) and [SSB PCI bridge aliases](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/ssb/b43_pci_bridge.c?h=v6.12.109). The [Buildroot firmware recipe](https://gitlab.com/buildroot.org/buildroot/-/blob/2025.02.18/package/b43-firmware/b43-firmware.mk) marks this firmware proprietary and non-redistributable. The private diagnostic image is not permission to publish those blobs. brcmsmac/brcmfmac firmware is separate and does not replace b43's files.

Every image build runs `scripts/check-hp2140-wireless.py`, which verifies the configuration, modules, autoload aliases and firmware structure and prints per-file hashes. Its result is deliberately named `packaging-verified-hardware-unqualified`; it does not simulate a Wi-Fi radio.

## Startup without a terminal login

BusyBox init starts the graphical appliance directly. There is no getty, desktop login or terminal command required to launch setup. Root starts the cookie-authenticated X server; `remoteos-xsession` switches to UID 600 before starting the GTK client and browser. Browser sandboxing remains enabled.

The sequence is:

1. `S00state` finds the state partition on the boot disk or explicitly marks temporary state. `S10udev` discovers devices and loads their kernel modules.
2. `S41remoteos-network` starts a background network worker and returns immediately. DHCP, a missing adapter, an unavailable radio or failed supplicant startup cannot hold up the GTK screen. The worker checks every four seconds for physical interfaces and owns at most one supplicant and one DHCP child per interface. It retries failed children and late adapters, and stops children when their interface disappears. After Wi-Fi association changes, it releases/replaces the owned DHCP lease so changing access points does not leave an address from the previous subnet.
3. `S45tailscale` starts the persistent TUN service and grants the unprivileged client local operator access. Tailnet enrollment remains a separate on-device setup action.
4. `S91remoteos` starts the privileged power/device supervisor and graphical launcher. The launcher starts Xorg and the unprivileged client, restarts an exited session, and stops after three consecutive early exits. A session that ran for at least one minute receives a fresh retry budget. Unsupported X screensaver/DPMS extensions are nonfatal.

The network worker leaves hardware and software rfkill blocks in place. If the setup screen reports a disabled radio, use the HP wireless button or the applicable firmware setting; the worker retries afterward without requiring an OS reboot. It does not repair unsupported firmware or a physically faulty card. Ethernet is initialized independently.

`/run/remoteos/network-status.txt` reports service discovery/startup states, **not internet connectivity**. `/run/remoteos/graphical-status.txt` distinguishes starting, X-session startup, exit, stopped and retry exhaustion. Actual network association, DHCP/DNS and browser connectivity are verified by setup and qualification tests.

Graphics stderr is drained continuously while retaining at most the first 64 KiB per attempt in a private RAM file. After a session exits, one private `reports/latest-graphical-failure.txt` is replaced atomically on the state partition when available. It includes the exit result and bounded log. Graphics retry exhaustion also prints a recovery message to the local console. No interactive root shell is exposed. Hardware inventory is separately saved as `reports/latest-boot-hardware.txt`. Treat failure logs as private diagnostics and review them before sharing; retrieve only the needed reports from generic Linux recovery media, not the whole credential-bearing state partition.

## Actual laptop acceptance

Boot the updated image with the physical wireless radio enabled and confirm that the GTK setup appears without logging in. Record the image hash, PCI ID, b43 PHY/core revision, loaded firmware and rfkill state. Scan, join an open network and WPA2-Personal network, obtain DHCP/DNS, complete captive-portal and Google/Tailscale browser enrollment, and reboot to check persistence. Repeat with the radio initially disabled, switch it on from setup, and confirm recovery without reboot. Record loss/reassociation and combined webcam/media throughput. A missing adapter, partial-driver failure or rejected browser remains an explicit qualification failure.

The startup fixtures cover blocked radio recovery, late adapter discovery, failed supplicant startup, independent Ethernet, duplicate prevention, DHCP lease replacement after changing access points, child cleanup and bounded graphics retries/logging. They pass under the actual image's i386 BusyBox shell with mocked devices. Neither these tests nor the BIOS/QEMU boot test establish physical wireless compatibility.
