#pragma once
#include <gdk/gdkx.h>
#include <X11/extensions/Xfixes.h>

namespace remoteos::client {
// The appliance owns its X server. Hide at the root as well as at GTK video
// windows, including Intel's explicitly installed root software cursor.
class SessionCursor {
public:
  explicit SessionCursor(GdkDisplay *display, bool own_root = true) : display_(display), own_root_(own_root) {
    int event = 0, error = 0;
    available_ = GDK_IS_X11_DISPLAY(display_) &&
                 XFixesQueryExtension(GDK_DISPLAY_XDISPLAY(display_), &event, &error);
  }
  ~SessionCursor() { set_hidden(false); }
  void set_hidden(bool hidden) {
    if (!own_root_) {
      hidden_ = hidden;
      apply_window_cursor();
      return;
    }
    if (!available_ || hidden == hidden_) return;
    auto *native = GDK_DISPLAY_XDISPLAY(display_);
    if (hidden) XFixesHideCursor(native, DefaultRootWindow(native));
    else XFixesShowCursor(native, DefaultRootWindow(native));
    hidden_ = hidden;
    XFlush(native);
  }
  void set_window(GdkWindow* window) {
    window_ = window;
    if (!own_root_) apply_window_cursor();
  }
private:
  void apply_window_cursor() {
    if (!window_) return;
    auto* cursor = hidden_ ? gdk_cursor_new_for_display(display_, GDK_BLANK_CURSOR) : nullptr;
    gdk_window_set_cursor(window_, cursor);
    if (cursor) g_object_unref(cursor);
  }
  GdkDisplay *display_;
  GdkWindow* window_ = nullptr;
  bool own_root_;
  bool available_ = false, hidden_ = false;
};
}
