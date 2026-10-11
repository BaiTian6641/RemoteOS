# Source from the repository root; all changes apply only to this shell.
REMOTEOS_DEV_PREFIX="$(pwd)/.deps/usr"
export PKG_CONFIG_PATH="$REMOTEOS_DEV_PREFIX/lib/x86_64-linux-gnu/pkgconfig:$REMOTEOS_DEV_PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$(pwd)/.deps/evdi-1.14.11/library:$REMOTEOS_DEV_PREFIX/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$REMOTEOS_DEV_PREFIX/bin:$PATH"
export GST_PLUGIN_PATH="$REMOTEOS_DEV_PREFIX/lib/x86_64-linux-gnu/gstreamer-1.0"
export GST_REGISTRY="$(pwd)/.cache/gstreamer-registry.bin"
