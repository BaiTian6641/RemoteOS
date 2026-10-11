#!/usr/bin/env python3
"""Record only display/session fields from a private D-Bus activation."""
import json
import os
from pathlib import Path
import sys

from gi.repository import Gio, GLib

if os.environ.get('REMOTEOS_PRIVATE_ACTIVATION_TEST') != '1':
    raise RuntimeError('This fixture requires an explicitly private test session')
keys = ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'XDG_CURRENT_DESKTOP',
        'DESKTOP_SESSION', 'XDG_SESSION_TYPE', 'KDE_FULL_SESSION',
        'KDE_SESSION_VERSION', 'QT_QPA_PLATFORM', 'GDK_BACKEND',
        'XDG_RUNTIME_DIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME')
Path(sys.argv[1]).write_text(json.dumps({key: os.environ.get(key) for key in keys}))
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
              'org.freedesktop.DBus', 'RequestName',
              GLib.Variant('(su)', ('org.remoteos.TestActivation', 0)),
              None, Gio.DBusCallFlags.NONE, 3000, None)
GLib.MainLoop().run()
