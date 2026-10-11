include $(sort $(wildcard $(BR2_EXTERNAL_REMOTEOS_PATH)/package/*/*.mk))

# Camera enumeration needs v4l-utils, but its optional broadcast-text gconv
# modules depend on additional glibc converters outside this minimal appliance.
LIBV4L_CONF_OPTS += -Dgconv=disabled

# HP 2140's 945GME uses KMS. The optional i810/i815 UMS backend references
# lazily loaded VBE/VGA/shadow modules, which cannot resolve under full RELRO's
# immediate binding. Omit that unrelated backend; keep SNA/UXA and hardening.
XDRIVER_XF86_VIDEO_INTEL_CONF_OPTS += --enable-kms-only
