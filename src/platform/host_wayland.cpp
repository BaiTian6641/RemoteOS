#include "remoteos/host_platform.hpp"
#include "display_config.hpp"
#include "kde_virtual_output.hpp"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <pipewire/pipewire.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace remoteos::platform {
namespace {
constexpr auto bus_name = "org.freedesktop.portal.Desktop";
constexpr auto portal_path = "/org/freedesktop/portal/desktop";
constexpr auto desktop_iface = "org.freedesktop.portal.RemoteDesktop";
constexpr auto capture_iface = "org.freedesktop.portal.ScreenCast";
using Clock = std::chrono::steady_clock;
struct VariantDelete { void operator()(GVariant *p) const { if (p) g_variant_unref(p); } };
using Variant = std::unique_ptr<GVariant, VariantDelete>;
std::string token() {
  auto *uuid = g_uuid_string_random(); std::string result = "remoteos_" + std::string(uuid); g_free(uuid);
  result.erase(std::remove(result.begin(), result.end(), '-'), result.end()); return result;
}
GVariant *options(const std::string& handle = "") {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  if (!handle.empty()) g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(handle.c_str()));
  return g_variant_builder_end(&b);
}
std::runtime_error dbus_error(const char *operation, GError *error) {
  const auto text = std::string(operation) + ": " + (error ? error->message : "desktop portal unavailable");
  g_clear_error(&error); return std::runtime_error(text);
}
struct Source {
  unsigned node = 0, logical_width = 0, logical_height = 0;
  std::uint64_t serial = 0;
  DisplayConfig pixels;
  GstElement *pipeline = nullptr, *sink = nullptr;
  GstSample *latest = nullptr;
  bool negotiated = false;
  bool virtual_monitor = false;
  Clock::time_point next{};
  ~Source() {
    if (latest) gst_sample_unref(latest);
    if (pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); }
    if (sink) gst_object_unref(sink);
  }
};

