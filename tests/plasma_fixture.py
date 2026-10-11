#!/usr/bin/env python3
"""A native Wayland app inside the private Plasma integration fixture."""
import json
import os
from pathlib import Path
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, Gdk

path = Path(sys.argv[1])
os.umask(0o077)
window = Gtk.Window(title='RemoteOS private Plasma input test')
window.set_default_size(640, 480)
window.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.KEY_PRESS_MASK)
box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
entry = Gtk.Entry()
entry.set_placeholder_text('Remote keyboard input')
button = Gtk.Button(label='Native Wayland app: click, then type')
box.pack_start(entry, False, False, 0)
box.pack_start(button, True, True, 0)
window.add(box)
observed = {'keys': [], 'clicks': [], 'width': 0, 'height': 0, 'painted_frames': 0, 'backend': window.get_display().__gtype__.name}

def update(*unused):
    allocation = window.get_allocation()
    observed.update(width=allocation.width, height=allocation.height, text=entry.get_text())
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(observed))
    temporary.replace(path)
    return True

def key(widget, event):
    observed['keys'].append(event.keyval)
    update()

def click(widget, event):
    observed['clicks'].append([event.x, event.y, event.button])
    update()

def activated(widget):
    observed['clicks'].append(['button-activated'])
    entry.grab_focus()
    update()

def painted(*unused):
    observed['painted_frames'] += 1

def animate():
    button.set_label('Native Wayland app: click, then type · frame ' + str(observed['painted_frames']))
    return True

window.connect('key-press-event', key)
window.connect('button-press-event', click)
button.connect('clicked', activated)
entry.connect('key-press-event', key)
window.connect('destroy', Gtk.main_quit)
window.connect_after('draw', painted)
window.show_all()
window.maximize()
GLib.timeout_add(100, update)
GLib.timeout_add(33, animate)
Gtk.main()
