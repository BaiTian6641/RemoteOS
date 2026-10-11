REMOTEOS_FONTS_VERSION = 383c681f015ed2626e919ec4e3cca16ccc204e9d
REMOTEOS_FONTS_SITE = https://raw.githubusercontent.com/IBM/plex/$(REMOTEOS_FONTS_VERSION)/IBM-Plex-Sans/fonts/complete/ttf
REMOTEOS_FONTS_SOURCE = IBMPlexSans-Regular.ttf
REMOTEOS_FONTS_EXTRA_DOWNLOADS = $(REMOTEOS_FONTS_SITE)/IBMPlexSans-SemiBold.ttf \
	https://raw.githubusercontent.com/IBM/plex/$(REMOTEOS_FONTS_VERSION)/LICENSE.txt
REMOTEOS_FONTS_LICENSE = OFL-1.1
REMOTEOS_FONTS_LICENSE_FILES = LICENSE.txt

define REMOTEOS_FONTS_EXTRACT_CMDS
	cp $(REMOTEOS_FONTS_DL_DIR)/IBMPlexSans-Regular.ttf $(@D)/
	cp $(REMOTEOS_FONTS_DL_DIR)/IBMPlexSans-SemiBold.ttf $(@D)/
	cp $(REMOTEOS_FONTS_DL_DIR)/LICENSE.txt $(@D)/
endef

define REMOTEOS_FONTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/IBMPlexSans-Regular.ttf $(TARGET_DIR)/usr/share/fonts/ibm-plex/IBMPlexSans-Regular.ttf
	$(INSTALL) -D -m 0644 $(@D)/IBMPlexSans-SemiBold.ttf $(TARGET_DIR)/usr/share/fonts/ibm-plex/IBMPlexSans-SemiBold.ttf
	$(INSTALL) -D -m 0644 $(@D)/LICENSE.txt $(TARGET_DIR)/usr/share/licenses/ibm-plex/LICENSE.txt
endef

$(eval $(generic-package))
