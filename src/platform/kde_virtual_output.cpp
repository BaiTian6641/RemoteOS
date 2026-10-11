#include "kde_virtual_output.hpp"
#include "kde-screencast-client.h"
#include <gio/gio.h>
#include <wayland-client.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace remoteos::platform {
namespace {
void require_opengl_compositor() {
    GError *error = nullptr;
    auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) { g_clear_error(&error); throw std::runtime_error("The KDE desktop session bus is unavailable"); }
    auto *value = g_dbus_connection_call_sync(bus, "org.kde.KWin", "/KWin", "org.kde.KWin", "supportInformation",
        nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    g_object_unref(bus);
    if (!value) { g_clear_error(&error); throw std::runtime_error("KDE native display mode requires a running KWin Wayland desktop"); }
    const char *text = nullptr; g_variant_get(value, "(&s)", &text);
    const bool supported = std::string(text).find("Compositing Type: OpenGL") != std::string::npos;
    g_variant_unref(value);
    // Plasma 6.5's screencast plugin dereferences an EGL display on QPainter.
    // Check before creating any virtual output, rather than risking its crash.
    if (!supported) throw std::runtime_error("KDE native virtual capture requires OpenGL compositing; the QPainter renderer is unsupported");
}
// KWin identifies the actual executable and looks up this declaration in
// KService. Register our OWN executable, never impersonate another application
// or turn off the compositor's interface permission checks.
void register_desktop_declaration() {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    std::filesystem::path data;
    if (const auto *base = std::getenv("XDG_DATA_HOME"); base && *base) data = base;
    else if (const auto *home = std::getenv("HOME"); home && *home) data = std::filesystem::path(home) / ".local/share";
    else throw std::runtime_error("A user data directory is required for KDE native virtual displays");
    if (!data.is_absolute()) throw std::runtime_error("XDG_DATA_HOME must be an absolute path");
    auto *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, executable.c_str(), -1);
    const auto path = data / "applications" / ("org.remoteos.NativeOutput." + std::string(hash, 16) + ".desktop");
    g_free(hash);
    std::string quoted;
    for (const char c : executable) {
        if (c == '\n' || c == '\r' || c == '%') throw std::runtime_error("Move RemoteOS to a path without newlines or percent signs");
        if (c == '\\') quoted += "\\\\\\\\";
        else { if (c == '"' || c == '$' || c == '`') quoted += "\\\\"; quoted += c; }
    }
    const std::string entry = "[Desktop Entry]\nType=Application\nName=RemoteOS Native Virtual Display\nCategories=Network;RemoteAccess;\nNoDisplay=true\nExec=\"" + quoted +
        "\"\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n";
    std::filesystem::create_directories(path.parent_path());
    gchar *previous = nullptr; gsize length = 0;
    const bool unchanged = g_file_get_contents(path.c_str(), &previous, &length, nullptr) && std::string(previous, length) == entry;
    g_free(previous);
    if (unchanged) return;
    GError *error = nullptr;
    if (!g_file_set_contents_full(path.c_str(), entry.c_str(), entry.size(), G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
        const std::string why = error ? error->message : "cannot save the application declaration";
        g_clear_error(&error); throw std::runtime_error("KDE native display registration: " + why);
    }
    auto *launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    g_subprocess_launcher_unsetenv(launcher, "LD_LIBRARY_PATH");
    g_subprocess_launcher_setenv(launcher, "QT_QPA_PLATFORM", "offscreen", TRUE);
    auto *process = g_subprocess_launcher_spawn(launcher, &error, "kbuildsycoca6", nullptr);
    g_object_unref(launcher);
    if (!process || !g_subprocess_wait_check(process, nullptr, &error)) {
        if (process) g_object_unref(process);
        const std::string why = error ? error->message : "kbuildsycoca6 is unavailable";
        g_clear_error(&error); throw std::runtime_error("Install KDE's kbuildsycoca6 to register the native display application: " + why);
    }
    g_object_unref(process);
}
}

struct KdeVirtualOutput::State {
    struct Output {
        wl_output *handle = nullptr;
        std::string name;
        int width = 0, height = 0, scale = 1;
        bool complete = false;
    };
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    zkde_screencast_unstable_v1 *manager = nullptr;
    zkde_screencast_stream_unstable_v1 *stream = nullptr;
    std::map<unsigned, Output> outputs;
    std::string requested_name, actual_name, error;
    unsigned wanted_width, wanted_height;
    bool created = false, closing = false;

