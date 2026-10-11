#pragma once
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

namespace remoteos::client {
// The appliance has no window manager. GTK's fullscreen call is a request to
// a WM, so also set the native bounds there, after local chrome is hidden.
// Managed desktops retain their normal EWMH fullscreen handling.
inline void fullscreen_window(GtkWidget* widget, GdkMonitor* monitor) {
  if (!monitor) return;
  auto* display = gtk_widget_get_display(widget);
  auto* screen = gtk_widget_get_screen(widget);
  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);
  int index = 0;
  for (int i = 0; i < gdk_display_get_n_monitors(display); ++i)
    if (gdk_display_get_monitor(display, i) == monitor) { index = i; break; }
  gtk_window_set_decorated(GTK_WINDOW(widget), FALSE);
  gtk_window_fullscreen_on_monitor(GTK_WINDOW(widget), screen, index);
  gtk_window_move(GTK_WINDOW(widget), geometry.x, geometry.y);
  gtk_window_resize(GTK_WINDOW(widget), geometry.width, geometry.height);
  auto* native = gtk_widget_get_window(widget);
  if (native && GDK_IS_X11_DISPLAY(display) &&
      !gdk_x11_screen_supports_net_wm_hint(
          screen, gdk_atom_intern_static_string("_NET_WM_STATE_FULLSCREEN")))
    gdk_window_move_resize(native, geometry.x, geometry.y,
                           geometry.width, geometry.height);
}
}
