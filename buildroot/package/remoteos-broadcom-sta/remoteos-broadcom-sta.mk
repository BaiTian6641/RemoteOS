################################################################################
#
# Optional HP 2140 Broadcom STA diagnostic driver; base image is unchanged.
#
################################################################################

REMOTEOS_BROADCOM_STA_VERSION = 6.30.223.271-32
REMOTEOS_BROADCOM_STA_SOURCE = broadcom-sta_6.30.223.271.orig.tar.xz
REMOTEOS_BROADCOM_STA_SITE = https://ftp.debian.org/debian/pool/non-free/b/broadcom-sta
REMOTEOS_BROADCOM_STA_EXTRA_DOWNLOADS = $(REMOTEOS_BROADCOM_STA_SITE)/broadcom-sta_$(REMOTEOS_BROADCOM_STA_VERSION).debian.tar.xz
REMOTEOS_BROADCOM_STA_LICENSE = Broadcom-STA (binary), ISC (wrapper), GPL-2.0 (compatibility header), MIT (Debian packaging)
REMOTEOS_BROADCOM_STA_LICENSE_FILES = amd64/lib/LICENSE.txt amd64/Makefile amd64/src/include/lib80211.h debian/copyright
REMOTEOS_BROADCOM_STA_DEPENDENCIES = host-kmod
REMOTEOS_BROADCOM_STA_MODULE_SUBDIRS = amd64
REMOTEOS_BROADCOM_STA_MODULE_MAKE_OPTS = API=CFG80211 INSTALL_MOD_DIR=extra

define REMOTEOS_BROADCOM_STA_EXTRACT_DEBIAN
	$(TAR) -C $(@D) -xf $(REMOTEOS_BROADCOM_STA_DL_DIR)/broadcom-sta_$(REMOTEOS_BROADCOM_STA_VERSION).debian.tar.xz
endef
REMOTEOS_BROADCOM_STA_POST_EXTRACT_HOOKS += REMOTEOS_BROADCOM_STA_EXTRACT_DEBIAN

define REMOTEOS_BROADCOM_STA_APPLY_DEBIAN
	$(APPLY_PATCHES) $(@D) $(@D)/debian/patches
	# Debian shares its maintained wrapper between both binary architectures.
	cp $(@D)/i386/lib/wlc_hybrid.o_shipped $(@D)/amd64/lib/wlc_hybrid.o_i386
endef
REMOTEOS_BROADCOM_STA_POST_PATCH_HOOKS += REMOTEOS_BROADCOM_STA_APPLY_DEBIAN

define REMOTEOS_BROADCOM_STA_CONFIGURE_CMDS
	test "$(LINUX_VERSION_PROBED)" = 6.12.109
	grep -qx CONFIG_X86_32=y $(LINUX_DIR)/.config
	# Built-in competing drivers cannot be excluded by the boot selector.
	$(foreach symbol,CFG80211 B43 SSB BCMA BRCMSMAC,grep -qx CONFIG_$(symbol)=m $(LINUX_DIR)/.config$(sep))
endef

define REMOTEOS_BROADCOM_STA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(REMOTEOS_BROADCOM_STA_PKGDIR)/S08remoteos-wifi-driver $(TARGET_DIR)/etc/init.d/S08remoteos-wifi-driver
	$(INSTALL) -D -m 0644 $(@D)/amd64/lib/LICENSE.txt $(TARGET_DIR)/usr/share/licenses/remoteos-broadcom-sta/Broadcom-STA-LICENSE.txt
	$(INSTALL) -m 0644 $(@D)/debian/copyright $(TARGET_DIR)/usr/share/licenses/remoteos-broadcom-sta/debian-copyright
	$(INSTALL) -m 0644 $(@D)/amd64/Makefile $(TARGET_DIR)/usr/share/licenses/remoteos-broadcom-sta/wrapper-Makefile-with-ISC-notice
	$(INSTALL) -m 0644 $(@D)/amd64/src/include/lib80211.h $(TARGET_DIR)/usr/share/licenses/remoteos-broadcom-sta/lib80211-compatibility-header
	$(INSTALL) -D -m 0644 $(REMOTEOS_BROADCOM_STA_PKGDIR)/README.md $(TARGET_DIR)/usr/share/doc/remoteos-broadcom-sta/README.md
	$(INSTALL) -m 0644 $(@D)/debian/patches/series $(TARGET_DIR)/usr/share/doc/remoteos-broadcom-sta/debian-patch-series
	$(INSTALL) -m 0644 $(REMOTEOS_BROADCOM_STA_PKGDIR)/remoteos-broadcom-sta.hash $(TARGET_DIR)/usr/share/doc/remoteos-broadcom-sta/source-hashes
endef

$(eval $(kernel-module))

define REMOTEOS_BROADCOM_STA_CHECK_MODULE
	$(HOST_DIR)/bin/modinfo -F license $(@D)/amd64/wl.ko | grep -qx MIXED/Proprietary
	$(HOST_DIR)/bin/modinfo -F vermagic $(@D)/amd64/wl.ko | grep -q '^6\.12\.109 .*ATOM '
	$(HOST_DIR)/bin/modinfo -F depends $(@D)/amd64/wl.ko | grep -qx cfg80211
	cmp $(@D)/i386/lib/wlc_hybrid.o_shipped $(@D)/amd64/lib/wlc_hybrid.o_i386
endef
REMOTEOS_BROADCOM_STA_POST_BUILD_HOOKS += REMOTEOS_BROADCOM_STA_CHECK_MODULE

$(eval $(generic-package))