// The entire grant (D-Bus session, restricted PipeWire FD, and capture
// pipelines) belongs to the visible application's sharing lifetime. Client
// disconnect stops frame delivery/input but keeps that grant for reconnects.
// The optional KDE adapter owns an HP-sized virtual output. Only a node
// explicitly included in the portal grant can enter the capture pipeline.
class WaylandHost final : public HostPlatform {
public:
  WaylandHost() : cancel_(g_cancellable_new()) {}
  ~WaylandHost() override {
    stop(); closing_ = true; cancel_prepare();
    if (worker_.joinable()) worker_.join();
    g_object_unref(cancel_);
  }
  void select_desktop_source(DesktopSource source) override {
    if (worker_.joinable()) throw std::logic_error("Stop sharing before changing the display source");
    selection_ = source;
  }
  void set_virtual_display_size(unsigned width, unsigned height) override {
    if (worker_.joinable()) throw std::logic_error("Stop sharing before changing the virtual display size");
    if (!((width == 1024 && height == 576) || (width == 1366 && height == 768) || (width == 1920 && height == 1080)))
      throw std::invalid_argument("Choose 1024x576, 1366x768 or 1920x1080 for the native virtual display");
    virtual_width_ = width; virtual_height_ = height;
  }
  void prepare(const std::string& parent_window) override {
    if (worker_.joinable()) throw std::logic_error("Wayland sharing is already prepared");
    std::promise<void> ready; auto result = ready.get_future();
    worker_ = std::thread([this, parent_window, promise = std::move(ready)]() mutable {
      context_ = g_main_context_new(); g_main_context_push_thread_default(context_);
      bool announced = false;
      try {
        if (selection_ == DesktopSource::kde_native_output) {
          native_output_ = std::make_unique<KdeVirtualOutput>(virtual_width_, virtual_height_);
          const auto deadline = Clock::now() + std::chrono::seconds(10);
          while (!native_output_->ready()) {
            check_cancel(); native_output_->pump();
            if (Clock::now() > deadline) throw std::runtime_error("KDE did not create the native virtual display");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
          }
        }
        open_portal(parent_window);
        open_capture();
        const auto timeout = Clock::now() + std::chrono::seconds(15);
        while (!all_negotiated()) {
          check_cancel(); pump(); collect();
          if (!session_error().empty()) throw std::runtime_error(session_error());
          if (Clock::now() > timeout) throw std::runtime_error("No screen frames from PipeWire; check the desktop's screencast backend");
          std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        prepared_ = true; promise.set_value(); announced = true;
        while (!closing_ && !g_cancellable_is_cancelled(cancel_)) {
          pump(); collect();
          if (g_dbus_connection_is_closed(bus_)) throw std::runtime_error("The desktop session bus disconnected");
          if (!session_error().empty()) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
      } catch (const std::exception& e) {
        fail(e.what());
        if (!announced) promise.set_exception(std::current_exception());
      }
      prepared_ = false;
      cleanup();
      g_main_context_pop_thread_default(context_); g_main_context_unref(context_); context_ = nullptr;
    });
    result.get();
  }
  void cancel_prepare() noexcept override { g_cancellable_cancel(cancel_); }
  Capabilities capabilities() const override {
    Capabilities c; c.capture = prepared_; c.input = prepared_;
    c.diagnostics.emplace_back("Wayland application sharing uses consented RemoteDesktop/ScreenCast portals and PipeWire; no custom drivers");
    c.diagnostics.emplace_back("Screen and keyboard/pointer permission must both be granted; login and locked desktops remain compositor-controlled");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      c.virtual_displays = prepared_ && std::any_of(sources_.begin(), sources_.end(), [](const auto& s) { return s->virtual_monitor; });
    }
    if (selection_ == DesktopSource::virtual_output)
      c.diagnostics.emplace_back("The portal creates one independent virtual display and controls its desktop resolution; Plasma 6.5 uses 1920x1080");
    if (selection_ == DesktopSource::kde_native_output)
      c.diagnostics.emplace_back("KDE native virtual output uses the chosen HP panel resolution at scale 1; only its consented PipeWire node is captured");
    const auto error = session_error(); if (!error.empty()) c.diagnostics.push_back(error);
    return c;
  }
  std::string session_error() const override { std::lock_guard<std::mutex> lock(error_mutex_); return error_; }
  std::vector<DisplayConfig> desktop_displays() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DisplayConfig> result;
    if (prepared_) for (const auto& source : sources_) result.push_back(source->pixels);
    return result;
  }
  void match_desktop(const std::vector<DisplayConfig>& displays) override {
    detail::validate_displays(displays);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!prepared_ || displays.size() > sources_.size()) throw std::runtime_error("Select enough screens before starting sharing");
    // PipeWire caps describe captured pixels. Changing them does not prove
    // that the compositor changed its logical desktop or window placement.
    // The standard portal has no request to modeset a selected output.
    for (std::size_t i = 0; i < displays.size(); ++i) {
      const auto& source = sources_[i]; const auto& desired = displays[i];
      if (source->logical_width == desired.width && source->logical_height == desired.height) continue;
      const auto actual = std::to_string(source->logical_width) + "x" + std::to_string(source->logical_height);
      const auto requested = std::to_string(desired.width) + "x" + std::to_string(desired.height);
      if (selection_ == DesktopSource::kde_native_output)
        throw std::runtime_error("The native virtual desktop is " + actual + ", but the client panel is " + requested +
          ". Stop sharing, choose that exact resolution in RemoteOS Host, and start sharing again. Native mode does not scale the desktop.");
      if (source->virtual_monitor)
        throw std::runtime_error("The Wayland portal created a virtual desktop at " + actual +
          ", but the HP requested " + requested + ". This portal cannot change that resolution (Plasma 6.5 creates 1920x1080). "
          "Clear Match host resolution on the HP to use host-side video scaling, or use a desktop backend with the required virtual display size.");
      throw std::runtime_error("Wayland physical monitor resolution is " + actual + ", but the HP requested " + requested +
        ". Change the host display settings, or select a virtual display in the RemoteOS Host app. "
        "Clear Match host resolution on the HP to use host-side video scaling.");
    }
  }
  void start(const std::vector<DisplayConfig>& displays, FrameCallback callback) override {
    stop(); detail::validate_displays(displays);
    if (!prepared_ || !callback) throw std::runtime_error("Approve screen and remote-input access in the desktop permission dialog first");
    std::lock_guard<std::mutex> lock(mutex_);
    if (displays.size() > sources_.size()) throw std::runtime_error("The desktop permission dialog selected fewer monitors than requested");
    if (selection_ == DesktopSource::kde_native_output) {
      // Native mode remains exact even if an older client unchecked matching.
      const auto& s = *sources_.front(); const auto& d = displays.front();
      if (s.logical_width != d.width || s.logical_height != d.height || s.pixels.width != d.width || s.pixels.height != d.height)
        throw std::runtime_error("Native virtual display size differs from the client panel; choose the matching HP resolution in RemoteOS Host");
    }
    logical_ = displays; callback_ = std::move(callback);
    for (auto& source : sources_) source->next = {};
  }
  void stop() noexcept override {
    release_all(); std::lock_guard<std::mutex> lock(mutex_); callback_ = {}; logical_.clear();
  }
  void inject(const InputEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!prepared_ || logical_.empty()) throw std::runtime_error("Wayland sharing permission is no longer active");
    try {
      switch (event.kind) {
      case InputKind::key:
        if (!event.key || event.key > 0x2ff) throw std::invalid_argument("Unsupported Linux keyboard code");
        // The compositor owns auto-repeat. Repeated downs must not leave an
        // unmatched press in its virtual keyboard state.
        if (event.down && keys_.count(event.key)) return;
        notify("NotifyKeyboardKeycode", g_variant_new("(o@a{sv}iu)", session_.c_str(), options(), int(event.key), unsigned(event.down)));
        if (event.down) keys_.insert(event.key); else keys_.erase(event.key);
        break;
      case InputKind::button:
        if (event.key < 0x110 || event.key > 0x116) throw std::invalid_argument("Unsupported Linux pointer button");
        if (event.down && buttons_.count(event.key)) return;
        notify("NotifyPointerButton", g_variant_new("(o@a{sv}iu)", session_.c_str(), options(), int(event.key), unsigned(event.down)));
        if (event.down) buttons_.insert(event.key); else buttons_.erase(event.key);
        break;
      case InputKind::absolute_pointer: {
        bool found = false;
        for (std::size_t i = 0; i < logical_.size(); ++i) {
          const auto& d = logical_[i]; const auto& s = *sources_[i];
          if (event.x < d.x || event.x >= d.x + int(d.width) || event.y < d.y || event.y >= d.y + int(d.height)) continue;
          const double x = double(event.x - d.x) * (s.logical_width - 1) / std::max(1u, d.width - 1);
          const double y = double(event.y - d.y) * (s.logical_height - 1) / std::max(1u, d.height - 1);
          notify("NotifyPointerMotionAbsolute", g_variant_new("(o@a{sv}udd)", session_.c_str(), options(), s.node, x, y));
          found = true; break;
        }
        if (!found) throw std::invalid_argument("Pointer is outside the shared monitor layout");
        break;
      }
      case InputKind::relative_pointer:
        notify("NotifyPointerMotion", g_variant_new("(o@a{sv}dd)", session_.c_str(), options(), double(event.delta_x), double(event.delta_y))); break;
      case InputKind::wheel:
        // Portal axes follow evdev/Wayland: negative vertical steps scroll up.
        if (event.delta_y) notify("NotifyPointerAxisDiscrete", g_variant_new("(o@a{sv}ui)", session_.c_str(), options(), 0u, -std::clamp(event.delta_y, -32, 32)));
        if (event.delta_x) notify("NotifyPointerAxisDiscrete", g_variant_new("(o@a{sv}ui)", session_.c_str(), options(), 1u, std::clamp(event.delta_x, -32, 32)));
        break;
      case InputKind::secure_attention:
        throw std::invalid_argument("The Wayland desktop controls secure attention sequences");
      }
    } catch (const std::runtime_error& error) { fail(error.what()); throw; }
  }
  void release_all() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prepared_ && bus_) {
      // Attempt every release even if a previous request fails. Session.Close
      // tears down the compositor's virtual input when permission is revoked.
      for (auto key : keys_) try { notify("NotifyKeyboardKeycode", g_variant_new("(o@a{sv}iu)", session_.c_str(), options(), int(key), 0u)); } catch (...) {}
      for (auto button : buttons_) try { notify("NotifyPointerButton", g_variant_new("(o@a{sv}iu)", session_.c_str(), options(), int(button), 0u)); } catch (...) {}
    }
    keys_.clear(); buttons_.clear();
  }
