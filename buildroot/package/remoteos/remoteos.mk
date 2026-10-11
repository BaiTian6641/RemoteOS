REMOTEOS_VERSION = 0.1.0
REMOTEOS_SITE = $(BR2_EXTERNAL_REMOTEOS_PATH)/..
REMOTEOS_SITE_METHOD = local
# The local repository contains build output and private provisioning caches.
# Never let generic legal-info recursively archive it; publish reviewed source
# separately when the project license/release policy has been established.
REMOTEOS_REDISTRIBUTE = NO
REMOTEOS_DEPENDENCIES = openssl libgtk3 gstreamer1 gst1-plugins-base webkitgtk \
	xlib_libX11 xlib_libXcursor xlib_libXfixes xlib_libXi
REMOTEOS_CONF_OPTS = -DBUILD_SHARED_LIBS=OFF -DREMOTEOS_WITH_GTK=ON -DREMOTEOS_WITH_MEDIA=ON \
	-DREMOTEOS_WITH_BROWSER=ON -DREMOTEOS_BUILD_TESTS=OFF \
	-DREMOTEOS_WITH_DESKTOP_HOST=OFF -DREMOTEOS_BUILD_HOST_APP=OFF
# Never recursively copy cross-build outputs back into the local-source package.
REMOTEOS_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude=.cache --exclude=output \
	--exclude=build --exclude=build-* --exclude=.deps --exclude=.git --exclude=.agents --exclude=.aws --exclude=.codex
$(eval $(cmake-package))
