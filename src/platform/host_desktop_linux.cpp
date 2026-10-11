#include "remoteos/host_platform.hpp"
#include "display_config.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <memory>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#ifdef REMOTEOS_HAVE_DESKTOP_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

namespace remoteos::platform {
namespace {
#ifdef REMOTEOS_HAVE_DESKTOP_X11
// X errors from unsupported RandR transforms must become a visible connection
// error rather than invoking Xlib's default process-termination handler.
class RandRErrors {
public:
  explicit RandRErrors(::Display *display) : display_(display) { XSync(display_, False); current_ = this; previous_ = XSetErrorHandler(handler); }
  ~RandRErrors() { XSync(display_, False); XSetErrorHandler(previous_); current_ = nullptr; }
  bool failed() { XSync(display_, False); return error_ != 0; }
private:
  static int handler(::Display *, XErrorEvent *event) { if (current_) current_->error_ = event->error_code; return 0; }
  inline static thread_local RandRErrors *current_ = nullptr;
  ::Display *display_;
  XErrorHandler previous_;
  int error_ = 0;
};
class DesktopHost final : public HostPlatform {
public:
  DesktopHost() {
    XInitThreads();
    display_ = XOpenDisplay(nullptr);
    if (!display_) { error_ = "Open this application inside the desktop's authorized Xorg session"; return; }
    int event, error, major, minor;
    input_available_ = XTestQueryExtension(display_, &event, &error, &major, &minor);
    cursor_available_ = XFixesQueryExtension(display_, &event, &error);
    // Capture has its own X connection. A framebuffer round trip must never
    // hold the input connection or its lock while a key/pointer waits.
    capture_display_ = XOpenDisplay(DisplayString(display_));
    independent_ = std::getenv("REMOTEOS_INDEPENDENT_DESKTOP") &&
      std::string(std::getenv("REMOTEOS_INDEPENDENT_DESKTOP")) == "1";
    if (independent_) if (const auto *window = std::getenv("REMOTEOS_NESTED_INPUT_WINDOW")) {
      char *end = nullptr;
      const auto value = std::strtoul(window, &end, 10);
      if (!value || !end || *end || value > 0xfffffffful)
        throw std::runtime_error("Invalid private compositor input window");
      nested_input_window_ = static_cast<Window>(value);
    }
  }
  ~DesktopHost() override { stop(); if (capture_display_) XCloseDisplay(capture_display_); if (display_) XCloseDisplay(display_); }
  Capabilities capabilities() const override {
    Capabilities caps;
    caps.capture = capture_display_ != nullptr;
    caps.input = input_available_;
    caps.virtual_displays = independent_;
    caps.diagnostics.emplace_back(independent_
      ? "Independent userspace desktop: private X server, input and session bus; same Unix account"
      : "Application mode shares existing monitors in this user's Xorg session; no EVDI/uinput or service installation");
    if (capture_display_ && XShmQueryExtension(capture_display_))
      caps.diagnostics.emplace_back("Shared-memory capture available; input uses a separate asynchronous X connection");
    if (!input_available_) caps.diagnostics.emplace_back("XTEST input extension is unavailable");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_.empty()) caps.diagnostics.push_back(error_);
    return caps;
  }
  std::vector<DisplayConfig> desktop_displays() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return configured_sources_.empty() ? monitors() : configured_sources_;
  }
  std::string session_error() const override {
    std::lock_guard<std::mutex> lock(mutex_); return error_;
  }
  void match_desktop(const std::vector<DisplayConfig>& requested) override {
    detail::validate_displays(requested);
    if (!display_ || running_) throw std::runtime_error("Stop desktop capture before changing display resolution");
    std::lock_guard<std::mutex> lock(mutex_);
    if (independent_) { configure_independent(requested); return; }
    const auto root = DefaultRootWindow(display_);
    auto *resources = XRRGetScreenResourcesCurrent(display_, root);
    if (!resources) throw std::runtime_error("The Xorg display has no RandR configuration");
    std::unique_ptr<XRRScreenResources, decltype(&XRRFreeScreenResources)> owned(resources, XRRFreeScreenResources);
    int count = 0; auto *monitor_list = XRRGetMonitors(display_, root, True, &count);
    std::unique_ptr<XRRMonitorInfo, decltype(&XRRFreeMonitors)> monitors_owned(monitor_list, XRRFreeMonitors);
    std::vector<XRRMonitorInfo *> selected;
    for (int i = 0; monitor_list && i < count; ++i) if (monitor_list[i].noutput) selected.push_back(&monitor_list[i]);
    std::stable_sort(selected.begin(), selected.end(), [](auto *a, auto *b) { return a->primary > b->primary; });
    if (requested.size() > selected.size()) throw std::runtime_error("Connect another host output before matching two client screens");
    bool already_matches = selected.size() == requested.size();
    for (std::size_t i = 0; i < requested.size(); ++i)
      already_matches = already_matches && selected[i]->width == int(requested[i].width) && selected[i]->height == int(requested[i].height) && selected[i]->x == requested[i].x && selected[i]->y == requested[i].y;
    if (already_matches) { configured_sources_ = requested; return; }
    int width = 0, height = 0, minimum_w, minimum_h, maximum_w, maximum_h;
    for (const auto& d : requested) { width = std::max(width, d.x + int(d.width)); height = std::max(height, d.y + int(d.height)); }
    if (!XRRGetScreenSizeRange(display_, root, &minimum_w, &minimum_h, &maximum_w, &maximum_h) || width < minimum_w || height < minimum_h || width > maximum_w || height > maximum_h)
      throw std::runtime_error("The Xorg driver cannot expose the client's desktop size");
    struct Target { SavedCrtc crtc; unsigned physical_width, physical_height; };
    std::vector<Target> targets;
    for (std::size_t i = 0; i < requested.size(); ++i) {
      auto *output = XRRGetOutputInfo(display_, resources, selected[i]->outputs[0]);
      if (!output) throw std::runtime_error("Host output disappeared during resolution matching");
      std::unique_ptr<XRROutputInfo, decltype(&XRRFreeOutputInfo)> output_owned(output, XRRFreeOutputInfo);
      const auto crtc_id = output->crtc;
      if (!crtc_id || std::any_of(targets.begin(), targets.end(), [&](const auto& t) { return t.crtc.id == crtc_id; }))
        throw std::runtime_error("Independent client screens require independent host outputs");
      auto *crtc = XRRGetCrtcInfo(display_, resources, crtc_id);
      if (!crtc) throw std::runtime_error("Cannot read host output configuration");
      std::unique_ptr<XRRCrtcInfo, decltype(&XRRFreeCrtcInfo)> crtc_owned(crtc, XRRFreeCrtcInfo);
      auto mode = crtc->mode;
      const bool rotated = crtc->rotation & (RR_Rotate_90 | RR_Rotate_270);
      for (int n = 0; n < output->nmode; ++n) for (int m = 0; m < resources->nmode; ++m) {
        const auto& candidate = resources->modes[m];
        if (candidate.id == output->modes[n] && (rotated ? candidate.height : candidate.width) == requested[i].width && (rotated ? candidate.width : candidate.height) == requested[i].height) { mode = candidate.id; break; }
      }
      const auto found = std::find_if(resources->modes, resources->modes + resources->nmode, [&](const auto& m) { return m.id == mode; });
      if (found == resources->modes + resources->nmode) throw std::runtime_error("Host output has no usable mode");
      Target target{}; target.crtc.id = crtc_id; target.crtc.mode = mode; target.crtc.rotation = crtc->rotation;
      target.crtc.x = requested[i].x; target.crtc.y = requested[i].y;
      target.crtc.outputs.assign(crtc->outputs, crtc->outputs + crtc->noutput);
      target.physical_width = rotated ? found->height : found->width;
      target.physical_height = rotated ? found->width : found->height;
      target.crtc.transform.matrix[0][0] = XFixed(std::llround(double(requested[i].width) / target.physical_width * 65536));
      target.crtc.transform.matrix[1][1] = XFixed(std::llround(double(requested[i].height) / target.physical_height * 65536));
      target.crtc.transform.matrix[2][2] = 65536;
      target.crtc.filter = "bilinear";
      targets.push_back(std::move(target));
    }
    if (saved_crtcs_.empty()) {
      XWindowAttributes attributes{}; XGetWindowAttributes(display_, root, &attributes);
      saved_width_ = attributes.width; saved_height_ = attributes.height;
      saved_mm_width_ = DisplayWidthMM(display_, DefaultScreen(display_)); saved_mm_height_ = DisplayHeightMM(display_, DefaultScreen(display_));
      for (int i = 0; i < resources->ncrtc; ++i) {
        auto *info = XRRGetCrtcInfo(display_, resources, resources->crtcs[i]);
        if (!info) continue;
        SavedCrtc saved{}; saved.id = resources->crtcs[i]; saved.mode = info->mode; saved.x = info->x; saved.y = info->y; saved.rotation = info->rotation;
        saved.outputs.assign(info->outputs, info->outputs + info->noutput); XRRFreeCrtcInfo(info);
        saved.transform.matrix[0][0] = saved.transform.matrix[1][1] = saved.transform.matrix[2][2] = 65536;
        XRRCrtcTransformAttributes *transform = nullptr;
        if (XRRGetCrtcTransform(display_, saved.id, &transform) && transform) {
          saved.transform = transform->currentTransform;
          if (transform->currentFilter) saved.filter = transform->currentFilter;
          if (transform->currentNparams) saved.parameters.assign(transform->currentParams, transform->currentParams + transform->currentNparams);
          XFree(transform);
        }
        if (auto *panning = XRRGetPanning(display_, resources, saved.id)) { saved.panning = *panning; XRRFreePanning(panning); }
        saved_crtcs_.push_back(std::move(saved));
      }
    }
    try {
      RandRErrors errors(display_);
      for (const auto& saved : saved_crtcs_) set_crtc(saved, true);
      XRRSetScreenSize(display_, root, width, height, std::max(1, saved_mm_width_ * width / saved_width_), std::max(1, saved_mm_height_ * height / saved_height_));
      for (const auto& target : targets) {
        XRRSetCrtcTransform(display_, target.crtc.id, const_cast<XTransform *>(&target.crtc.transform), target.crtc.filter.c_str(), nullptr, 0);
        auto *current = XRRGetScreenResourcesCurrent(display_, root); XRRPanning panning{};
        const auto previous = std::find_if(saved_crtcs_.begin(), saved_crtcs_.end(), [&](const auto& c) { return c.id == target.crtc.id; });
        if (previous != saved_crtcs_.end() && (previous->panning.width || previous->panning.height)) XRRSetPanning(display_, current, target.crtc.id, &panning);
        XRRFreeScreenResources(current);
        set_crtc(target.crtc, false);
      }
      if (errors.failed()) throw std::runtime_error("This Xorg driver cannot match the client's resolution through RandR modes or host-side scaling");
      configured_sources_ = requested;
    } catch (...) { restore_modes(); throw; }
  }
  void start(const std::vector<DisplayConfig>& displays, FrameCallback callback) override {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    release_all();
    detail::validate_displays(displays);
    if (!capture_display_ || !input_available_ || !callback)
      throw std::runtime_error("Desktop capture and XTEST must be available in the current Xorg session");
    sources_ = desktop_displays();
    if (displays.size() > sources_.size()) throw std::runtime_error("Connect another host monitor to share a second independent desktop");
    for (std::size_t i = 0; i < displays.size(); ++i)
      if (sources_[i].width > 3840 || sources_[i].height > 2160)
        throw std::runtime_error("Application capture currently supports source monitors up to 3840x2160");
    logical_ = displays;
    callback_ = std::move(callback);
    running_ = true;
    worker_ = std::thread([this] { capture(); });
  }
  void stop() noexcept override {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    release_all();
    logical_.clear(); sources_.clear(); callback_ = {};
    std::lock_guard<std::mutex> lock(mutex_); restore_modes();
  }
  void inject(const InputEvent& event) override {
    if (!display_ || !input_available_) throw std::runtime_error("Desktop input unavailable");
    std::lock_guard<std::mutex> lock(mutex_);
    // Private session helpers can create an X11 input window and steal the
    // outer server's keyboard focus. The nested compositor owns the remote
    // seat; keep keyboard/button events directed to its launcher-owned window.
    // This never runs when sharing the logged-in desktop's actual X server.
    if (nested_input_window_ && (event.kind == InputKind::key || event.kind == InputKind::button))
      XSetInputFocus(display_, nested_input_window_, RevertToParent, CurrentTime);
    switch (event.kind) {
    case InputKind::key:
      if (!event.key || event.key > 247) throw std::invalid_argument("Key is outside the Xorg evdev keycode range");
      XTestFakeKeyEvent(display_, event.key + 8, event.down, CurrentTime);
      if (event.down) keys_.insert(event.key); else keys_.erase(event.key);
      break;
    case InputKind::button: {
      const unsigned button = button_number(event.key);
      XTestFakeButtonEvent(display_, button, event.down, CurrentTime);
      if (event.down) buttons_.insert(button); else buttons_.erase(button);
      break;
    }
    case InputKind::absolute_pointer: {
      bool mapped = false;
      for (std::size_t i = 0; i < logical_.size(); ++i) {
        const auto& d = logical_[i]; const auto& source = sources_[i];
        if (event.x < d.x || event.x >= d.x + int(d.width) || event.y < d.y || event.y >= d.y + int(d.height)) continue;
        const auto x = source.x + (std::int64_t(event.x - d.x) * (source.width - 1)) / std::max(1u, d.width - 1);
        const auto y = source.y + (std::int64_t(event.y - d.y) * (source.height - 1)) / std::max(1u, d.height - 1);
        XTestFakeMotionEvent(display_, DefaultScreen(display_), int(x), int(y), CurrentTime);
        mapped = true; break;
      }
      if (!mapped) throw std::invalid_argument("Pointer is outside the shared monitor layout");
      break;
    }
    case InputKind::relative_pointer:
      XTestFakeRelativeMotionEvent(display_, event.delta_x, event.delta_y, CurrentTime); break;
    case InputKind::wheel:
      wheel(event.delta_y, 4, 5); wheel(event.delta_x, 7, 6); break;
    case InputKind::secure_attention:
      throw std::runtime_error("Application hosting cannot inject a secure attention sequence");
    }
    // XTEST requests remain ordered on this connection. Flush sends them
    // immediately without a synchronous server acknowledgement per event.
    XFlush(display_);
  }
  void release_all() noexcept override {
    if (!display_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (nested_input_window_) XSetInputFocus(display_, nested_input_window_, RevertToParent, CurrentTime);
    for (auto key : keys_) XTestFakeKeyEvent(display_, key + 8, False, CurrentTime);
    for (auto button : buttons_) XTestFakeButtonEvent(display_, button, False, CurrentTime);
    keys_.clear(); buttons_.clear(); XSync(display_, False);
  }
private:
  // Each capture image is allocated once per stream. IPC_PRIVATE and 0600
  // restrict the segment; mark it for deletion after the X server attaches.
  struct CaptureImage {
    ::Display *display;
    XImage *image = nullptr;
    XShmSegmentInfo shared{};
    bool attached = false;
    CaptureImage(::Display *d, unsigned width, unsigned height) : display(d) {
      shared.shmid = -1; shared.shmaddr = reinterpret_cast<char *>(-1);
      if (!XShmQueryExtension(d)) return;
      image = XShmCreateImage(d, DefaultVisual(d, DefaultScreen(d)), DefaultDepth(d, DefaultScreen(d)),
                              ZPixmap, nullptr, &shared, width, height);
      if (!image) return;
      shared.shmid = shmget(IPC_PRIVATE, std::size_t(image->bytes_per_line) * image->height, IPC_CREAT | 0600);
      if (shared.shmid < 0) { dispose(); return; }
      shared.shmaddr = static_cast<char *>(shmat(shared.shmid, nullptr, 0));
      if (shared.shmaddr == reinterpret_cast<char *>(-1)) { dispose(); return; }
      image->data = shared.shmaddr; shared.readOnly = False;
      {
        RandRErrors errors(d);
        const auto accepted = XShmAttach(d, &shared);
        attached = accepted && !errors.failed();
      }
      if (!attached) { dispose(); return; }
      shmctl(shared.shmid, IPC_RMID, nullptr);
    }
    ~CaptureImage() { dispose(); }
    void dispose() noexcept {
      if (attached) { XShmDetach(display, &shared); XSync(display, False); attached = false; }
      if (image) { image->data = nullptr; XDestroyImage(image); image = nullptr; }
      if (shared.shmaddr != reinterpret_cast<char *>(-1)) { shmdt(shared.shmaddr); shared.shmaddr = reinterpret_cast<char *>(-1); }
      if (shared.shmid >= 0) { shmctl(shared.shmid, IPC_RMID, nullptr); shared.shmid = -1; }
    }
  };
  void configure_independent(const std::vector<DisplayConfig>& requested) {
    // This branch is reachable only in the launcher-owned Xvnc server.
    // Physical desktop configuration is never modified here.
    const auto root = DefaultRootWindow(display_);
    auto *resources = XRRGetScreenResourcesCurrent(display_, root);
    if (!resources || !resources->ncrtc || !resources->noutput) {
      if (resources) XRRFreeScreenResources(resources);
      throw std::runtime_error("The independent desktop has no virtual RandR output");
    }
    const auto crtc = resources->crtcs[0], output = resources->outputs[0];
    int width = 0, height = 0;
    for (const auto& d : requested) { width = std::max(width, d.x + int(d.width)); height = std::max(height, d.y + int(d.height)); }
    const std::string name = "RemoteOS-" + std::to_string(width) + "x" + std::to_string(height);
    RRMode mode = None;
    for (int i = 0; i < resources->nmode; ++i)
      if (resources->modes[i].width == unsigned(width) && resources->modes[i].height == unsigned(height)) mode = resources->modes[i].id;
    RandRErrors errors(display_);
    if (!mode) {
      XRRModeInfo info{}; info.width = width; info.height = height; info.name = const_cast<char *>(name.c_str()); info.nameLength = name.size();
      info.hSyncStart = width + 8; info.hSyncEnd = width + 16; info.hTotal = width + 32;
      info.vSyncStart = height + 1; info.vSyncEnd = height + 4; info.vTotal = height + 8;
      info.dotClock = info.hTotal * info.vTotal * 60ul;
      mode = XRRCreateMode(display_, root, &info);
      if (!mode || errors.failed()) { XRRFreeScreenResources(resources); throw std::runtime_error("Cannot create the independent desktop's native mode"); }
      XRRAddOutputMode(display_, output, mode);
    }
    XRRSetCrtcConfig(display_, resources, crtc, CurrentTime, 0, 0, None, RR_Rotate_0, nullptr, 0);
    XRRFreeScreenResources(resources);
    XRRSetScreenSize(display_, root, width, height, std::max(1, width * 254 / 960), std::max(1, height * 254 / 960));
    resources = XRRGetScreenResourcesCurrent(display_, root);
    if (!resources) throw std::runtime_error("Cannot resize the independent desktop");
    const auto result = XRRSetCrtcConfig(display_, resources, crtc, CurrentTime, 0, 0, mode, RR_Rotate_0, const_cast<RROutput *>(&output), 1);
    XRRFreeScreenResources(resources);
    // Logical monitors preserve native coordinates for two separate streams.
    for (unsigned i = 0; i < 2; ++i) {
      const auto atom = XInternAtom(display_, ("RemoteOS-" + std::to_string(i)).c_str(), False);
      int count = 0; auto *list = XRRGetMonitors(display_, root, False, &count);
      bool exists = false; for (int n = 0; list && n < count; ++n) exists = exists || list[n].name == atom;
      if (list) XRRFreeMonitors(list);
      if (exists) XRRDeleteMonitor(display_, root, atom);
      if (i < requested.size()) {
        const auto& d = requested[i]; XRRMonitorInfo monitor{};
        monitor.name = atom; monitor.primary = i == 0; monitor.x = d.x; monitor.y = d.y;
        monitor.width = d.width; monitor.height = d.height; monitor.mwidth = d.width * 254 / 960; monitor.mheight = d.height * 254 / 960;
        monitor.noutput = i == 0 ? 1 : 0; monitor.outputs = i == 0 ? const_cast<RROutput *>(&output) : nullptr;
        XRRSetMonitor(display_, root, &monitor);
      }
    }
    if (result != Success || errors.failed()) throw std::runtime_error("Independent desktop rejected native monitor geometry");
    if (const auto *helper = std::getenv("REMOTEOS_NESTED_LAYOUT_HELPER"); helper && *helper) {
      // The launcher owns both this X server and the nested compositor. Run
      // the bounded layout helper with system Python, never our bundled libs.
      std::string serialized = "[";
      for (const auto &d : requested) {
        if (serialized.size() > 1) serialized += ',';
        serialized += "{\"width\":" + std::to_string(d.width) + ",\"height\":" + std::to_string(d.height) +
            ",\"x\":" + std::to_string(d.x) + ",\"y\":" + std::to_string(d.y) + '}';
      }
      serialized += ']';
      std::vector<std::string> environment;
      for (char **entry = environ; *entry; ++entry) {
        const std::string value(*entry);
        if (value.rfind("LD_LIBRARY_PATH=", 0) != 0 && value.rfind("GST_", 0) != 0 && value.rfind("FONTCONFIG_FILE=", 0) != 0)
          environment.push_back(value);
      }
      std::vector<char *> env;
      for (auto &entry : environment) env.push_back(entry.data());
      env.push_back(nullptr);
      char *args[] = {const_cast<char *>("python3"), const_cast<char *>(helper), const_cast<char *>("--layout"), serialized.data(), nullptr};
      pid_t child = 0;
      const int error = posix_spawnp(&child, "python3", nullptr, nullptr, args, env.data());
      if (error) throw std::runtime_error("Cannot launch the private Plasma display layout helper");
      int status = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (waitpid(child, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
          kill(child, SIGKILL); waitpid(child, &status, 0);
          throw std::runtime_error("Private Plasma display layout timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Private Plasma display layout failed; see independent-desktop.log");
    }
    configured_sources_ = requested;
  }
  struct SavedCrtc {
    RRCrtc id = 0; RRMode mode = 0; int x = 0, y = 0; Rotation rotation = RR_Rotate_0;
    std::vector<RROutput> outputs;
    XTransform transform{}; std::string filter; std::vector<XFixed> parameters; XRRPanning panning{};
  };
  void set_crtc(const SavedCrtc& crtc, bool disable) {
    auto *resources = XRRGetScreenResourcesCurrent(display_, DefaultRootWindow(display_));
    if (!resources) throw std::runtime_error("Cannot read RandR resources");
    const auto status = XRRSetCrtcConfig(display_, resources, crtc.id, CurrentTime, disable ? 0 : crtc.x, disable ? 0 : crtc.y,
        disable ? None : crtc.mode, crtc.rotation, disable ? nullptr : const_cast<RROutput *>(crtc.outputs.data()), disable ? 0 : int(crtc.outputs.size()));
    XRRFreeScreenResources(resources);
    if (status != Success) throw std::runtime_error("The Xorg driver rejected the client's monitor layout");
  }
  void restore_modes() noexcept {
    if (saved_crtcs_.empty()) { configured_sources_.clear(); return; }
    try {
      RandRErrors errors(display_);
      for (const auto& saved : saved_crtcs_) set_crtc(saved, true);
      XRRSetScreenSize(display_, DefaultRootWindow(display_), saved_width_, saved_height_, saved_mm_width_, saved_mm_height_);
      for (const auto& saved : saved_crtcs_) {
        XRRSetCrtcTransform(display_, saved.id, const_cast<XTransform *>(&saved.transform), saved.filter.c_str(), const_cast<XFixed *>(saved.parameters.data()), int(saved.parameters.size()));
        set_crtc(saved, false);
        auto *resources = XRRGetScreenResourcesCurrent(display_, DefaultRootWindow(display_));
        auto panning = saved.panning;
        if (panning.width || panning.height) XRRSetPanning(display_, resources, saved.id, &panning);
        XRRFreeScreenResources(resources);
      }
      if (errors.failed()) error_ = "Xorg could not restore the previous display configuration";
    } catch (const std::exception& e) { error_ = e.what(); }
    saved_crtcs_.clear(); configured_sources_.clear();
  }
  std::vector<DisplayConfig> monitors() const {
    if (!display_) return {};
    int count = 0;
    auto *list = XRRGetMonitors(display_, DefaultRootWindow(display_), True, &count);
    std::vector<DisplayConfig> result;
    std::vector<std::pair<bool, DisplayConfig>> ordered;
    for (int i = 0; list && i < count; ++i)
      if (list[i].width >= 320 && list[i].height >= 200)
        ordered.push_back({bool(list[i].primary), {0, unsigned(list[i].width), unsigned(list[i].height), list[i].x, list[i].y, 30}});
    if (list) XRRFreeMonitors(list);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& item : ordered) {
      item.second.id = unsigned(result.size()); result.push_back(item.second);
      if (result.size() == 2) break;
    }
    if (result.empty()) result.push_back({0, unsigned(DisplayWidth(display_, DefaultScreen(display_))),
                                        unsigned(DisplayHeight(display_, DefaultScreen(display_))), 0, 0, 30});
    return result;
  }
  static unsigned button_number(unsigned code) {
    switch (code) {
    case 0x110: return 1; case 0x111: return 3; case 0x112: return 2;
    case 0x113: case 0x116: return 8; case 0x114: case 0x115: return 9;
    default: throw std::invalid_argument("Unsupported pointer button");
    }
  }
  void wheel(int delta, unsigned positive, unsigned negative) {
    const auto button = delta > 0 ? positive : negative;
    for (int i = 0; i < std::min(32, std::abs(delta)); ++i) {
      XTestFakeButtonEvent(display_, button, True, CurrentTime);
      XTestFakeButtonEvent(display_, button, False, CurrentTime);
    }
  }
  void cursor(VideoFrame& frame, const DisplayConfig& source) {
    if (!cursor_available_) return;
    auto *image = XFixesGetCursorImage(capture_display_);
    if (!image) return;
    if (image->width <= 512 && image->height <= 512) {
      const int left = image->x - image->xhot - source.x, top = image->y - image->yhot - source.y;
      for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x) {
        const int dx = left + x, dy = top + y;
        if (dx < 0 || dy < 0 || dx >= int(frame.width) || dy >= int(frame.height)) continue;
        const auto pixel = image->pixels[y * image->width + x];
        const unsigned alpha = (pixel >> 24) & 255;
        auto *destination = frame.bgra.data() + std::size_t(dy) * frame.stride + dx * 4;
        for (int channel = 0; channel < 3; ++channel)
          destination[channel] = std::uint8_t(std::min(255ul, ((pixel >> (channel * 8)) & 255) + (destination[channel] * (255 - alpha) + 127) / 255));
        destination[3] = 255;
      }
    }
    XFree(image);
  }
  void capture() noexcept {
    try {
      std::vector<std::chrono::steady_clock::time_point> next(logical_.size());
      std::vector<std::unique_ptr<CaptureImage>> buffers;
      for (const auto& source : sources_) buffers.push_back(std::make_unique<CaptureImage>(capture_display_, source.width, source.height));
      while (running_) {
        for (std::size_t i = 0; i < logical_.size() && running_; ++i) {
          const auto now = std::chrono::steady_clock::now();
          if (now < next[i]) continue;
          VideoFrame frame;
          {
            const auto& source = sources_[i];
            auto& buffer = *buffers[i];
            auto *image = buffer.attached ? buffer.image : XGetImage(capture_display_, DefaultRootWindow(capture_display_), source.x, source.y,
                                    source.width, source.height, AllPlanes, ZPixmap);
            if (buffer.attached && !XShmGetImage(capture_display_, DefaultRootWindow(capture_display_), image, source.x, source.y, AllPlanes))
              throw std::runtime_error("Shared-memory desktop capture failed after a display change");
            if (!image) throw std::runtime_error("The shared monitor disappeared; reconnect after display changes");
            frame.display_id = logical_[i].id; frame.width = source.width; frame.height = source.height;
            frame.stride = source.width * 4; frame.timestamp = now;
            try {
              frame.bgra.resize(std::size_t(frame.stride) * frame.height);
              if (image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
                  image->red_mask == 0xff0000 && image->green_mask == 0xff00 && image->blue_mask == 0xff) {
                for (unsigned y = 0; y < frame.height; ++y)
                  std::memcpy(frame.bgra.data() + std::size_t(y) * frame.stride,
                              image->data + std::size_t(y) * image->bytes_per_line, frame.stride);
              } else throw std::runtime_error("Application capture requires an Xorg 24/32-bit TrueColor display");
              cursor(frame, source);
            } catch (...) { if (!buffer.attached) XDestroyImage(image); throw; }
            if (!buffer.attached) XDestroyImage(image);
          }
          callback_(std::move(frame));
          next[i] = detail::next_capture(now, next[i], logical_[i].fps);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_); error_ = error.what(); running_ = false;
    }
  }
  Display *display_ = nullptr;
  Display *capture_display_ = nullptr;
  Window nested_input_window_ = 0;
  bool independent_ = false;
  bool input_available_ = false, cursor_available_ = false;
  mutable std::mutex mutex_;
  std::string error_;
  std::vector<DisplayConfig> sources_, logical_;
  std::vector<DisplayConfig> configured_sources_;
  std::vector<SavedCrtc> saved_crtcs_;
  int saved_width_ = 0, saved_height_ = 0, saved_mm_width_ = 0, saved_mm_height_ = 0;
  std::set<unsigned> keys_, buttons_;
  FrameCallback callback_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};
#else
class DesktopHost final : public HostPlatform {
public:
  Capabilities capabilities() const override {
    Capabilities caps; caps.diagnostics.emplace_back("Build with REMOTEOS_WITH_DESKTOP_HOST=ON for application hosting"); return caps;
  }
  void start(const std::vector<DisplayConfig>&, FrameCallback) override { throw std::runtime_error("Application desktop capture was not built"); }
  void stop() noexcept override {}
  void inject(const InputEvent&) override { throw std::runtime_error("Application desktop input was not built"); }
  void release_all() noexcept override {}
};
#endif
} // namespace
#ifdef REMOTEOS_HAVE_WAYLAND_HOST
std::unique_ptr<HostPlatform> make_wayland_host_platform();
#endif
std::unique_ptr<HostPlatform> make_desktop_host_platform() {
  const auto *session = std::getenv("XDG_SESSION_TYPE");
  const auto *wayland = std::getenv("WAYLAND_DISPLAY");
  const bool use_wayland = session && *session ? std::string(session) == "wayland" : wayland && *wayland;
  if (use_wayland) {
#ifdef REMOTEOS_HAVE_WAYLAND_HOST
    return make_wayland_host_platform();
#else
    throw std::runtime_error("Build with REMOTEOS_WITH_WAYLAND_HOST=ON for Wayland desktop sharing");
#endif
  }
  return std::make_unique<DesktopHost>();
}
} // namespace remoteos::platform
