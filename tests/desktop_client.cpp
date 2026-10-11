#include "remoteos/client_session.hpp"
#include "remoteos/host_platform.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <X11/Xlib.h>
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
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool pressed(::Display *x, unsigned key) {
  char keys[32]{}; XQueryKeymap(x, keys); return keys[key / 8] & (1 << (key % 8));
}
bool left_down(::Display *x) {
  Window root, child; int rx, ry, wx, wy; unsigned mask;
  XQueryPointer(x, DefaultRootWindow(x), &root, &child, &rx, &ry, &wx, &wy, &mask);
  return mask & Button1Mask;
}
void platform_test(::Display *x) {
  XSetWindowBackground(x, DefaultRootWindow(x), 0x3366aa); XClearWindow(x, DefaultRootWindow(x)); XSync(x, False);
  auto host = platform::make_desktop_host_platform();
  const auto caps = host->capabilities();
  require(caps.capture && caps.input && !caps.virtual_displays && !caps.unattended_qualified, "Incorrect application capabilities");
  const auto outputs = host->desktop_displays();
  require(outputs.size() == 1 && outputs[0].width == 640 && outputs[0].height == 480, "Test requires its private 640x480 Xvfb");
  std::atomic<bool> frame_ok{false};
  host->start({{0, 320, 240, 0, 0, 60}}, [&](platform::VideoFrame frame) {
    if (frame.width == 640 && frame.height == 480 && frame.stride == 2560 && frame.bgra.size() == 640 * 480 * 4) {
      const auto *p = frame.bgra.data() + 16 * frame.stride + 16 * 4;
      frame_ok = p[0] == 0xaa && p[1] == 0x66 && p[2] == 0x33;
    }
  });
  const auto end = Clock::now() + std::chrono::seconds(3);
  while (!frame_ok && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  require(frame_ok, "Desktop capture lost native geometry or framebuffer pixels");
  platform::InputEvent motion; motion.kind = platform::InputKind::absolute_pointer; motion.x = 319; motion.y = 239;
  host->inject(motion);
  Window root, child; int rx, ry, wx, wy; unsigned mask;
  XQueryPointer(x, DefaultRootWindow(x), &root, &child, &rx, &ry, &wx, &wy, &mask);
  require(rx == 639 && ry == 479, "Scaled pointer did not reach native bottom-right corner");
  platform::InputEvent shift; shift.key = 42; shift.down = true; host->inject(shift);
  platform::InputEvent button; button.kind = platform::InputKind::button; button.key = 0x110; button.down = true; host->inject(button);
  require(pressed(x, 50) && left_down(x), "Application input was not delivered");
  host->release_all(); require(!pressed(x, 50) && !left_down(x), "release_all left held input");
  host->inject(shift); host->inject(button); host->stop();
  require(!pressed(x, 50) && !left_down(x), "Stopping capture left held input");
  std::cout << "Native framebuffer capture, pointer scaling, XTEST and input release passed\n";
}
struct Event { bool media; Json value; };
}
int main(int argc, char **argv) {
  if (argc != 5 || !std::getenv("REMOTEOS_PRIVATE_X_TEST")) return 2;
  XInitThreads();
  auto *x = XOpenDisplay(nullptr);
  if (!x) return 2;
  ClientSession session; MediaReceiver receiver;
  try {
    platform_test(x);
    ClientOptions options;
    options.bandwidth_limit_bps = 1000000000; // raw transport regression, not the appliance's 40 Mbps default
    options.host = "127.0.0.1"; options.port = unsigned(std::stoul(argv[1]));
    options.identity_dir = argv[2]; options.host_fingerprint = argv[3];
    options.pairing_code = read_private_file(argv[4]);
    while (!options.pairing_code.empty() && std::isspace(static_cast<unsigned char>(options.pairing_code.back()))) options.pairing_code.pop_back();
    remoteos::Display internal{"internal", 320, 240, 0, 0, 320, 240, 60, 0}; internal.codec = "raw-i420";
    if (const auto *codec = std::getenv("REMOTEOS_DESKTOP_TEST_CODEC"); codec && std::string(codec) == "mpeg2") {
      internal.codec = "mpeg2"; internal.fps = 30; internal.bitrate = 1000000;
    }
    auto external = internal; external.id = "vga"; external.x = 320; external.fps = 30; external.codec = "mpeg2"; external.bitrate = 1000000;
    options.displays = {internal, external};
    std::deque<Event> events; std::mutex mutex; std::condition_variable ready;
    auto emit = [&](bool media, const Json& value) {
      { std::lock_guard<std::mutex> lock(mutex); events.push_back({media, value}); } ready.notify_one();
    };
    bool paired = false, capabilities = false, offered = false, verified = false, peer_ready = false, authorized = false;
    session.connect(options, [&](const Json& value) { emit(false, value); });
    const auto end = Clock::now() + std::chrono::seconds(15);
    Json stats;
    while (Clock::now() < end) {
      Event event; bool have = false;
      {
        std::unique_lock<std::mutex> lock(mutex); ready.wait_for(lock, std::chrono::milliseconds(20), [&] { return !events.empty(); });
        if (!events.empty()) { event = std::move(events.front()); events.pop_front(); have = true; }
      }
      if (have) {
        const auto type = event.value.value("type", "");
        if (type == "error") throw std::runtime_error(event.value.dump());
        if (event.media) {
          if (type == "media_verified") { verified = true; session.send({{"type", "media_ready"}}); }
        } else if (type == "paired") paired = true;
        else if (type == "capabilities") {
          require(event.value.value("host_mode", "") == "application" && !event.value.value("test_source", true) &&
                  event.value.value("capture", false) && event.value.value("input", false) && !event.value.value("usb", true) &&
                  !event.value.value("virtual_displays", true) && !event.value.value("unattended_qualified", true), "Host advertised privileged/synthetic capabilities");
          capabilities = true;
        } else if (type == "offer") {
          require(capabilities && event.value.value("host_mode", "") == "application" && !event.value.value("audio", true) &&
                  event.value.at("displays").size() == 1 && event.value.at("streams").size() == 1, "Missing-host-monitor negotiation failed");
          const auto& stream = event.value.at("streams")[0];
          require(stream.at("source_width") == 640 && stream.at("source_height") == 480 &&
                  stream.at("profile").at("width") == 320 && stream.at("profile").at("codec") == internal.codec && stream.at("profile").at("fps") == internal.fps,
                  "Host did not preserve native capture and client stream profile");
          offered = true; receiver.start(event.value, {0}, combined_identity_pem(options.identity_dir), [&](const Json& value) { emit(true, value); });
        } else if (type == "media_ready") peer_ready = true;
      }
      if (verified && peer_ready && !authorized) {
        require(receiver.stats().value("frames", 0u) == 0, "Media released before authenticated control authorization");
        receiver.authorize(); authorized = true;
      }
      if (authorized && (stats = receiver.stats()).value("frames", 0u) >= 8) break;
    }
    require(paired && offered && authorized && stats.value("frames", 0u) >= 8, "Encrypted application desktop media timed out");
    require(!std::filesystem::exists(argv[4]), "One-time pairing code was not consumed");
    session.send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"}, {"x", 319.0}, {"y", 239.0}});
    session.send({{"type", "input"}, {"kind", "key"}, {"code", 42}, {"down", true}});
    session.send({{"type", "input"}, {"kind", "button"}, {"code", 0x110}, {"down", true}});
    const auto held_end = Clock::now() + std::chrono::seconds(3);
    while ((!pressed(x, 50) || !left_down(x)) && Clock::now() < held_end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(pressed(x, 50) && left_down(x), "Control channel did not deliver input");
    receiver.stop(); session.disconnect();
    const auto released_end = Clock::now() + std::chrono::seconds(3);
    while ((pressed(x, 50) || left_down(x)) && Clock::now() < released_end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(!pressed(x, 50) && !left_down(x), "Connection loss left host input held");
    std::cout << "Mutual TLS pairing, pinned " << internal.codec << " media, source downscaling, existing-monitor negotiation and disconnect input release passed: " << stats.dump() << '\n';
    XCloseDisplay(x); return 0;
  } catch (const std::exception& error) {
    receiver.stop(); session.disconnect(); XCloseDisplay(x); std::cerr << error.what() << '\n'; return 1;
  }
}