    State(unsigned width, unsigned height) : wanted_width(width), wanted_height(height) {}
    ~State() {
        closing = true;
        if (stream) zkde_screencast_stream_unstable_v1_close(stream);
        for (auto& item : outputs) wl_output_release(item.second.handle);
        if (manager) zkde_screencast_unstable_v1_destroy(manager);
        if (registry) wl_registry_destroy(registry);
        if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
    void inspect() {
        for (const auto& item : outputs) {
            const auto& output = item.second;
            if (output.name != requested_name && output.name != "Virtual-" + requested_name) continue;
            actual_name = output.name;
            if (output.complete && (output.width != int(wanted_width) || output.height != int(wanted_height) || output.scale != 1))
                error = "KDE changed the native virtual output size or scale; stop sharing and start again";
        }
    }
    bool ready() const {
        if (!created || actual_name.empty()) return false;
        return std::any_of(outputs.begin(), outputs.end(), [&](const auto& item) {
            const auto& o = item.second;
            return o.name == actual_name && o.complete && o.width == int(wanted_width) && o.height == int(wanted_height) && o.scale == 1;
        });
    }
};

KdeVirtualOutput::KdeVirtualOutput(unsigned width, unsigned height) : state_(std::make_unique<State>(width, height)) {
    if (!((width == 1024 && height == 576) || (width == 1366 && height == 768) || (width == 1920 && height == 1080)))
        throw std::invalid_argument("Choose an HP 2140 panel size or 1920x1080 for the native virtual output");
    require_opengl_compositor();
    register_desktop_declaration();
    auto& s = *state_;
    auto *uuid = g_uuid_string_random(); s.requested_name = "RemoteOS-HP2140-" + std::string(uuid); g_free(uuid);
    static const wl_output_listener output_listener{
        +[](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {},
        +[](void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t) {
            auto& o = *static_cast<State::Output *>(data);
            if (flags & WL_OUTPUT_MODE_CURRENT) { o.width = width; o.height = height; }
        },
        +[](void *data, wl_output *) { static_cast<State::Output *>(data)->complete = true; },
        +[](void *data, wl_output *, int32_t scale) { static_cast<State::Output *>(data)->scale = scale; },
        +[](void *data, wl_output *, const char *name) { static_cast<State::Output *>(data)->name = name; },
        +[](void *, wl_output *, const char *) {}
    };
    static const wl_registry_listener registry_listener{
        +[](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
            auto& s = *static_cast<State *>(data);
            if (std::string(interface) == zkde_screencast_unstable_v1_interface.name && version >= 4)
                s.manager = static_cast<zkde_screencast_unstable_v1 *>(wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, std::min(version, 5u)));
            if (std::string(interface) == wl_output_interface.name && version >= 4) {
                auto& o = s.outputs[name];
                o.handle = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 4));
                wl_output_add_listener(o.handle, &output_listener, &o);
            }
        },
        +[](void *data, wl_registry *, uint32_t name) {
            auto& s = *static_cast<State *>(data); const auto found = s.outputs.find(name);
            if (found == s.outputs.end()) return;
            if (!s.closing && found->second.name == s.actual_name) s.error = "The native RemoteOS virtual display was removed";
            wl_output_release(found->second.handle); s.outputs.erase(found);
        }
    };
    // KService throttles its cache checks (1.5 s on Plasma 6.5). A compositor
    // can still see the old cache immediately after kbuildsycoca finishes.
    // Retry with fresh connections: KWin caches requested interfaces per client.
    for (unsigned attempt = 0; attempt < 25; ++attempt) {
        s.display = wl_display_connect(nullptr);
        if (!s.display) throw std::runtime_error("Cannot connect to KDE's Wayland compositor");
        s.registry = wl_display_get_registry(s.display);
        wl_registry_add_listener(s.registry, &registry_listener, &s);
        if (wl_display_roundtrip(s.display) < 0) throw std::runtime_error("The Wayland compositor disconnected during display setup");
        if (s.manager) break;
        for (auto& item : s.outputs) wl_output_release(item.second.handle);
        s.outputs.clear(); wl_registry_destroy(s.registry); s.registry = nullptr;
        wl_display_disconnect(s.display); s.display = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!s.manager) throw std::runtime_error("This compositor does not offer KDE's native virtual-output API. Use KDE Plasma with kbuildsycoca6, or Existing monitors");
    const std::string description = "RemoteOS HP 2140 (" + std::to_string(width) + "x" + std::to_string(height) + ", native)";
    s.stream = zkde_screencast_unstable_v1_stream_virtual_output_with_description(s.manager, s.requested_name.c_str(), description.c_str(),
        int(width), int(height), wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    static const zkde_screencast_stream_unstable_v1_listener stream_listener{
        +[](void *data, zkde_screencast_stream_unstable_v1 *) { static_cast<State *>(data)->error = "KDE closed the native virtual display"; },
        +[](void *data, zkde_screencast_stream_unstable_v1 *, uint32_t) { static_cast<State *>(data)->created = true; },
        +[](void *data, zkde_screencast_stream_unstable_v1 *, const char *why) { static_cast<State *>(data)->error = why; }
    };
    zkde_screencast_stream_unstable_v1_add_listener(s.stream, &stream_listener, &s);
    if (wl_display_flush(s.display) < 0 && errno != EAGAIN) throw std::runtime_error("Cannot request the native KDE virtual display");
}

KdeVirtualOutput::~KdeVirtualOutput() = default;
void KdeVirtualOutput::pump() {
    auto& s = *state_;
    while (wl_display_prepare_read(s.display) != 0) {
        if (wl_display_dispatch_pending(s.display) < 0) throw std::runtime_error("KDE disconnected the virtual output");
    }
    const int flushed = wl_display_flush(s.display);
    pollfd fd{wl_display_get_fd(s.display), short(POLLIN | (flushed < 0 && errno == EAGAIN ? POLLOUT : 0)), 0};
    const int result = poll(&fd, 1, 0);
    if (result > 0 && (fd.revents & POLLIN)) {
        if (wl_display_read_events(s.display) < 0) throw std::runtime_error("KDE disconnected the virtual output");
    } else wl_display_cancel_read(s.display);
    if ((result < 0 && errno != EINTR) || (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        throw std::runtime_error("KDE disconnected the virtual output");
    if (wl_display_dispatch_pending(s.display) < 0) throw std::runtime_error("Cannot receive KDE virtual output state");
    s.inspect();
    if (!s.error.empty()) throw std::runtime_error(s.error);
}
bool KdeVirtualOutput::ready() const { return state_->ready(); }
const std::string& KdeVirtualOutput::output_name() const { return state_->actual_name; }
unsigned KdeVirtualOutput::width() const { return state_->wanted_width; }
unsigned KdeVirtualOutput::height() const { return state_->wanted_height; }
} // namespace remoteos::platform
