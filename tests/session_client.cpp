#include "remoteos/client_session.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using namespace remoteos;
using Clock = std::chrono::steady_clock;

// All asynchronous callbacks only enqueue data. In particular, no callback may
// join the control worker or stop its own GStreamer pipeline.
struct Event {
  unsigned connection, media;
  bool from_media;
  Json value;
};
class Events {
public:
  void push(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
    ready_.notify_one();
  }
  bool next(Event &event) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, std::chrono::milliseconds(20),
                    [&] { return !events_.empty(); });
    if (events_.empty())
      return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Event> events_;
};

int main(int argc, char **argv) {
  if (argc < 5 || argc > 6) {
    std::cerr << "usage: session-client PORT IDENTITY FINGERPRINT CODEFILE [kde-native-1024|kde-native-1366]\n";
    return 2;
  }
  Events events;
  ClientSession session;
  MediaReceiver receiver;
  try {
    const bool portal_test = std::getenv("REMOTEOS_PRIVATE_PORTAL_TEST") != nullptr;
    const bool native_test = argc == 6;
    const std::string native_mode = native_test ? argv[5] : "";
    if (native_test && (!portal_test || (native_mode != "kde-native-1024" && native_mode != "kde-native-1366")))
      throw std::runtime_error("Native session tests require a private portal and an HP panel profile");
    const int width = native_mode == "kde-native-1366" ? 1366 : native_test ? 1024 : 320;
    const int height = native_mode == "kde-native-1366" ? 768 : native_test ? 576 : 240;
    ClientOptions options;
    options.bandwidth_limit_bps = 1000000000; // explicit isolated raw-video qualification profile
    options.host = "127.0.0.1";
    options.port = static_cast<unsigned>(std::stoul(argv[1]));
    options.identity_dir = argv[2];
    options.host_fingerprint = argv[3];
    options.pairing_code = read_private_file(argv[4]);
    while (
        !options.pairing_code.empty() &&
        std::isspace(static_cast<unsigned char>(options.pairing_code.back())))
      options.pairing_code.pop_back();
    Display internal{"internal", 320, 240, 0, 0, 320, 240, 30, 1000000};
    Display external = internal;
    external.id = "vga";
    external.x = 320;
    internal.codec = "raw-i420";
    internal.fps = 60;
    internal.bitrate = 0;
    options.displays = {internal, external};
    if (native_test) {
      // Intentionally request smaller encoded pixels: native mode must replace
      // them with the actual panel dimensions, including on renegotiation.
      internal = {"internal", width, height, 0, 0, 320, 240, 60, 20000000};
      options.displays = {internal};
      options.bandwidth_limit_bps = 40000000;
      options.match_host_resolution = true;
    }
    unsigned connection = 1, media = 0;
    bool saw_pairing = false, saw_capabilities = false, usb_enabled = false;
    const int native_max_fps = width == 1366 ? 60 : 30;
    int expected_native_fps = native_max_fps;
    auto connect = [&] {
      const auto current = connection;
      session.connect(options, [&, current](const Json &event) {
        events.push({current, 0, false, event});
      });
    };
    auto receive = [&](unsigned expected_videos, const char *phase) {
      const auto end = Clock::now() + std::chrono::seconds(10);
      bool offered = false, verified = false, peer_ready = false,
           authorized = false;
      unsigned current_media = ++media;
      Json last_stats;
      while (Clock::now() < end) {
        Event event;
        if (events.next(event)) {
          if (event.connection != connection ||
              (event.from_media && event.media != current_media))
            continue;
          const auto type = event.value.value("type", "");
          if (type == "error")
            throw std::runtime_error(std::string(phase) + ": " +
                                     event.value.dump());
          if (event.from_media) {
            if (type == "media_verified") {
              if (!offered)
                throw std::runtime_error("verification before offer");
              verified = true;
              session.send({{"type", "media_ready"}});
            } else if (type == "keyframe")
              session.send(event.value);
          } else if (type == "paired") {
            if (connection != 1)
              throw std::runtime_error("reconnect unexpectedly paired again");
            saw_pairing = true;
          } else if (type == "capabilities") {
            if (!event.value.at("media").value("compiled", false) ||
                !event.value.at("media").at("missing_elements").empty() ||
                event.value.value("test_source", false) == portal_test)
              throw std::runtime_error("host lacks explicit test media mode");
            if (portal_test && (!event.value.value("capture", false) || !event.value.value("input", false) ||
                                event.value.value("host_mode", "") != "application" || event.value.value("virtual_displays", true) != native_test))
              throw std::runtime_error("Wayland application capabilities incorrect");
            saw_capabilities = true;
            usb_enabled = event.value.value("usb", false);
          } else if (type == "offer") {
            if (offered)
              throw std::runtime_error("unexpected duplicate offer");
            if (!saw_capabilities || event.value.value("audio", false) == portal_test ||
                event.value.at("displays").size() != expected_videos ||
                event.value.at("streams").size() != expected_videos + (portal_test ? 0 : 1))
              throw std::runtime_error("incorrect negotiated streams: " +
                                       event.value.dump());
            if (native_test && (event.value.value("media_transport", "") != "rtp-udp" ||
                                event.value.value("jitter_ms", 0) != 10 ||
                                event.value.at("streams").at(0).value("payload", "") != "gst-frame"))
              throw std::runtime_error("Native session did not negotiate framed MPEG-2 over trusted UDP");
            for (const auto &display : event.value.at("displays"))
              if (display.at("width") != width || display.at("height") != height)
                throw std::runtime_error("host changed logical dimensions");
              else if (native_test && (display.at("encoded_width") != width || display.at("encoded_height") != height ||
                                      display.at("codec") != "mpeg2" || display.at("fps") != expected_native_fps))
                throw std::runtime_error("native host reduced encoded dimensions or changed MPEG-2 profile");
              else if (display.at("id") == "internal" &&
                       !native_test &&
                       (display.at("codec") != "raw-i420" ||
                        display.at("fps") != 60 || display.at("bitrate") != 0))
                throw std::runtime_error(
                    "host changed raw performance profile");
              else if (display.at("id") == "vga" &&
                       (display.at("codec") != "mpeg2" ||
                        display.at("fps") != 30))
                throw std::runtime_error(
                    "host changed VGA compression profile");
            if (portal_test) {
              for (unsigned i = 0; i < expected_videos; ++i) {
                const auto& source = event.value.at("streams")[i];
                if (source.at("source_width") != (native_test ? width : i == 0 ? 640 : 800) ||
                    source.at("source_height") != (native_test ? height : i == 0 ? 480 : 600))
                  throw std::runtime_error("Portal pixels confused with compositor coordinates");
              }
            }
            offered = true;
            const auto current_connection = connection;
            receiver.start(
                event.value, std::vector<std::uintptr_t>(expected_videos, 0),
                combined_identity_pem(options.identity_dir),
                [&, current_connection, current_media](const Json &value) {
                  events.push({current_connection, current_media, true, value});
                });
          } else if (type == "media_ready")
            peer_ready = true;
        }
        if (verified && peer_ready && !authorized) {
          const auto stats = receiver.stats();
          if (stats.value("frames", 0u) || stats.value("audio_buffers", 0u))
            throw std::runtime_error("media rendered before authorization");
          receiver.authorize();
          authorized = true;
        }
        if (!authorized)
          continue;
        last_stats = receiver.stats();
        bool enough = last_stats.at("streams").size() == expected_videos + (portal_test ? 0 : 1);
        for (const auto &stream : last_stats.at("streams"))
          enough = enough && stream.value("buffers", 0u) >= 8;
        if (enough) {
          std::cout << phase << ": " << last_stats.dump() << std::endl;
          if (native_test) {
            // A short startup burst is not an achieved frame-rate result.
            // Measure a sustained interval after negotiation has settled.
            receiver.stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));
            const auto steady = receiver.stats();
            const auto& stream = steady.at("streams").at(0);
            const double fps = stream.at("fps");
            if (fps < expected_native_fps * 0.85 || fps > expected_native_fps * 1.15 ||
                stream.value("drop_fraction", 1.0) > 0.08)
              throw std::runtime_error(std::string(phase) + " steady native cadence regressed: " + steady.dump());
            std::cout << "steady " << phase << ": " << steady.dump() << std::endl;
          }
          return;
        }
      }
      throw std::runtime_error(std::string(phase) +
                               " timed out: " + last_stats.dump());
    };

    connect();
    receive(native_test ? 1 : 2, native_test ? "native HP MPEG-2 over trusted UDP at 40 Mbps budget" :
            portal_test ? "consented PipeWire screens over pinned raw60 and MPEG-2" : "paired raw 60 fps, MPEG-2 VGA and Opus");
    if (!saw_pairing || std::filesystem::exists(argv[4]))
      throw std::runtime_error("pairing code was not consumed");
    if (native_test) {
      // Exercise old HP feedback (ambiguous aggregate audio queue) and new
      // feedback (labelled video/audio queues) through the actual TLS host.
      for (int i = 0; i < 3; ++i)
        session.send({{"type", "stats"}, {"cpu", 20}, {"loss", 0}, {"queue_ms", 120}});
      for (int i = 0; i < 3; ++i)
        session.send({{"type", "stats"}, {"cpu", 20}, {"loss", 0}, {"queue_ms", 120},
                      {"streams", Json::array({{{"kind", "video"}, {"queue_ms", 0}},
                                                {{"kind", "audio"}, {"queue_ms", 120}}})}});
      const auto end = Clock::now() + std::chrono::milliseconds(350);
      Event event;
      while (Clock::now() < end) {
        if (!events.next(event) || event.connection != connection || event.from_media) continue;
        if (event.value.value("type", "") == "offer" || event.value.value("type", "") == "error")
          throw std::runtime_error("Ordinary audio buffering incorrectly renegotiated video: " + event.value.dump());
      }
      std::cout << "Legacy and labelled audio queues do not throttle native video\n";
      for (int i = 0; i < 4; ++i) {
        session.send({{"type", "stats"}, {"cpu", 50}, {"loss", 0}, {"queue_ms", 0}, {"drop_fraction", 0.25}});
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      }
      // Sustained 25% presentation loss selects a legal MPEG-2 cadence below
      // the measured delivery capacity, instead of making one small step.
      expected_native_fps = native_max_fps == 60 ? 30 : 20;
      receiver.stop();
      receive(1, "late presentation lowers frame rate without changing native pixels");
    }
    if (usb_enabled) {
      auto control = [&](const std::string &type) {
        const auto end = Clock::now() + std::chrono::seconds(5);
        Event event;
        while (Clock::now() < end) {
          if (!events.next(event) || event.connection != connection ||
              event.from_media)
            continue;
          if (event.value.value("type", "") == "error" ||
              event.value.contains("device_error"))
            throw std::runtime_error("USB grant regression: " +
                                     event.value.dump());
          if (event.value.value("type", "") == type)
            return event.value;
        }
        throw std::runtime_error("USB grant response timed out: " + type);
      };
      auto prepare = [&] {
        session.send({{"type", "attach_device"}, {"bus_id", "1-2"}});
        return control("open_tunnel");
      };
      auto cancel = [&](const Json &grant, const std::string &token) {
        session.send({{"type", "detach_device"},
                      {"bus_id", "1-2"},
                      {"cancel", true},
                      {"session_id", grant.at("session_id")},
                      {"token", token}});
      };
      auto expect_devices = [&](std::size_t count) {
        session.send({{"type", "device_list"}});
        if (control("device_list").at("devices").size() != count)
          throw std::runtime_error(
              "USB grant cleanup changed the wrong device");
      };
      const auto old_grant = prepare();
      cancel(old_grant, std::string(64, '0'));
      expect_devices(1);
      cancel(old_grant, old_grant.at("token"));
      expect_devices(0);
      const auto new_grant = prepare();
      if (new_grant.at("token") == old_grant.at("token"))
        throw std::runtime_error("USB grant token was reused");
      cancel(old_grant, old_grant.at("token"));
      expect_devices(1);
      cancel(new_grant, new_grant.at("token"));
      expect_devices(0);
      std::cout << "USB preparation cancellation, stale-token refusal and "
                   "reattach passed\n";
    }
    session.send({{"type", "release_all"}});
    receiver.stop();
    expected_native_fps = native_max_fps;
    session.send({{"type", "display_changed"},
                  {"displays", std::vector<Display>{internal}}});
    receive(1, native_test ? "native pixels preserved on renegotiation" : "external display removed and streams renegotiated");

    if (portal_test) {
      if (native_test)
        session.send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"},
                      {"x", width - 1}, {"y", height - 1}});
      session.send({{"type", "input"}, {"kind", "key"}, {"code", 29}, {"down", true}});
      session.send({{"type", "input"}, {"kind", "button"}, {"code", 0x110}, {"down", true}});
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else session.send({{"type", "release_all"}});
    session.disconnect();
    receiver.stop();
    ++connection;
    // Let the real host observe EOF and tear down its former session before a
    // fresh controlling connection. The persistent identity stays unchanged.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    options.pairing_code.clear();
    options.host_fingerprint.clear(); // exercise the stored pin, not caller pin
    options.displays = {internal};
    saw_capabilities = false;
    connect();
    receive(1, "saved identity reconnect");
    session.send({{"type", "release_all"}});
    session.disconnect();
    receiver.stop();
    std::cout << "Real host paired TLS control and negotiated UDP media, display change and persisted "
                 "identity reconnect passed\n";
    return 0;
  } catch (const std::exception &error) {
    session.disconnect();
    receiver.stop();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
