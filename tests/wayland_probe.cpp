#include "remoteos/host_platform.hpp"
#include "remoteos/protocol.hpp"
#include <gio/gio.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace remoteos;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
Json state(const char *method = "State") {
  GError *error = nullptr; auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) throw std::runtime_error("Private test bus unavailable");
  auto *value = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
    "org.remoteos.PortalTest", method, nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
  g_object_unref(bus); if (!value) throw std::runtime_error(error ? error->message : "Fixture failed");
  Json result; if (std::string(method) == "State") { const char *text; g_variant_get(value, "(&s)", &text); result = Json::parse(text); }
  g_variant_unref(value); return result;
}
int main(int argc, char **argv) {
  if (argc != 2 || !std::getenv("REMOTEOS_PRIVATE_PORTAL_TEST")) return 2;
  try {
    const std::string mode = argv[1];
    auto host = platform::make_desktop_host_platform();
    if (mode.rfind("kde-native", 0) == 0) {
      const unsigned w = mode.find("1366") != std::string::npos ? 1366 : 1024;
      const unsigned h = w == 1366 ? 768 : 576;
      host->select_desktop_source(platform::DesktopSource::kde_native_output);
      host->set_virtual_display_size(w, h);
      host->prepare();
      require(host->capabilities().virtual_displays, "Native virtual desktop not reported");
      const auto sources = host->desktop_displays();
      require(sources.size() == 1 && sources[0].width == w && sources[0].height == h,
              "Native capture fell back to another monitor or scaled pixels");
      const std::vector<platform::DisplayConfig> layout{{0, w, h, 0, 0, 30}};
      host->match_desktop(layout);
      std::atomic<unsigned> frames{};
      host->start(layout, [&](platform::VideoFrame frame) {
        if (frame.width == w && frame.height == h && frame.stride == w * 4 && frame.bgra.size() == std::size_t(w) * h * 4) ++frames;
      });
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (frames < 2 && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      require(frames >= 2, "Native KWin frame pixels did not arrive");
      platform::InputEvent input; input.kind = platform::InputKind::absolute_pointer;
      input.x = int(w - 1); input.y = int(h - 1); host->inject(input);
      const auto seen = state();
      require(seen["absolute"].back()[1] == double(w - 1) && seen["absolute"].back()[2] == double(h - 1),
              "Native pointer coordinates were scaled");
      input = {}; input.key = 29; input.down = true; host->inject(input);
      input.kind = platform::InputKind::button; input.key = 0x110; host->inject(input);
      host->stop();
      require(state()["keys"].empty() && state()["buttons"].empty(), "Native stop left host input held");
      auto wrong = layout; wrong[0].width = w == 1024 ? 1366 : 1024; wrong[0].height = w == 1024 ? 768 : 576;
      bool rejected = false;
      try { host->start(wrong, [](platform::VideoFrame) {}); } catch (const std::runtime_error&) { rejected = true; }
      require(rejected && host->session_error().empty(), "Native start silently scaled a mismatched client panel");
      host.reset();
      std::cout << mode << ": real KWin output and capture " << w << 'x' << h
                << " at scale 1, native pointer endpoints, foreign monitor exclusion, mismatch rejection and clean input release passed\n";
      return 0;
    }
    const bool single_virtual = mode == "virtual-only" || mode == "virtual-fixed";
    if (single_virtual) host->select_desktop_source(platform::DesktopSource::virtual_output);
    std::cerr << "Preparing private portal capture\n"; host->prepare("wayland:private-test");
    std::cerr << "Portal capture prepared\n";
    const auto caps = host->capabilities();
    require(caps.capture && caps.input && caps.virtual_displays == (single_virtual || mode == "virtual-match") && !caps.unattended_qualified, "Portal capabilities incorrect");
    const auto displays = host->desktop_displays();
    require(displays.size() == (single_virtual ? 1u : 2u) && displays[0].width == 640 && displays[0].height == 480 &&
            (single_virtual || (displays[1].width == 800 && displays[1].height == 600)),
            "Compositor coordinates incorrectly used as captured pixels");
    if (single_virtual) {
      auto requested = displays;
      if (mode == "virtual-fixed") {
        bool rejected = false;
        try { host->match_desktop(requested); }
        catch (const std::runtime_error& error) {
          const std::string text = error.what();
          rejected = text.find("1920x1080") != std::string::npos && text.find("640x480") != std::string::npos && text.find("Clear Match") != std::string::npos;
        }
        require(rejected && host->session_error().empty() && host->capabilities().capture,
                "Fixed virtual desktop size was confused with capture pixels or closed the grant");
      } else host->match_desktop(requested);
      host->start(requested, [](platform::VideoFrame) {});
      platform::InputEvent input; input.kind = platform::InputKind::absolute_pointer; input.x = 639; input.y = 479;
      host->inject(input);
      auto observed = state();
      require(observed["absolute"].back()[1] == (mode == "virtual-fixed" ? 1919.0 : 639.0) &&
              observed["absolute"].back()[2] == (mode == "virtual-fixed" ? 1079.0 : 479.0),
              "Virtual desktop pointer scaling did not preserve the final pixel");
      host.reset();
      std::cout << mode << ": VIRTUAL alone, one source, actual desktop size and pointer endpoints verified\n";
      return 0;
    }
    if (mode == "physical-match" || mode == "virtual-match") {
      std::vector<platform::DisplayConfig> requested = mode == "virtual-match"
        ? std::vector<platform::DisplayConfig>{{0, 640, 480, 0, 0, 30}, {1, 800, 600, 640, 0, 30}}
        : std::vector<platform::DisplayConfig>{{0, 320, 240, 0, 0, 30}, {1, 400, 300, 320, 0, 30}};
      if (mode == "physical-match") {
        auto mismatch = requested; mismatch[0].height += 2;
        bool rejected = false;
        try { host->match_desktop(mismatch); }
        catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("virtual") != std::string::npos; }
        require(rejected && host->session_error().empty(), "Physical monitor mismatch silently scaled or lost consent");
      }
      host->match_desktop(requested);
      if (mode == "virtual-match") {
        auto mismatch = requested; mismatch[0].height += 2;
        bool rejected = false;
        try { host->match_desktop(mismatch); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && host->session_error().empty() && host->desktop_displays()[0].height == 480,
                "Virtual resize was invented by changing PipeWire caps");
      }
      platform::InputEvent input; input.kind = platform::InputKind::absolute_pointer;
      input.x = requested[1].x + requested[1].width - 1; input.y = requested[1].height - 1;
      host->start(requested, [](platform::VideoFrame) {}); host->inject(input);
      const auto observed = state();
      require(observed["absolute"].back()[1] == double(requested[1].width - 1) &&
              observed["absolute"].back()[2] == double(requested[1].height - 1),
              "Matched display did not preserve native pointer endpoints");
      host.reset();
      std::cout << mode << ": confirmed display dimensions and pointer endpoints; "
                << (mode == "physical-match" ? "unsupported physical resize rejected" : "actual virtual geometry confirmed; unsupported resize rejected") << '\n';
      return 0;
    }
    std::atomic<unsigned> frames[2]{};
    const std::vector<platform::DisplayConfig> logical{{0, 320, 240, 0, 0, 60}, {1, 320, 240, 320, 0, 30}};
    auto callback = [&](platform::VideoFrame f) {
      if (f.display_id > 1 || f.width != displays[f.display_id].width || f.height != displays[f.display_id].height || f.stride != f.width * 4) return;
      const auto *pixel = f.bgra.data() + 10 * f.stride + 10 * 4;
      if ((f.display_id == 0 && pixel[0] < 5 && pixel[1] < 5 && pixel[2] > 250) ||
          (f.display_id == 1 && pixel[0] > 250 && pixel[1] < 5 && pixel[2] < 5)) ++frames[f.display_id];
    };
    host->start(logical, callback);
    auto wait_frames = [&] {
      auto deadline = Clock::now() + std::chrono::seconds(5);
      while ((frames[0] < 3 || frames[1] < 3) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      require(frames[0] >= 3 && frames[1] >= 3, "Two real PipeWire streams did not produce correct pixels");
    };
    wait_frames();
    if (mode == "slow-ack") {
      const auto before = frames[0].load();
      const auto begin = Clock::now();
      platform::InputEvent motion; motion.kind = platform::InputKind::absolute_pointer;
      for (int i = 0; i < 20; ++i) { motion.x = i; motion.y = i; host->inject(motion); }
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
      require(elapsed < 100, "Portal reply latency blocked the host input path");
      auto observed = state();
      require(observed["absolute"].size() == 20 && observed["absolute"].back()[1] == 19.0,
              "Asynchronous portal input lost events or changed their order");
      std::this_thread::sleep_for(std::chrono::milliseconds(160));
      require(frames[0] > before, "Slow portal replies stopped capture delivery");
      host.reset();
      std::cout << "20 ordered input events queued in " << elapsed
                << " ms despite 100 ms portal replies; capture continued\n";
      return 0;
    }
    platform::InputEvent input; input.key = 42; input.down = true; host->inject(input); input.repeat = true; host->inject(input);
    input = {}; input.kind = platform::InputKind::button; input.key = 0x110; input.down = true; host->inject(input);
    input = {}; input.kind = platform::InputKind::absolute_pointer; input.x = 639; input.y = 239; host->inject(input);
    input.x = 319; input.y = 239; host->inject(input);
    input.x = 1000; bool rejected = false; try { host->inject(input); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && host->session_error().empty(), "Invalid pointer destroyed the permission session");
    input = {}; input.kind = platform::InputKind::relative_pointer; input.delta_x = 4; input.delta_y = -3; host->inject(input);
    input.kind = platform::InputKind::wheel; input.delta_x = 2; input.delta_y = 100; host->inject(input);
    auto observed = state();
    require(observed["keys"] == Json::array({42}) && observed["buttons"] == Json::array({0x110}) && observed["key_calls"].size() == 1,
            "Key/button codes or repeat ownership incorrect");
    require(observed["absolute"][0][1] == 399.0 && observed["absolute"][0][2] == 299.0 &&
            observed["absolute"][1][1] == 319.0 && observed["absolute"][1][2] == 239.0, "HiDPI or second-screen pointer mapping incorrect");
    require(observed["relative"].back() == Json::array({4.0, -3.0}) && observed["axes"][0] == Json::array({0, -32}) && observed["axes"][1] == Json::array({1, 2}),
            "Relative motion/scroll axes incorrect");
    host->release_all(); observed = state(); require(observed["keys"].empty() && observed["buttons"].empty(), "Portal input release failed");
    input = {}; input.key = 29; input.down = true; host->inject(input);
    host->stop(); require(state()["keys"].empty(), "Stopping frame delivery left a held modifier");
    const auto count = frames[0].load(); std::this_thread::sleep_for(std::chrono::milliseconds(150)); require(frames[0] == count, "Capture delivery continued after stop");
    host->start(logical, callback); frames[0] = 0; frames[1] = 0; wait_frames();
    if (std::string(argv[1]) == "revoke") {
      state("Revoke"); const auto deadline = Clock::now() + std::chrono::seconds(3);
      while (host->session_error().empty() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      require(!host->session_error().empty(), "Permission revocation not observed");
      bool blocked = false; try { host->inject(input); } catch (...) { blocked = true; }
      require(blocked && !host->capabilities().capture, "Revoked session still allows capture/input");
    }
    host.reset();
    require(state()["session_closed"].get<unsigned>() >= 1, "Portal session was not closed");
    std::cout << "Two consent-scoped PipeWire streams, native pixels, HiDPI pointer mapping, evdev input, scroll, release, reconnect and " << argv[1] << " cleanup passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
