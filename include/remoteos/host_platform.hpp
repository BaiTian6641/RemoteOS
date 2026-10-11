#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace remoteos::platform {

struct DisplayConfig {
    std::uint32_t id = 0;
    unsigned width = 1366, height = 768;
    int x = 0, y = 0;
    unsigned fps = 30;
};

// key is a Linux input-event KEY_* code, including BTN_LEFT/RIGHT/MIDDLE.
// Absolute pointer coordinates are in host logical desktop pixels.
enum class InputKind { key, button, absolute_pointer, relative_pointer, wheel, secure_attention };
struct InputEvent {
    InputKind kind = InputKind::key;
    std::uint16_t key = 0;
    bool down = false;
    bool repeat = false;
    int x = 0, y = 0, delta_x = 0, delta_y = 0;
};

struct VideoFrame {
    std::uint32_t display_id = 0;
    unsigned width = 0, height = 0, stride = 0;
    std::chrono::steady_clock::time_point timestamp;
    std::vector<std::uint8_t> bgra;
};

struct Capabilities {
    bool capture = false;
    bool input = false;
    bool virtual_displays = false;
    // This is deliberately false until the platform's login/lock/UAC matrix
    // has passed. Installed code is not evidence of hardware qualification.
    bool unattended_qualified = false;
    std::vector<std::string> diagnostics;
};

using FrameCallback = std::function<void(VideoFrame)>;
enum class DesktopSource { monitors, virtual_output, kde_native_output };

class HostPlatform {
public:
    virtual ~HostPlatform() = default;
    virtual Capabilities capabilities() const = 0;
    // Select before prepare. A virtual source is an explicit portal request,
    // not a physical capture scaled to look like an independent desktop.
    virtual void select_desktop_source(DesktopSource source) {
        if (source != DesktopSource::monitors)
            throw std::runtime_error("Virtual display sharing requires a Wayland portal with virtual-output support");
    }
    // Native virtual outputs are created at this size before asking for the
    // desktop sharing grant. The client still verifies its detected panel size.
    virtual void set_virtual_display_size(unsigned, unsigned) {
        throw std::runtime_error("This backend cannot create a native virtual output");
    }
    // Application backends may need user consent before advertising sources.
    // prepare runs on a setup worker; cancellation must be thread safe.
    virtual void prepare(const std::string& = "") {}
    virtual void cancel_prepare() noexcept {}
    virtual std::string session_error() const { return {}; }
    // Existing monitor geometry for application mode. The client keeps its
    // own logical layout; the backend maps input into these physical outputs.
    virtual std::vector<DisplayConfig> desktop_displays() const { return {}; }
    // Resize the real host desktop before capture. Implementations must
    // restore changed modes on stop; unsupported matching is an explicit
    // error, never a claim that scaling a capture resized the desktop.
    virtual void match_desktop(const std::vector<DisplayConfig>&) {
        throw std::runtime_error("This desktop cannot match the client resolution automatically");
    }
    // A callback runs on a capture thread; callers must be thread safe and
    // keep it short. Frames own their bytes. start/stop/inject are serialized
    // by the caller. Exactly one active client, at most two displays.
    virtual void start(const std::vector<DisplayConfig>& displays, FrameCallback callback) = 0;
    virtual void stop() noexcept = 0;
    virtual void inject(const InputEvent& event) = 0;
    virtual void release_all() noexcept = 0;
};

std::unique_ptr<HostPlatform> make_host_platform();
std::unique_ptr<HostPlatform> make_desktop_host_platform();

} // namespace remoteos::platform
