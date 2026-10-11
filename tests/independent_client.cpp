#include "remoteos/client_session.hpp"
#include "remoteos/host_platform.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <X11/Xlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using namespace remoteos;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool good, const char *why) { if (!good) throw std::runtime_error(why); }
bool pressed(::Display *x, unsigned key) { char keys[32]{}; XQueryKeymap(x, keys); return keys[key / 8] & (1 << (key % 8)); }
std::pair<int, int> pointer(::Display *x) { Window root, child; int rx, ry, wx, wy; unsigned mask; XQueryPointer(x, DefaultRootWindow(x), &root, &child, &rx, &ry, &wx, &wy, &mask); return {rx, ry}; }
template<class F> void until(F condition, const char *why) { const auto end = Clock::now() + std::chrono::seconds(4); while (!condition() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5)); require(condition(), why); }
struct Event { bool media; Json value; };
}
int main(int argc, char **argv) {
  if (argc != 5 || !std::getenv("REMOTEOS_PRIVATE_X_TEST") || !std::getenv("REMOTEOS_LOCAL_TEST_DISPLAY")) return 2;
  XInitThreads(); auto *x = XOpenDisplay(nullptr);
  auto *local = XOpenDisplay(std::getenv("REMOTEOS_LOCAL_TEST_DISPLAY"));
  if (!x || !local) return 2;
  ClientSession session; MediaReceiver receiver;
  const bool audio_test = std::getenv("REMOTEOS_INDEPENDENT_TEST_AUDIO") != nullptr;
  try {
    const auto local_pointer = pointer(local);
    auto platform = platform::make_desktop_host_platform();
    require(platform->capabilities().virtual_displays, "Independent desktop capability missing");
    for (const auto& layout : std::vector<std::vector<platform::DisplayConfig>>{
      {{0, 1366, 768, 0, 0, 60}}, {{0, 1024, 576, 0, 0, 60}, {1, 1920, 1080, 1024, 0, 30}}, {{0, 1024, 576, 0, 0, 60}}}) {
      platform->match_desktop(layout);
      const auto sources = platform->desktop_displays();
      require(sources.size() == layout.size(), "Independent desktop lost a logical monitor");
      std::atomic<unsigned> frames{0}; std::atomic<bool> wrong{false};
      platform->start(layout, [&](platform::VideoFrame frame) {
        const auto& expected = layout.at(frame.display_id);
        if (frame.width != expected.width || frame.height != expected.height || frame.stride != expected.width * 4) wrong = true;
        ++frames;
      });
      until([&] { return frames >= 8; }, "Independent capture stopped after native resize/hotplug");
      platform::InputEvent shift; shift.key = 42; shift.down = true; platform->inject(shift);
      until([&] { return pressed(x, 50); }, "Private XTEST keyboard injection failed");
      require(!pressed(local, 50) && pointer(local) == local_pointer, "Independent input reached the local desktop");
      platform->stop(); until([&] { return !pressed(x, 50); }, "Private input remained held after stop");
      require(!wrong, "Capture scaled native framebuffer pixels");
    }
    platform.reset();
    ClientOptions options; options.host = "127.0.0.1"; options.port = std::stoul(argv[1]);
    options.identity_dir = argv[2]; options.host_fingerprint = argv[3]; options.pairing_code = read_private_file(argv[4]);
    while (!options.pairing_code.empty() && std::isspace(static_cast<unsigned char>(options.pairing_code.back()))) options.pairing_code.pop_back();
    options.displays = {{"internal", 1366, 768, 0, 0, 1366, 768, 60, 8000000}, {"vga", 640, 480, 1366, 0, 640, 480, 30, 4000000}};
    options.match_host_resolution = false; // Independent sessions always keep native geometry.
    options.live_video_reconfigure = true;
    std::deque<Event> events; std::mutex mutex; std::condition_variable ready;
    auto emit = [&](bool media, const Json& value) { { std::lock_guard<std::mutex> lock(mutex); events.push_back({media, value}); } ready.notify_one(); };
    bool offered = false, verified = false, peer_ready = false, authorized = false;
    session.connect(options, [&](const Json& value) { emit(false, value); });
    Json stats;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
      Event event; bool have = false;
      { std::unique_lock<std::mutex> lock(mutex); ready.wait_for(lock, std::chrono::milliseconds(10), [&] { return !events.empty(); }); if (!events.empty()) { event = std::move(events.front()); events.pop_front(); have = true; } }
      if (have) {
        const auto type = event.value.value("type", "");
        if (type == "error") throw std::runtime_error(event.value.dump());
        if (event.media && type == "media_verified") { verified = true; session.send({{"type", "media_ready"}}); }
        if (!event.media && type == "offer") {
          require(event.value.at("displays").size() == 2 && event.value.at("streams").size() == (audio_test ? 3 : 2), "Independent host removed a client display or its private audio");
          for (const auto& stream : event.value.at("streams")) {
            if (stream.at("kind") != "video") continue;
            const auto profile = stream.at("profile").get<remoteos::Display>();
            require(stream.at("source_width") == profile.width && stream.at("source_height") == profile.height && profile.width == profile.encoded_width && profile.height == profile.encoded_height,
              "Independent host scaled native pixels");
          }
          offered = true; receiver.start(event.value, {0, 0}, combined_identity_pem(options.identity_dir), [&](const Json& value) { emit(true, value); });
        }
        if (!event.media && type == "media_ready") peer_ready = true;
      }
      if (verified && peer_ready && !authorized) { receiver.authorize(); authorized = true; }
      if (authorized && (stats = receiver.stats()).value("frames", 0u) >= 15) break;
    }
    require(offered && authorized && stats.value("frames", 0u) >= 15, "Native dual-stream session did not start");
    if (const auto *path = std::getenv("REMOTEOS_PLASMA_TEST_EVENTS")) {
      // A Wayland configure changes compositor geometry before the client
      // commits its resized surface. Wait for the application's native buffer
      // size as well before testing input outside its former 1024-pixel area.
      until([&] { const auto app = Json::parse(read_private_file(path)); return app.value("width", 0) == 1366; }, "Native Plasma app did not finish its native resize");
      const auto app = Json::parse(read_private_file(path));
      require(app.value("backend", "") == "GdkWaylandDisplay", "Private Plasma fixture did not use native Wayland");
      require(app.value("width", 0) <= 1366 && app.value("height", 0) <= 768, "Native Plasma app spans both remote monitors");
      // Activate a real native control before sending keyboard input.
      session.send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"}, {"x", 400.0}, {"y", 300.0}});
      until([&] { return pointer(x) == std::pair<int,int>{400,300}; }, "Remote pointer did not reach the private compositor");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      session.send({{"type", "input"}, {"kind", "button"}, {"code", 272}, {"down", true}});
      until([&] { Window root, child; int rx, ry, wx, wy; unsigned mask; XQueryPointer(x, DefaultRootWindow(x), &root, &child, &rx, &ry, &wx, &wy, &mask); return (mask & Button1Mask) != 0; }, "Remote button press did not reach the private X server");
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      session.send({{"type", "input"}, {"kind", "button"}, {"code", 272}, {"down", false}});
      until([&] { return !Json::parse(read_private_file(path)).at("clicks").empty(); }, "Remote click did not activate the native Wayland button");
      session.send({{"type", "input"}, {"kind", "key"}, {"code", 30}, {"down", true}});
      try { until([&] { const auto value = Json::parse(read_private_file(path));
          return value.value("text", "") == "a";
        }, "Remote input did not reach the native Wayland application in private Plasma");
      } catch (...) {
        const auto position = pointer(x);
        std::cerr << "Private X pointer after remote input: " << position.first << ',' << position.second << '\n';
        Window focus = 0, root = 0, child = 0; int revert = 0, rx = 0, ry = 0, wx = 0, wy = 0; unsigned mask = 0;
        XGetInputFocus(x, &focus, &revert); XQueryPointer(x, DefaultRootWindow(x), &root, &child, &rx, &ry, &wx, &wy, &mask);
        std::cerr << "Private X focus/window: " << focus << '/' << child << '\n';
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& item : events) if (item.value.value("type", "") == "error") std::cerr << item.value.dump() << '\n';
        throw;
      }
      session.send({{"type", "input"}, {"kind", "key"}, {"code", 30}, {"down", false}});
      const auto previous_clicks = Json::parse(read_private_file(path)).at("clicks").size();
      session.send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"}, {"x", 1100.0}, {"y", 300.0}});
      until([&] { return pointer(x) == std::pair<int,int>{1100,300}; }, "Far-edge native pointer mapping failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      session.send({{"type", "input"}, {"kind", "button"}, {"code", 272}, {"down", true}});
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      session.send({{"type", "input"}, {"kind", "button"}, {"code", 272}, {"down", false}});
      until([&] { return Json::parse(read_private_file(path)).at("clicks").size() > previous_clicks; }, "Native Wayland button did not work at the far edge after resize");
    } else {
    const auto window = XCreateSimpleWindow(x, DefaultRootWindow(x), 100, 100, 400, 300, 0, 0, 0);
    XStoreName(x, window, "RemoteOS private work-area test"); XMapWindow(x, window); XFlush(x);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    XEvent maximize{}; maximize.xclient.type = ClientMessage; maximize.xclient.window = window;
    maximize.xclient.message_type = XInternAtom(x, "_NET_WM_STATE", False); maximize.xclient.format = 32;
    maximize.xclient.data.l[0] = 1;
    maximize.xclient.data.l[1] = XInternAtom(x, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    maximize.xclient.data.l[2] = XInternAtom(x, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    XSendEvent(x, DefaultRootWindow(x), False, SubstructureRedirectMask | SubstructureNotifyMask, &maximize); XFlush(x);
    XWindowAttributes maximized{};
    until([&] { XGetWindowAttributes(x, window, &maximized); return maximized.width > 1000; }, "Independent window manager did not maximize a window");
    require(maximized.width <= 1366 && maximized.height <= 768, "Maximized app spans both HP monitors instead of using its native work area");
    XDestroyWindow(x, window); XSync(x, False);
    }
    unsigned native_redraws = 0;
    const auto *native_events = std::getenv("REMOTEOS_PLASMA_TEST_EVENTS");
    const auto painted_before = native_events ? Json::parse(read_private_file(native_events)).value("painted_frames", 0u) : 0u;
    receiver.stats(); std::this_thread::sleep_for(std::chrono::seconds(2)); stats = receiver.stats();
    if (native_events) {
      const auto painted = Json::parse(read_private_file(native_events)).value("painted_frames", 0u) - painted_before;
      native_redraws = painted;
      std::cout << "Native Wayland animated desktop redraws in two seconds: " << painted << '\n';
      require(painted >= 50, "Private compositor stalled native application redraws despite stream cadence");
    }
    for (std::size_t i = 0; i < 2; ++i) require(stats["streams"][i].at("fps").get<double>() >= options.displays[i].fps * .85, "Real desktop capture/encode/decode missed native target cadence on the builder");
    if (audio_test) require(stats.value("audio_buffers", 0u) > 50, "Private playback audio was not captured/encoded/decoded");
    session.send({{"type", "input"}, {"kind", "motion"}, {"display", "vga"}, {"x", 639.0}, {"y", 479.0}});
    session.send({{"type", "input"}, {"kind", "key"}, {"code", 42}, {"down", true}});
    until([&] { return pressed(x, 50) && pointer(x) == std::pair<int,int>{2005,479}; }, "Private dual-monitor pointer coordinates/input failed");
    require(pointer(local) == local_pointer && !pressed(local, 50), "Streamed input interfered with local desktop");
    session.send({{"type", "release_all"}});
    until([&] { return !pressed(x, 50); }, "Explicit private input release failed");
    // Exercise live adaptation through the real authenticated control channel,
    // not just MediaSender/Receiver calls. Video changes must retain the same
    // media session, private playback and held input throughout both updates.
    bool host_rates = false;
    unsigned profile_updates = 0;
    auto profiles = options.displays;
    auto drain_events = [&] {
      std::deque<Event> pending;
      { std::lock_guard<std::mutex> lock(mutex); pending.swap(events); }
      for (const auto &event : pending) {
        const auto type = event.value.value("type", "");
        if (type == "error") throw std::runtime_error(std::string(event.media ? "Receiver" : "Host/control") +
            " error after " + std::to_string(profile_updates) + " live updates: " + event.value.dump());
        require(type != "offer" && type != "media_ready" && type != "media_verified",
                "Live video adaptation restarted the media session or handshake");
        if (!event.media && type == "status" && event.value.contains("host_media"))
          for (const auto &stream : event.value["host_media"]["streams"])
            if (stream.value("id", "") == "internal" && stream.value("encoded_fps", 0.0) > 50) host_rates = true;
        if (!event.media && type == "video_profile") {
          profiles = parse_displays(event.value.at("displays"));
          require(receiver.update_profiles(profiles), "Live profile could not update the existing receiver");
          ++profile_updates;
        }
      }
    };
    drain_events();
    const auto before_adaptation = receiver.stats();
    session.send({{"type", "input"}, {"kind", "key"}, {"code", 42}, {"down", true}});
    until([&] { return pressed(x, 50); }, "Private input did not become held before live adaptation");
    auto adapt = [&](const Json &feedback, unsigned expected_updates) {
      const auto end = Clock::now() + std::chrono::seconds(9);
      auto next_feedback = Clock::now();
      while (profile_updates < expected_updates && Clock::now() < end) {
        if (Clock::now() >= next_feedback) {
          session.send(feedback);
          next_feedback = Clock::now() + std::chrono::milliseconds(1100);
        }
        drain_events();
        require(pressed(x, 50), "Live adaptation released a held host key");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      require(profile_updates == expected_updates, "Host did not deliver the expected live video profile over TLS");
    };
    // Aggregate/audio loss cannot throttle the panel. Loss confined to VGA
    // reduces only that display's bitrate, without lowering its frame rate.
    adapt({{"type", "stats"}, {"cpu", 40.0}, {"loss", .5}, {"drop_fraction", .5},
           {"streams", Json::array({{{"id", "internal"}, {"kind", "video"}, {"loss", 0.0}, {"drop_fraction", 0.0}},
                                    {{"id", "vga"}, {"kind", "video"}, {"loss", .05}, {"drop_fraction", .5}},
                                    {{"id", "playback"}, {"kind", "audio"}, {"loss", .9}}})}}, 1);
    require(profiles.size() == 2 && profiles[0].fps == 60 && profiles[0].bitrate == 8000000 &&
            profiles[1].fps == 30 && profiles[1].bitrate == 3000000 && profiles[1].encoded_width == 640,
            "Audio/VGA loss throttled the native panel or changed fps instead of network bitrate");
    require(host_rates, "Real host did not report native encode rates");
    // A 26 ms decoder cannot sustain 60 fps. Keep its quality and native pixels
    // while reducing the panel to the supported MPEG-2 cadence of 30 fps.
    adapt({{"type", "stats"}, {"cpu", 40.0}, {"streams", Json::array({
            {{"id", "internal"}, {"kind", "video"}, {"loss", 0.0}, {"decode_ms", 26.0},
             {"presentation_drop_fraction", 0.0}, {"recovery_drop_fraction", 0.0}},
            {{"id", "vga"}, {"kind", "video"}, {"loss", 0.0}, {"drop_fraction", 0.0}}})}}, 2);
    require(profiles.size() == 2 && profiles[0].fps == 30 && profiles[0].bitrate == 8000000 &&
            profiles[0].width == 1366 && profiles[0].encoded_width == 1366 &&
            profiles[0].height == 768 && profiles[0].encoded_height == 768 &&
            profiles[1].fps == 30 && profiles[1].bitrate == 3000000,
            "Decode pressure changed native geometry/quality or another display's profile");
    session.send({{"type", "input"}, {"kind", "motion"}, {"display", "vga"}, {"x", 500.0}, {"y", 400.0}});
    until([&] { drain_events(); return pressed(x, 50) && pointer(x) == std::pair<int,int>{1866,400}; },
          "Private input stopped working after live video adaptation");
    const auto after_profile = receiver.stats();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto after_adaptation = receiver.stats(); drain_events();
    require(profile_updates == 2 && pressed(x, 50), "Live adaptation restarted or released held input after applying its profile");
    for (std::size_t i = 0; i < 2; ++i) {
      require(after_adaptation["streams"][i].at("buffers").get<std::uint64_t>() > before_adaptation["streams"][i].at("buffers").get<std::uint64_t>(),
              "Live adaptation reset or stopped a video stream");
      const auto fps = after_adaptation["streams"][i].at("fps").get<double>();
      require(fps >= 25 && fps <= 35, "Video did not settle at the live 30 fps profile");
    }
    if (audio_test)
      require(after_adaptation.value("audio_buffers", 0u) > before_adaptation.value("audio_buffers", 0u) &&
              after_adaptation.value("audio_buffers", 0u) > after_profile.value("audio_buffers", 0u),
              "Live video adaptation reset or stopped playback audio");
    require(pointer(local) == local_pointer && !pressed(local, 50), "Live adaptation routed input to the local desktop");
    receiver.stop(); session.disconnect(); until([&] { return !pressed(x, 50); }, "Disconnect left private keys pressed");
    // Reconnect retains the desktop and the same host identity.
    std::cout << Json{{"native_geometry", true}, {"dual_monitors", true}, {"local_input_untouched", true},
                      {"per_video_adaptation", true}, {"live_profile_updates", profile_updates}, {"live_input_preserved", true},
                      {"live_audio_preserved", audio_test},
                      {"host_encode_rates", true}, {"native_app_redraws_in_2s", native_redraws}, {"stats", stats}, {"after_adaptation", after_adaptation}}.dump() << '\n';
    XCloseDisplay(local); XCloseDisplay(x); return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    Window focus = 0; int revert = 0; XGetInputFocus(x, &focus, &revert); char *name = nullptr;
    XFetchName(x, focus, &name); std::cerr << "Private X focus: " << focus << " " << (name ? name : "no title") << '\n';
    if (name) XFree(name);
    receiver.stop(); session.disconnect(); XCloseDisplay(local); XCloseDisplay(x); return 1;
  }
}
