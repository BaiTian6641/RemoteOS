#pragma once
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <X11/extensions/XInput2.h>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace remoteos::client {
// Preserve subpixel motion without introducing an absolute cursor position.
// XI2's values are the X server's processed deltas (including libinput's
// touchpad handling), rather than the device-specific raw_values.
class RelativeDeltas {
  double x_ = 0, y_ = 0;
public:
  void reset() noexcept { x_ = y_ = 0; }
  std::pair<int, int> add(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 32767 || std::abs(y) > 32767) return {};
    x_ += x; y_ += y;
    const auto dx = int(x_), dy = int(y_);
    x_ -= dx; y_ -= dy;
    return {dx, dy};
  }
};

class RelativeMouseCapture {
  ::Display* display_ = nullptr;
  int opcode_ = 0;
  guint watch_ = 0;
  GdkSeat* seat_ = nullptr;
  RelativeDeltas deltas_;
  std::map<int, bool> relative_sources_;
  std::function<void(int,int)> send_;
  bool relative_source(int id) {
    const auto known = relative_sources_.find(id);
    if (known != relative_sources_.end()) return known->second;
    int count = 0; auto* devices = XIQueryDevice(display_, id, &count);
    bool x = false, y = false;
    for (int i = 0; devices && i < count; ++i)
      for (int j = 0; j < devices[i].num_classes; ++j) {
        auto* klass = devices[i].classes[j];
        if (klass->type != XIValuatorClass) continue;
        auto* axis = reinterpret_cast<XIValuatorClassInfo*>(klass);
        if (axis->number == 0) x = axis->mode == XIModeRelative;
        if (axis->number == 1) y = axis->mode == XIModeRelative;
      }
    if (devices) XIFreeDeviceInfo(devices);
    return relative_sources_[id] = x && y;
  }
  void drain() {
    while (display_ && XPending(display_)) {
      XEvent event; XNextEvent(display_, &event);
      if (event.type != GenericEvent || event.xcookie.extension != opcode_ || !XGetEventData(display_, &event.xcookie)) continue;
      if (event.xcookie.evtype == XI_HierarchyChanged) relative_sources_.clear();
      else if (event.xcookie.evtype == XI_RawMotion) {
        auto* raw = static_cast<XIRawEvent*>(event.xcookie.data);
        if (relative_source(raw->sourceid)) {
          double x = 0, y = 0; auto* value = raw->valuators.values;
          for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis)
            if (XIMaskIsSet(raw->valuators.mask, axis)) {
              if (axis == 0) x = *value;
              if (axis == 1) y = *value;
              ++value;
            }
          const auto [dx,dy] = deltas_.add(x,y);
          if (dx || dy) send_(dx,dy);
        }
      }
      XFreeEventData(display_, &event.xcookie);
    }
  }
public:
  ~RelativeMouseCapture() { stop(); }
  bool active() const noexcept { return display_ != nullptr; }
  void start(GtkWidget* viewer, std::function<void(int,int)> send) {
    if (active()) return;
    auto* window = gtk_widget_get_window(viewer);
    if (!window || !GDK_IS_X11_DISPLAY(gdk_window_get_display(window)))
      throw std::runtime_error("Relative mouse needs X11 or XWayland.");
    display_ = XOpenDisplay(gdk_display_get_name(gdk_window_get_display(window)));
    if (!display_) throw std::runtime_error("Cannot open relative mouse input.");
    try {
      int event_base, error_base, major = 2, minor = 1;
      if (!XQueryExtension(display_, "XInputExtension", &opcode_, &event_base, &error_base) ||
          XIQueryVersion(display_, &major, &minor) != Success || major < 2 || (major == 2 && minor < 1))
        throw std::runtime_error("Relative mouse needs XInput 2.1 or newer.");
      unsigned char bits[XIMaskLen(XI_LASTEVENT)]{}, hierarchy[XIMaskLen(XI_LASTEVENT)]{};
      XISetMask(bits, XI_RawMotion); XISetMask(hierarchy, XI_HierarchyChanged);
      XIEventMask masks[]{{XIAllMasterDevices, sizeof(bits), bits}, {XIAllDevices, sizeof(hierarchy), hierarchy}};
      if (XISelectEvents(display_, DefaultRootWindow(display_), masks, 2) != Success)
        throw std::runtime_error("Cannot subscribe to relative mouse input.");
      XSync(display_, False);
      // Ignore motion queued before ownership changed.
      while (XPending(display_)) { XEvent old; XNextEvent(display_, &old); }
      auto* seat = gdk_display_get_default_seat(gdk_window_get_display(window));
      auto* cursor = gdk_cursor_new_for_display(gdk_window_get_display(window), GDK_BLANK_CURSOR);
      const auto result = gdk_seat_grab(seat, window, GDK_SEAT_CAPABILITY_POINTER, FALSE, cursor, nullptr, nullptr, nullptr);
      if (cursor) g_object_unref(cursor);
      if (result != GDK_GRAB_SUCCESS) throw std::runtime_error("Mouse is in use by another application. Resume to try again.");
      seat_ = seat; send_ = std::move(send); deltas_.reset();
      watch_ = g_unix_fd_add(ConnectionNumber(display_), G_IO_IN,
        +[](gint, GIOCondition, gpointer p) -> gboolean {
          static_cast<RelativeMouseCapture*>(p)->drain(); return G_SOURCE_CONTINUE;
        }, this);
    } catch (...) { stop(); throw; }
  }
  void stop() noexcept {
    if (watch_) { g_source_remove(watch_); watch_ = 0; }
    if (seat_) { gdk_seat_ungrab(seat_); seat_ = nullptr; }
    if (display_) { XCloseDisplay(display_); display_ = nullptr; }
    relative_sources_.clear(); deltas_.reset(); send_ = {};
  }
};
}