private:
  void fail(const std::string& error) { std::lock_guard<std::mutex> lock(error_mutex_); if (error_.empty()) error_ = error; }
  void check_cancel() { if (closing_ || g_cancellable_is_cancelled(cancel_)) throw std::runtime_error("Desktop sharing was canceled"); }
  void pump() {
    if (native_output_) native_output_->pump();
    while (g_main_context_pending(context_)) g_main_context_iteration(context_, FALSE);
  }
  Variant call(const char *iface, const char *method, GVariant *parameters, const GVariantType *reply, const char *path = portal_path) {
    GError *error = nullptr;
    auto *result = g_dbus_connection_call_sync(bus_, bus_name, path, iface, method, parameters, reply,
                                             G_DBUS_CALL_FLAGS_NONE, 5000, cancel_, &error);
    if (!result) throw dbus_error(method, error);
    return Variant(result);
  }
  void notify(const char *method, GVariant *parameters) {
    // Input notifications have no return data. Queue them in connection
    // order without a synchronous portal round trip under the capture lock.
    // The consented session and its Closed/owner-change signals still govern
    // input lifetime; every event remains checked by the portal backend.
    auto *message = g_dbus_message_new_method_call(bus_name, portal_path, desktop_iface, method);
    g_dbus_message_set_body(message, parameters);
    g_dbus_message_set_flags(message, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    GError *error = nullptr;
    const bool sent = g_dbus_connection_send_message(bus_, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, &error);
    g_object_unref(message);
    if (!sent) throw dbus_error(method, error);
  }
  unsigned property(const char *iface, const char *name) {
    auto result = call("org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"));
    GVariant *boxed = nullptr; g_variant_get(result.get(), "(v)", &boxed); Variant value(boxed);
    if (!g_variant_is_of_type(value.get(), G_VARIANT_TYPE_UINT32)) throw std::runtime_error("Unexpected desktop portal property type");
    return g_variant_get_uint32(value.get());
  }
  struct Response { std::string path; unsigned code = 2; Variant results; };
  Variant request(const char *iface, const char *method, const std::string& handle, GVariant *parameters) {
    std::string sender = g_dbus_connection_get_unique_name(bus_); sender.erase(0, 1);
    std::replace(sender.begin(), sender.end(), '.', '_');
    Response response{"/org/freedesktop/portal/desktop/request/" + sender + "/" + handle, 2, {}};
    auto subscription = g_dbus_connection_signal_subscribe(bus_, bus_name, "org.freedesktop.portal.Request", "Response", nullptr, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection *, const gchar *, const gchar *path, const gchar *, const gchar *, GVariant *value, gpointer data) {
        auto *r = static_cast<Response *>(data); if (r->path != path) return;
        GVariant *results = nullptr; g_variant_get(value, "(u@a{sv})", &r->code, &results); r->results.reset(results);
      }, &response, nullptr);
    try {
      auto result = call(iface, method, parameters, G_VARIANT_TYPE("(o)"));
      const char *path = nullptr; g_variant_get(result.get(), "(&o)", &path); response.path = path;
      const auto deadline = Clock::now() + std::chrono::minutes(3);
      while (!response.results) {
        check_cancel(); pump();
        if (!session_error().empty()) throw std::runtime_error(session_error());
        if (Clock::now() > deadline) throw std::runtime_error("Desktop sharing permission timed out; choose Start sharing to try again");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (response.code) throw std::runtime_error(response.code == 1 ? "Desktop sharing was canceled in the permission dialog" : "Desktop denied screen or input sharing");
      g_dbus_connection_signal_unsubscribe(bus_, subscription); return std::move(response.results);
    } catch (...) {
      // Close on cancel/timeout without using the canceled GCancellable.
      auto *result = g_dbus_connection_call_sync(bus_, bus_name, response.path.c_str(), "org.freedesktop.portal.Request", "Close", nullptr,
                                               nullptr, G_DBUS_CALL_FLAGS_NONE, 1500, nullptr, nullptr);
      if (result) g_variant_unref(result);
      g_dbus_connection_signal_unsubscribe(bus_, subscription); throw;
    }
  }
  void open_portal(const std::string& parent) {
    GError *error = nullptr; auto *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel_, &error);
    if (!address) throw dbus_error("Session bus", error);
    bus_ = g_dbus_connection_new_for_address_sync(address,
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, cancel_, &error); g_free(address);
    if (!bus_) throw dbus_error("Session bus", error);
    g_dbus_connection_set_exit_on_close(bus_, FALSE);
    // KDE's RemoteDesktop backend checks types == Virtual, not types &
    // Virtual. Combining MONITOR|VIRTUAL silently shares physical screens.
    const unsigned source_types = selection_ == DesktopSource::virtual_output ? 4u : 1u;
    const auto available_sources = property(capture_iface, "AvailableSourceTypes");
    if ((property(desktop_iface, "AvailableDeviceTypes") & 3) != 3)
      throw std::runtime_error("This desktop needs a portal backend with both RemoteDesktop keyboard/pointer and ScreenCast monitor support");
    if (!(available_sources & source_types))
      throw std::runtime_error(selection_ == DesktopSource::virtual_output
        ? "This desktop portal does not offer virtual displays. Choose Existing monitors in RemoteOS Host"
        : "This desktop needs a portal backend with ScreenCast monitor support");
    if (property(capture_iface, "version") < 2 || !(property(capture_iface, "AvailableCursorModes") & 2))
      throw std::runtime_error("This screen-sharing backend must support an embedded cursor");
    const auto create = token();
    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(create.c_str()));
    const auto session_token = token(); g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(session_token.c_str()));
    auto session = request(desktop_iface, "CreateSession", create, g_variant_new("(@a{sv})", g_variant_builder_end(&b)));
    const char *path = nullptr;
    if (!g_variant_lookup(session.get(), "session_handle", "&s", &path) || !g_variant_is_object_path(path))
      throw std::runtime_error("Invalid desktop portal session handle");
    session_ = path;
    closed_subscription_ = g_dbus_connection_signal_subscribe(bus_, bus_name, "org.freedesktop.portal.Session", "Closed", session_.c_str(), nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *, gpointer p) {
        static_cast<WaylandHost *>(p)->fail("Desktop sharing permission was revoked or the selected monitor became unavailable");
      }, this, nullptr);
    owner_subscription_ = g_dbus_connection_signal_subscribe(bus_, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", bus_name,
      G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *v, gpointer p) {
        const char *name, *old_owner, *new_owner; g_variant_get(v, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (*old_owner && std::strcmp(old_owner, new_owner)) static_cast<WaylandHost *>(p)->fail("The desktop portal restarted; start sharing again");
      }, this, nullptr);
    const auto devices = token(); g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(devices.c_str()));
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(3));
    request(desktop_iface, "SelectDevices", devices, g_variant_new("(o@a{sv})", session_.c_str(), g_variant_builder_end(&b)));
    const auto sources = token(); g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(sources.c_str()));
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(source_types));
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(selection_ != DesktopSource::virtual_output));
    g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(2));
    request(capture_iface, "SelectSources", sources, g_variant_new("(o@a{sv})", session_.c_str(), g_variant_builder_end(&b)));
    const auto start = token();
    auto granted = request(desktop_iface, "Start", start, g_variant_new("(os@a{sv})", session_.c_str(), parent.c_str(), options(start)));
    unsigned granted_devices = 0;
    if (!g_variant_lookup(granted.get(), "devices", "u", &granted_devices) || (granted_devices & 3) != 3)
      throw std::runtime_error("Grant both keyboard and pointer access to use RemoteOS");
    Variant streams(g_variant_lookup_value(granted.get(), "streams", G_VARIANT_TYPE("a(ua{sv})")));
    const auto maximum_streams = native_output_ ? 16u : 2u;
    if (!streams || g_variant_n_children(streams.get()) < 1 || g_variant_n_children(streams.get()) > maximum_streams)
      throw std::runtime_error("The desktop portal must return one or two monitors");
    if (selection_ == DesktopSource::virtual_output && g_variant_n_children(streams.get()) != 1)
      throw std::runtime_error("The virtual display request must create exactly one output");
    for (std::size_t i = 0; i < g_variant_n_children(streams.get()); ++i) {
      Variant item(g_variant_get_child_value(streams.get(), i)); GVariant *properties = nullptr;
      auto s = std::make_unique<Source>(); g_variant_get(item.get(), "(u@a{sv})", &s->node, &properties); Variant props(properties);
      if (!s->node || s->node == UINT32_MAX) throw std::runtime_error("Invalid PipeWire screen node");
      for (const auto& previous : sources_) if (previous->node == s->node) throw std::runtime_error("Desktop portal returned duplicate screen nodes");
      unsigned type = 1; g_variant_lookup(props.get(), "source_type", "u", &type);
      if (type != 1 && type != 4) throw std::runtime_error("Select a monitor or virtual output rather than a window for RemoteOS");
      if (selection_ == DesktopSource::virtual_output && type != 4)
        throw std::runtime_error("The desktop portal returned a physical screen for the virtual display request; sharing was not started");
      s->virtual_monitor = type == 4;
      int w = 0, h = 0; g_variant_lookup(props.get(), "size", "(ii)", &w, &h);
      // size is compositor/logical geometry, not necessarily captured pixels.
      if (w < 0 || h < 0 || w > 16384 || h > 16384 || (w == 0) != (h == 0)) throw std::runtime_error("Invalid compositor screen size");
      s->logical_width = unsigned(w); s->logical_height = unsigned(h);
      g_variant_lookup(props.get(), "logical_size", "(ii)", &w, &h);
      if (w > 0 && h > 0 && w <= 16384 && h <= 16384) { s->logical_width = unsigned(w); s->logical_height = unsigned(h); }
      g_variant_lookup(props.get(), "pipewire-serial", "t", &s->serial);
      s->pixels.id = unsigned(i); sources_.push_back(std::move(s));
    }
    if (native_output_) select_native_node();
    pipewire_fd_ = open_pipewire_remote();
  }
  int open_pipewire_remote() {
    GError *error = nullptr;
    GUnixFDList *fds = nullptr; int index = -1;
    auto *result = g_dbus_connection_call_with_unix_fd_list_sync(bus_, bus_name, portal_path, capture_iface, "OpenPipeWireRemote",
      g_variant_new("(o@a{sv})", session_.c_str(), options()), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &fds, cancel_, &error);
    if (!result) throw dbus_error("OpenPipeWireRemote", error);
    g_variant_get(result, "(h)", &index); g_variant_unref(result);
    int fd = -1;
    if (fds) { fd = g_unix_fd_list_get(fds, index, &error); g_object_unref(fds); }
    if (fd < 0) throw dbus_error("PipeWire descriptor", error);
    return fd;
  }
  void select_native_node() {
    // KDE's RemoteDesktop portal grants all monitor nodes in one consent
    // prompt. Inspect their metadata on a separate restricted connection;
    // never open capture pipelines for a physical screen. Size alone cannot
    // establish identity: a physical monitor could have the same dimensions.
    struct Lookup {
      struct Node {
        Lookup *owner = nullptr;
        unsigned id = 0;
        pw_node *proxy = nullptr;
        spa_hook hook{};
        ~Node() { if (proxy) { spa_hook_remove(&hook); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy)); } }
      };
      pw_main_loop *loop = nullptr;
      pw_context *context = nullptr;
      pw_core *core = nullptr;
      pw_registry *registry = nullptr;
      spa_hook global_hook{}, core_hook{};
      std::map<unsigned, std::string> names;
      std::map<unsigned, std::uint64_t> serials;
      std::set<unsigned> allowed;
      std::map<unsigned, std::unique_ptr<Node>> bound;
      int sequence = -1;
      bool done = false;
      std::string error;
      ~Lookup() {
        bound.clear();
        if (registry) { spa_hook_remove(&global_hook); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry)); }
        if (core) { spa_hook_remove(&core_hook); pw_core_disconnect(core); }
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
      }
    } lookup;
    for (const auto& source : sources_) lookup.allowed.insert(source->node);
    pw_init(nullptr, nullptr);
    lookup.loop = pw_main_loop_new(nullptr);
    if (!lookup.loop) throw std::runtime_error("Cannot inspect the consented PipeWire node identities");
    lookup.context = pw_context_new(pw_main_loop_get_loop(lookup.loop), nullptr, 0);
    if (!lookup.context) throw std::runtime_error("Cannot create the PipeWire identity lookup");
    const int fd = open_pipewire_remote();
    lookup.core = pw_context_connect_fd(lookup.context, fd, nullptr, 0); // takes ownership of fd
    if (!lookup.core) throw std::runtime_error("Cannot connect to the consented PipeWire registry");
    lookup.registry = pw_core_get_registry(lookup.core, PW_VERSION_REGISTRY, 0);
    if (!lookup.registry) throw std::runtime_error("Cannot read the consented PipeWire registry");
    static const auto node_events = [] {
      pw_node_events events{}; events.version = PW_VERSION_NODE_EVENTS;
      events.info = +[](void *data, const pw_node_info *info) {
        auto& node = *static_cast<Lookup::Node *>(data);
        if (!info->props) return;
        const auto *name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME);
        if (!name) name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
        if (name && std::strlen(name) < 256) node.owner->names[node.id] = name;
        if (const auto *serial = spa_dict_lookup(info->props, PW_KEY_OBJECT_SERIAL)) {
          char *end = nullptr; const auto value = g_ascii_strtoull(serial, &end, 10);
          if (value && end && !*end) node.owner->serials[node.id] = value;
        }
      };
      return events;
    }();
    pw_registry_events registry_events{}; registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = +[](void *data, uint32_t id, uint32_t, const char *type, uint32_t version, const spa_dict *) {
      auto& lookup = *static_cast<Lookup *>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) || !lookup.allowed.count(id) || lookup.bound.count(id)) return;
      auto node = std::make_unique<Lookup::Node>(); node->owner = &lookup; node->id = id;
      node->proxy = static_cast<pw_node *>(pw_registry_bind(lookup.registry, id, PW_TYPE_INTERFACE_Node, std::min(version, uint32_t(PW_VERSION_NODE)), 0));
      if (!node->proxy) { lookup.error = "Cannot read the granted screen node properties"; return; }
      pw_node_add_listener(node->proxy, &node->hook, &node_events, node.get());
      lookup.bound.emplace(id, std::move(node));
    };
    pw_registry_add_listener(lookup.registry, &lookup.global_hook, &registry_events, &lookup);
    pw_core_events core_events{}; core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = +[](void *data, uint32_t id, int sequence) {
      auto& lookup = *static_cast<Lookup *>(data); if (id == PW_ID_CORE && sequence == lookup.sequence) lookup.done = true;
    };
    core_events.error = +[](void *data, uint32_t, int, int, const char *message) { static_cast<Lookup *>(data)->error = message; };
    pw_core_add_listener(lookup.core, &lookup.core_hook, &core_events, &lookup);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    // First round trip enumerates granted nodes. The second receives info for
    // binds queued by the registry callbacks; media.name is not a global prop.
    for (unsigned round = 0; round < 2; ++round) {
      lookup.done = false; lookup.sequence = pw_core_sync(lookup.core, PW_ID_CORE, lookup.sequence);
      while (!lookup.done) {
        check_cancel(); pump();
        if (!session_error().empty()) throw std::runtime_error(session_error());
        if (!lookup.error.empty()) throw std::runtime_error("PipeWire identity lookup: " + lookup.error);
        if (Clock::now() > deadline) throw std::runtime_error("PipeWire did not confirm the native virtual display identity");
        if (pw_loop_iterate(pw_main_loop_get_loop(lookup.loop), 20) < 0) throw std::runtime_error("PipeWire registry disconnected");
      }
    }
    const auto expected = "kwin-screencast-" + native_output_->output_name();
    std::unique_ptr<Source> selected;
    for (auto& source : sources_) {
      const auto name = lookup.names.find(source->node);
      if (name == lookup.names.end() || name->second != expected) continue;
      if (selected) throw std::runtime_error("The portal returned an ambiguous native virtual display identity");
      const auto serial = lookup.serials.find(source->node);
      if (serial == lookup.serials.end() || (source->serial && source->serial != serial->second))
        throw std::runtime_error("The native virtual display's PipeWire serial did not match the portal grant");
      source->serial = serial->second;
      if (source->logical_width != virtual_width_ || source->logical_height != virtual_height_)
        throw std::runtime_error("The portal's native display geometry does not match the HP panel");
      source->virtual_monitor = true; source->pixels.id = 0; selected = std::move(source);
    }
    sources_.clear();
    if (!selected) throw std::runtime_error("The desktop portal did not grant the RemoteOS native virtual display; no physical screen was captured");
    sources_.push_back(std::move(selected));
  }
  void open_capture() {
    gst_init(nullptr, nullptr);
    for (auto& s : sources_) {
      s->pipeline = gst_pipeline_new(nullptr);
      auto *source = gst_element_factory_make("pipewiresrc", nullptr);
      auto *convert = gst_element_factory_make("videoconvert", nullptr);
      auto *sink = gst_element_factory_make("appsink", nullptr);
      if (!source || !convert || !sink) {
        if (source) gst_object_unref(source);
        if (convert) gst_object_unref(convert);
        if (sink) gst_object_unref(sink);
        throw std::runtime_error("Install the GStreamer PipeWire, video conversion and app plugins for Wayland sharing");
      }
      gst_bin_add_many(GST_BIN(s->pipeline), source, convert, sink, nullptr);
      // pipewiresrc shares its GstPipeWireCore by the SAME fd value. Passing
      // different dup() descriptors would speak two protocols on one socket.
      // It duplicates internally; keep our original until ALL pipelines end.
      g_object_set(source, "fd", pipewire_fd_, "do-timestamp", TRUE, nullptr);
      if (s->serial) {
        const auto serial = std::to_string(s->serial);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object"))
          g_object_set(source, "target-object", serial.c_str(), nullptr);
        else {
          auto *props = gst_structure_new("props", "target.object", G_TYPE_STRING, serial.c_str(), nullptr);
          g_object_set(source, "stream-properties", props, nullptr); gst_structure_free(props);
        }
      } else { const auto node = std::to_string(s->node); g_object_set(source, "path", node.c_str(), nullptr); }
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "keepalive-time")) g_object_set(source, "keepalive-time", 1000, nullptr);
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "resend-last")) g_object_set(source, "resend-last", TRUE, nullptr);
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "always-copy")) g_object_set(source, "always-copy", TRUE, nullptr);
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "on-disconnect"))
        gst_util_set_object_arg(G_OBJECT(source), "on-disconnect", "error");
      auto *caps = gst_caps_from_string("video/x-raw,format=BGRA,width=[320,3840],height=[200,2160]");
      g_object_set(sink, "caps", caps, "sync", FALSE, "max-buffers", 1u, "drop", TRUE, "enable-last-sample", FALSE, nullptr); gst_caps_unref(caps);
      s->sink = GST_ELEMENT(gst_object_ref(sink));
      if (!gst_element_link_many(source, convert, sink, nullptr) || gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("Cannot start the consented PipeWire screen stream");
    }
  }
  bool all_negotiated() const {
    return !sources_.empty() && std::all_of(sources_.begin(), sources_.end(), [](const auto& s) { return s->negotiated; });
  }
  void collect() {
    for (auto& s : sources_) {
      auto *bus = gst_element_get_bus(s->pipeline);
      auto *message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)); gst_object_unref(bus);
      if (message) {
        std::string why = "PipeWire screen stream ended";
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) { GError *e = nullptr; gst_message_parse_error(message, &e, nullptr); if (e) { why = e->message; g_error_free(e); } }
        gst_message_unref(message); throw std::runtime_error(why);
      }
      if (auto *fresh = gst_app_sink_try_pull_sample(GST_APP_SINK(s->sink), 0)) {
        if (s->latest) gst_sample_unref(s->latest);
        s->latest = fresh;
      }
      // Damage-driven compositors may not produce another frame on an idle
      // desktop. Keep exactly one owned sample for a fresh session/reconnect
      // and repeat it at the negotiated cadence; never accumulate old frames.
      auto *sample = s->latest;
      if (!sample) continue;
      GstVideoInfo info{};
      if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA ||
          info.width < 1 || info.height < 1 || info.width > 3840 || info.height > 2160)
        throw std::runtime_error("Unsupported PipeWire frame format or source size (maximum 3840x2160)");
      std::lock_guard<std::mutex> lock(mutex_);
      if (!s->negotiated) {
        if (native_output_ && (unsigned(info.width) != virtual_width_ || unsigned(info.height) != virtual_height_))
          throw std::runtime_error("KDE capture pixels differ from the native HP desktop; scaling was not started");
        s->pixels.width = unsigned(info.width); s->pixels.height = unsigned(info.height); s->negotiated = true;
        if (!s->logical_width) { s->logical_width = unsigned(info.width); s->logical_height = unsigned(info.height); }
      } else if (s->pixels.width != unsigned(info.width) || s->pixels.height != unsigned(info.height))
        throw std::runtime_error("A shared monitor changed resolution; choose Start sharing again");
      const auto i = s->pixels.id; const auto now = Clock::now();
      if (!callback_ || i >= logical_.size() || now < s->next) continue;
      s->next = detail::next_capture(now, s->next, logical_[i].fps);
      GstVideoFrame mapped{};
      if (!gst_video_frame_map(&mapped, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) throw std::runtime_error("Cannot read a PipeWire video buffer");
      const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
      if (std::abs(stride) < info.width * 4) { gst_video_frame_unmap(&mapped); throw std::runtime_error("Invalid PipeWire row stride"); }
      VideoFrame frame; frame.display_id = logical_[i].id; frame.width = unsigned(info.width); frame.height = unsigned(info.height);
      frame.stride = frame.width * 4; frame.timestamp = now; frame.bgra.resize(std::size_t(frame.stride) * frame.height);
      const auto *data = static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0));
      for (unsigned y = 0; y < frame.height; ++y) std::memcpy(frame.bgra.data() + std::size_t(y) * frame.stride, data + std::ptrdiff_t(y) * stride, frame.stride);
      gst_video_frame_unmap(&mapped); callback_(std::move(frame));
    }
  }
  void cleanup() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {}; logical_.clear(); sources_.clear();
    if (pipewire_fd_ >= 0) { close(pipewire_fd_); pipewire_fd_ = -1; }
    if (bus_) {
      if (closed_subscription_) g_dbus_connection_signal_unsubscribe(bus_, closed_subscription_);
      if (owner_subscription_) g_dbus_connection_signal_unsubscribe(bus_, owner_subscription_);
      if (!session_.empty()) {
        auto *r = g_dbus_connection_call_sync(bus_, bus_name, session_.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                                             nullptr, G_DBUS_CALL_FLAGS_NONE, 1500, nullptr, nullptr);
        if (r) g_variant_unref(r);
      }
      g_dbus_connection_close_sync(bus_, nullptr, nullptr); g_clear_object(&bus_);
    }
    keys_.clear(); buttons_.clear(); session_.clear();
    native_output_.reset(); // closing the owner removes only our virtual output
  }
  DesktopSource selection_ = DesktopSource::monitors;
  unsigned virtual_width_ = 1024, virtual_height_ = 576;
  std::unique_ptr<KdeVirtualOutput> native_output_;
  GCancellable *cancel_;
  GMainContext *context_ = nullptr;
  GDBusConnection *bus_ = nullptr;
  std::string session_, error_;
  guint closed_subscription_ = 0, owner_subscription_ = 0;
  int pipewire_fd_ = -1;
  std::atomic<bool> closing_{false}, prepared_{false};
  mutable std::mutex mutex_, error_mutex_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<DisplayConfig> logical_;
  std::set<unsigned> keys_, buttons_;
  FrameCallback callback_;
  std::thread worker_;
};
}
std::unique_ptr<HostPlatform> make_wayland_host_platform() { return std::make_unique<WaylandHost>(); }
} // namespace remoteos::platform
