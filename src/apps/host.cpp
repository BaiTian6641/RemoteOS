#include "remoteos/devices.hpp"
#include "remoteos/discovery.hpp"
#include "remoteos/host_platform.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include "remoteos/tunnel.hpp"
#include "remoteos/resources.hpp"
#include "transport.hpp"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <openssl/crypto.h>
#include <set>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if !defined(__APPLE__)
#include "remoteos/audio_session.hpp"
#endif
#include <unistd.h>
#endif

using namespace remoteos;
struct Server {
  asio::io_context io;
  asio::ssl::context tls{asio::ssl::context::tls};
  net::tcp::acceptor acceptor{io};
  asio::signal_set signals{io, SIGINT, SIGTERM};
  asio::steady_timer desktop_poll{io};
  std::shared_ptr<net::Channel> active;
  std::unique_ptr<platform::HostPlatform> platform =
      platform::make_host_platform();
  MediaSender media;
  std::unique_ptr<DiscoveryResponder> discovery;
  std::uint64_t bandwidth_limit = default_bandwidth_bps, session_bandwidth = default_bandwidth_bps;
  bool match_resolution = false;
  std::unique_ptr<devices::TunnelServer> tunnels;
  // Setup/detach/abrupt cleanup must not race over a reusable kernel VHCI port.
  // Bulk tunnel I/O remains asynchronous on the control executor.
  asio::thread_pool device_workers{1};
  struct DeviceState {
    std::string token;
    std::uint16_t local_port = 0;
    std::atomic<int> imported_port{-1};
    bool busy = false;
    bool detaching = false;
    std::string closed_during_detach;
    std::atomic<bool> cancelled{false};
  };
  std::map<std::string, std::shared_ptr<DeviceState>> devices;
  std::string control_session;
  std::unique_ptr<devices::TunnelServer> resource_server;
  struct Resource { std::string token,cap;unsigned port=0;std::unique_ptr<devices::CameraBridge> camera; };
  std::map<std::string,Resource> resources;
  void send_resources() {
    Json list=Json::array();for(auto& [id,r]:resources)list.push_back({{"resource",id},{"url",id=="camera"?(r.camera?r.camera->url():""):"http://127.0.0.1:"+std::to_string(r.port)+"/"+r.cap+"/"}});
    if(active&&authorized)active->send({{"type","resource_status"},{"resources",list}});
    write_private_file(std::filesystem::path(identity).parent_path()/"resources.json",list.dump());
    std::cout<<Json{{"event","resource-status"},{"resources",list}}.dump()<<std::endl;
  }
  void refresh_resource_budget() {
    if(displays.empty()||!active||!authorized)return;
    // A 12 Mb/s reservation covers YUYV camera plus rate-limited shared files.
    // Restore the full video wire ceiling once both resources are stopped.
    media.stop();make_offer(active);active->send(offer);
  }
  Json audio_config;
#if !defined(_WIN32) && !defined(__APPLE__)
  audio::SessionSource audio_source;
  asio::thread_pool audio_worker{1};
  asio::steady_timer audio_poll{io};
  int audio_fd = -1;
  std::uint64_t audio_generation = 0, audio_epoch = 0;
  std::string audio_reason = "Waiting for an active user playback session";
  void poll_audio() {
    audio_poll.expires_after(std::chrono::seconds(3));
    audio_poll.async_wait([this](std::error_code error) {
      if (error)
        return;
      if (!authorized || displays.empty() ||
          audio_config.value("audio_source", "") != "pipewire") {
        poll_audio();
        return;
      }
      const auto epoch = audio_epoch, known_generation = audio_generation;
      const bool connected = audio_fd >= 0;
      asio::post(audio_worker, [this, epoch, known_generation, connected] {
        const auto session = audio_source.refresh();
        std::string reason = session.reason;
        auto socket = std::shared_ptr<int>(new int(-1), [](int *fd) {
          if (*fd >= 0)
            close(*fd);
          delete fd;
        });
        if (session.available &&
            (!connected || session.generation != known_generation)) {
          try {
            *socket = audio_source.connect(session);
          } catch (const std::exception &e) {
            reason = e.what();
          }
        }
        asio::post(io, [this, epoch, session, reason, socket] {
          if (epoch == audio_epoch && authorized && active &&
              !displays.empty()) {
            const bool changed =
                *socket >= 0 ||
                (audio_fd >= 0 && session.generation != audio_generation);
            audio_generation = session.generation;
            if (reason != audio_reason) {
              audio_reason = reason;
              active->send(
                  {{"type", "status"},
                   {"audio_status",
                    reason.empty() ? "User playback available" : reason}});
            }
            if (changed) {
              platform->release_all();
              held_keys.clear();
              media.stop();
              if (audio_fd >= 0)
                close(audio_fd);
              audio_fd = *socket;
              *socket = -1;
              try {
                make_offer(active);
                active->send(offer);
              } catch (const std::exception &e) {
                active->close(e.what());
              }
            }
          }
          poll_audio();
        });
      });
    });
  }
#endif
  std::string identity, pair_file, pair_code;
  Json trusted = Json::object();
  std::vector<Display> displays;
  std::vector<Adaptation> adaptation;
  Json offer;
  std::chrono::steady_clock::time_point pair_expiry;
  int pair_attempts = 0;
  bool authorized = false, test_source = false;
  bool application_mode = false;
  bool native_resolution = false;
  bool independent_desktop = false;
  int mpeg2_max_fps = 30;
  std::string preferred_media_transport = "dtls-srtp", media_transport = "dtls-srtp";
  unsigned configured_jitter_ms = 40, jitter_ms = 40, video_reorder_ms = 150;
  bool frame_payload = false, live_video_reconfigure = false, automatic_video_bitrate = false;
  std::vector<platform::DisplayConfig> desktop_sources;
  std::chrono::steady_clock::time_point adaptation_not_before{}, last_feedback{};
  std::set<int> held_keys;
  std::mutex profile_mutex;
  std::atomic<bool> generating{false};
  std::thread generator;
  static void cleanup_device(const std::string &bus,
                             const std::shared_ptr<DeviceState> &state) {
#if !defined(_WIN32) && !defined(__APPLE__)
    const int port = state->imported_port.exchange(-1);
    if (port < 0)
      return;
    try {
      devices::abandon_import(unsigned(port), bus, state->local_port);
    } catch (const std::exception &e) {
      std::cerr << "USB unplug cleanup: " << e.what() << '\n';
    }
#else
    (void)bus;
    (void)state;
#endif
  }
  void clear_devices() {
    if(resource_server&&!control_session.empty())resource_server->revoke_session(control_session);
    resources.clear();
    for (auto &entry : devices)
      entry.second->cancelled = true;
    if (tunnels && !control_session.empty())
      tunnels->revoke_session(control_session);
    for (auto &entry : devices) {
      auto state = entry.second;
      auto bus = entry.first;
      asio::post(device_workers, [state, bus] { cleanup_device(bus, state); });
    }
    devices.clear();
    control_session.clear();
  }
  void device_tunnel_closed(const std::string &session, const std::string &bus,
                            const std::string &token, const std::string &reason,
                            bool notify = true) {
    const auto found = devices.find(bus);
    if (session != control_session || found == devices.end() ||
        found->second->token != token)
      return;
    const auto state = found->second;
    if (notify && state->detaching) {
      // A successful kernel detach closes its relay before its worker reply
      // necessarily reaches this executor. Let that operation report success.
      state->closed_during_detach = reason;
      return;
    }
    state->cancelled = true;
    devices.erase(found);
    // The tunnel is already gone: this has physical-unplug semantics. Serialize
    // kernel cleanup behind any in-flight import so its port cannot be reused.
    asio::post(device_workers, [state, bus] { cleanup_device(bus, state); });
    if (notify && active && authorized)
      active->send({{"type", "status"},
                    {"bus_id", bus},
                    {"device_token", token},
                    {"device_error", "USB relay stopped: " + reason}});
  }
  void send_devices(const std::shared_ptr<net::Channel> &ch) {
    Json list = Json::array();
    for (auto &entry : devices)
      list.push_back(
          {{"bus_id", entry.first},
           {"state",
            entry.second->imported_port >= 0 ? "attached" : "connecting"},
           {"port", entry.second->imported_port.load()}});
    ch->send({{"type", "device_list"},
              {"devices", list},
              {"enabled", bool(tunnels)}});
  }
#ifdef _WIN32
  asio::steady_timer stop_poll{io};
  HANDLE stop_event = nullptr;
  void poll_stop() {
    stop_poll.expires_after(std::chrono::milliseconds(250));
    stop_poll.async_wait([this](std::error_code e) {
      if (e)
        return;
      if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
        if (active)
          active->close("host service stopping");
        stop_session();
        clear_devices();
        io.stop();
      } else
        poll_stop();
    });
  }
#endif
  void stop_session() {
    if (application_mode && !displays.empty())
      std::cout << Json{{"event", "session-stopped"}}.dump() << std::endl;
    generating = false;
    if (generator.joinable())
      generator.join();
    platform->release_all();
    held_keys.clear();
    platform->stop();
    media.stop();
#if !defined(_WIN32) && !defined(__APPLE__)
    ++audio_epoch;
    if (audio_fd >= 0)
      close(audio_fd);
    audio_fd = -1;
    audio_generation = 0;
#endif
    displays.clear();
  }
  void accept() {
    auto ch = std::make_shared<net::Channel>(io, tls);
    acceptor.async_accept(ch->stream.next_layer(), [this,
                                                    ch](std::error_code e) {
      if (!e) {
        if (active) {
          std::error_code ec;
          ch->stream.next_layer().close(ec);
        } else {
          active = ch;
          authorized = false;
          ch->on_close = [this, weak = std::weak_ptr<net::Channel>(ch)](
                             std::string reason) {
            if (active == weak.lock()) {
              stop_session();
              clear_devices();
              active.reset();
              authorized = false;
            }
            std::cerr << "connection closed: " << reason << '\n';
          };
          ch->on_message = [this, ch](Json j) {
            try {
              message(ch, j);
            } catch (const std::exception &x) {
              platform->release_all();
              held_keys.clear();
              if (!authorized)
                ch->close(std::string("authentication rejected: ") + x.what());
              else
                ch->send({{"type", "error"}, {"message", x.what()}});
            }
          };
          ch->handshake(asio::ssl::stream_base::server, [] {});
        }
      }
      if (acceptor.is_open())
        accept();
    });
  }
  void message(const std::shared_ptr<net::Channel> &ch, const Json &j) {
    const std::string type = j.at("type");
    if (type == "pair") {
      if (authorized)
        throw std::runtime_error("already authorized");
      if (++pair_attempts > 5 || pair_code.empty() ||
          std::chrono::steady_clock::now() > pair_expiry)
        throw std::runtime_error(
            "pairing disabled, expired or attempt limit reached");
      auto code = j.at("code").get<std::string>();
      if (code.size() != pair_code.size() ||
          CRYPTO_memcmp(code.data(), pair_code.data(), code.size()))
        throw std::runtime_error("pairing rejected");
      trusted[ch->peer_fingerprint()] = {{"paired", true}};
      write_private_file(std::filesystem::path(identity) / "clients.json",
                         trusted.dump(2));
      OPENSSL_cleanse(pair_code.data(), pair_code.size());
      pair_code.clear();
      if (!pair_file.empty())
        std::filesystem::remove(pair_file);
      ch->send({{"type", "paired"},
                {"fingerprint",
                 certificate_fingerprint(std::filesystem::path(identity) /
                                         "certificate.pem")}});
      return;
    }
    if (type == "hello") {
      if (!trusted.contains(ch->peer_fingerprint()))
        throw std::runtime_error("client is not paired");
      authorized = true;
      if (control_session.empty())
        control_session = random_token(16);
      auto c = platform->capabilities();
      auto caps = media_capabilities();
      caps["mpeg2_max_fps"] = mpeg2_max_fps;
      ch->send({{"type", "capabilities"},
                {"version", protocol_version},
                {"capture", c.capture || test_source},
                {"input", c.input && !test_source},
                {"virtual_displays", c.virtual_displays},
                {"host_mode", application_mode ? "application" : "service"},
                {"unattended_qualified", c.unattended_qualified},
                {"diagnostics", c.diagnostics},
                {"media", caps},
                {"usb", bool(tunnels)},
                {"userspace_devices", bool(resource_server)},
                {"test_source", test_source}});
      return;
    }
    if (!authorized)
      throw std::runtime_error("authenticate before issuing commands");
    if(type=="attach_resource"||type=="detach_resource") {
      if(!resource_server||displays.empty())throw std::runtime_error("Userspace resources need an active desktop");
      const auto id=j.value("resource",std::string{});
      if(id!="camera"&&id!="files")throw std::runtime_error("Unknown userspace resource");
      resource_server->revoke_device(control_session,id);resources.erase(id);
      if(type=="attach_resource"){
        const auto cap=j.value("capability",std::string{});
        if(cap.size()!=64||cap.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid resource capability");
        Resource r;r.cap=cap;r.token=random_token(32);
        r.port=resource_server->prepare(control_session,id,r.token,ch->peer_fingerprint(),true);
        const auto token=r.token;resources.emplace(id,std::move(r));
        ch->send({{"type","open_resource"},{"resource",id},{"session_id",control_session},{"token",token},{"port",resource_server->port()}});
        if(id=="camera")resources.at(id).camera=std::make_unique<devices::CameraBridge>(resources.at(id).port,cap);
      }
      send_resources();refresh_resource_budget();return;
    }
    if (type == "ping") {
      ch->send({{"type", "pong"}, {"nonce", j.value("nonce", std::string{})}});
      return;
    }
    if (type == "pong")
      return;
    if (type == "disconnect") {
      ch->close("clean disconnect");
      return;
    }
    if (type == "release_all") {
      platform->release_all();
      held_keys.clear();
      return;
    }
    if (type == "start" || type == "display_changed") {
      if (!media_available())
        throw std::runtime_error("host built without GStreamer media support");
      stop_session();
      if (type == "start") {
        const auto transports = j.value("media_transports", Json::array());
        const auto payloads = j.value("mpeg2_payloads", Json::array());
        if (!transports.is_array() || !payloads.is_array()) throw std::runtime_error("Invalid media capabilities");
        media_transport = preferred_media_transport == "rtp-udp" &&
          std::find(transports.begin(), transports.end(), "rtp-udp") != transports.end() ? "rtp-udp" : "dtls-srtp";
        frame_payload = std::find(payloads.begin(), payloads.end(), "gst-frame") != payloads.end();
        live_video_reconfigure = j.value("live_video_reconfigure", false);
        automatic_video_bitrate = j.value("automatic_video_bitrate", false);
        jitter_ms = j.value("low_latency", false) ? configured_jitter_ms : 40;
      }
      displays = parse_displays(j.at("displays"));
      for (auto& display : displays)
        if (display.codec == "mpeg2") display.fps = std::min(display.fps, mpeg2_max_fps);
      if (native_resolution) for (auto& display : displays) {
        display.encoded_width = display.width; display.encoded_height = display.height;
      }
      session_bandwidth = std::min(bandwidth_limit, j.value("bandwidth_limit_bps", bandwidth_limit));
      limit_video_bandwidth(displays, session_bandwidth);
      match_resolution = j.value("match_host_resolution", false);
      if (application_mode) {
        desktop_sources = platform->desktop_displays();
        if (desktop_sources.empty()) throw std::runtime_error("No existing desktop monitor is available");
        if (!independent_desktop && displays.size() > desktop_sources.size()) displays.resize(desktop_sources.size());
      }
      adaptation.clear();
      const bool automatic_mpeg = automatic_video_bitrate &&
          std::all_of(displays.begin(), displays.end(), [](const Display &d) { return d.codec == "mpeg2"; });
      std::uint64_t video_pixels = 0;
      for (const auto &d : displays) video_pixels += std::uint64_t(d.encoded_width) * d.encoded_height;
      for (auto &d : displays) {
        int ceiling = d.bitrate;
        if (automatic_mpeg) {
          // Codec payload occupies at most 70% of the connection ceiling.
          // Reserve room for packet headers, I-picture pacing, audio and input.
          // Fair shares are fixed by native pixels, independent of adaptation.
          ceiling = int(std::min<std::uint64_t>(24000000,
              session_bandwidth * 70 / 100 * std::uint64_t(d.encoded_width) * d.encoded_height / video_pixels));
          ceiling = std::max(250000, ceiling);
          d.bitrate = std::min(ceiling, std::max(d.bitrate, 12000000));
        }
        adaptation.emplace_back(d, native_resolution || match_resolution, ceiling);
      }
      std::vector<platform::DisplayConfig> cfg;
      for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto &d = displays[i];
        cfg.push_back({unsigned(i), unsigned(d.width), unsigned(d.height), d.x,
                       d.y, unsigned(d.fps)});
      }
      if (application_mode && (match_resolution || independent_desktop)) {
        try {
          platform->match_desktop(cfg);
          desktop_sources = platform->desktop_displays();
        } catch (...) { stop_session(); throw; }
      }
      make_offer(ch);
      try {
        if (test_source) {
          generating = true;
          generator = std::thread([this] {
            unsigned tick = 0;
            int target_fps = 1;
            for (const auto &display : displays)
              target_fps = std::max(target_fps, display.fps);
            const auto period = std::chrono::microseconds(1000000 / target_fps);
            auto next_frame = std::chrono::steady_clock::now();
            while (generating) {
              for (const auto &d : displays) {
                std::vector<std::uint8_t> pixels(std::size_t(d.width) *
                                                 d.height * 4);
                for (int y = 0; y < d.height; ++y)
                  for (int x = 0; x < d.width; ++x) {
                    auto p = (std::size_t(y) * d.width + x) * 4;
                    pixels[p] = std::uint8_t(x + tick);
                    pixels[p + 1] = std::uint8_t(y);
                    pixels[p + 2] = std::uint8_t(tick);
                    pixels[p + 3] = 255;
                  }
                media.frame(d.id, d.width, d.height, d.width * 4, pixels.data(),
                            pixels.size());
              }
              ++tick;
              next_frame += period;
              std::this_thread::sleep_until(next_frame);
            }
          });
        } else
          platform->start(cfg, [this](platform::VideoFrame frame) {
            std::string id;
            {
              std::lock_guard<std::mutex> lock(profile_mutex);
              if (frame.display_id < displays.size())
                id = displays[frame.display_id].id;
            }
            if (!id.empty())
              media.frame(id, int(frame.width), int(frame.height),
                          int(frame.stride), frame.bgra.data(),
                          frame.bgra.size());
          });
      } catch (...) {
        stop_session();
        throw;
      }
      ch->send(offer);
      if (application_mode)
        std::cout << Json{{"event", "session-started"}, {"monitors", displays.size()}, {"displays", displays},
                         {"bandwidth_limit_bps", session_bandwidth}, {"match_host_resolution", match_resolution}}.dump() << std::endl;
      return;
    }
    if (type == "media_ready") {
      media.authorize();
      return;
    }
    if (type == "keyframe") {
      media.request_keyframe(j.value("id", std::string{}));
      return;
    }
    if (type == "input") {
      if (displays.empty())
        throw std::runtime_error("no active display session");
      if (test_source)
        return;
      platform::InputEvent e;
      auto kind = j.at("kind").get<std::string>();
      e.key = std::uint16_t(j.value("code", 0));
      e.down = j.value("down", false);
      e.repeat = j.value("repeat", false);
      if (kind == "key") {
        if (e.down)
          held_keys.insert(e.key);
        else
          held_keys.erase(e.key);
#ifdef _WIN32
        if (e.down && e.key == 111 &&
            (held_keys.count(29) || held_keys.count(97)) &&
            (held_keys.count(56) || held_keys.count(100))) {
          platform->release_all();
          held_keys.clear();
          e.kind = platform::InputKind::secure_attention;
          platform->inject(e);
          return;
        }
#endif
      }
      if (kind == "key")
        e.kind = platform::InputKind::key;
      else if (kind == "button")
        e.kind = platform::InputKind::button;
      else if (kind == "scroll") {
        e.kind = platform::InputKind::wheel;
        e.delta_x = j.value("dx", 0);
        e.delta_y = j.value("dy", 0);
      } else if (j.contains("display")) {
        auto id = j.at("display").get<std::string>();
        auto found = std::find_if(displays.begin(), displays.end(),
                                  [&](auto &d) { return d.id == id; });
        if (found == displays.end())
          throw std::runtime_error("unknown input display");
        auto p = map_pointer(*found, j.value("x", 0.0), j.value("y", 0.0));
        e.kind = platform::InputKind::absolute_pointer;
        e.x = int(p.first);
        e.y = int(p.second);
      } else {
        e.kind = platform::InputKind::relative_pointer;
        e.delta_x = j.value("dx", 0);
        e.delta_y = j.value("dy", 0);
      }
      platform->inject(e);
      return;
    }
    if (type == "stats") {
      // Profile changes are explicit renegotiations, preserving logical
      // geometry.
      bool changed = false;
      const auto now = std::chrono::steady_clock::now();
      const auto seconds = last_feedback.time_since_epoch().count() ? std::chrono::duration<double>(now - last_feedback).count() : 1.0;
      last_feedback = now;
      if (now >= adaptation_not_before)
        for (std::size_t i = 0; i < adaptation.size(); ++i) {
          const Json *sample = nullptr;
          if (j.contains("streams") && j["streams"].is_array())
            for (const auto &stream : j["streams"])
              if (stream.value("kind", "") == "video" && stream.value("id", "") == displays[i].id) sample = &stream;
          // Modern clients report per-video loss; audio or the other monitor
          // must not reduce this stream's frame rate. Keep a legacy fallback.
          if (adaptation[i].sample(j.value("cpu", 0.0), sample ? sample->value("loss", j.value("loss", 0.0)) : j.value("loss", 0.0),
                sample ? sample->value("queue_ms", 0.0) : 0.0,
                sample ? sample->value("drop_fraction", 0.0) : j.value("drop_fraction", 0.0),
                sample ? sample->value("receiver_timeline_delay_ms", 0.0) : 0.0,
                sample ? sample->value("decode_ms", 0.0) : 0.0, seconds,
                sample ? sample->value("presentation_drop_fraction", -1.0) : -1.0,
                sample ? sample->value("recovery_drop_fraction", 0.0) : 0.0)) changed = true;
        }
      Json decisions = Json::array();
      for (std::size_t i = 0; i < adaptation.size(); ++i)
        decisions.push_back({{"id", displays[i].id}, {"requested_fps", adaptation[i].requested_fps()},
                             {"effective_fps", adaptation[i].profile().fps},
                             {"bitrate_bps", adaptation[i].profile().bitrate},
                             {"reason", adaptation[i].reason()}});
      const auto host_sample = media.stats();
      ch->send({{"type", "status"}, {"host_media", host_sample}, {"adaptation", decisions}});
      if (application_mode)
        std::cout << Json{{"event", "session-performance"}, {"adaptation", decisions},
                          {"client_streams", j.value("streams", Json::array())}, {"host_media", host_sample}}.dump() << std::endl;
      if (changed && !test_source) {
        std::vector<Display> profiles;
        for (const auto &controller : adaptation) profiles.push_back(controller.profile());
        const bool tuned = live_video_reconfigure && media.reconfigure(profiles);
        {
          std::lock_guard<std::mutex> lock(profile_mutex);
          displays = profiles;
        }
        if (tuned) {
          // Keep the media session, audio and held host input intact. Only the
          // video encoder is replaced at a complete raw-picture boundary.
          adaptation_not_before = now + std::chrono::seconds(2);
          ch->send({{"type", "video_profile"}, {"displays", displays}});
        } else {
          // Legacy peers or geometry changes still require a full offer.
          platform->release_all();
          held_keys.clear();
          media.stop();
#if !defined(_WIN32) && !defined(__APPLE__)
          ++audio_epoch;
          if (audio_fd >= 0) close(audio_fd);
          audio_fd = -1;
#endif
          make_offer(ch);
          ch->send(offer);
        }
      }
      return;
    }
    if (type == "device_list") {
      send_devices(ch);
      return;
    }
    if (type == "attach_device" || type == "detach_device") {
      const auto bus = j.at("bus_id").get<std::string>();
      try {
        if (!tunnels)
          throw std::runtime_error(
              "USB import is disabled or unavailable on this host");
        if (!devices::valid_bus_id(bus))
          throw std::runtime_error("Invalid USB bus identifier");
        if (type == "detach_device" && j.value("cancel", false)) {
          const auto session = j.value("session_id", std::string{});
          const auto token = j.value("token", std::string{});
          const auto pending = devices.find(bus);
          if (session == control_session && pending != devices.end() &&
              pending->second->token == token) {
            tunnels->revoke_device(session, bus);
            device_tunnel_closed(session, bus, token, "Client cancelled export",
                                 false);
          }
          return;
        }
        if (displays.empty())
          throw std::runtime_error(
              "Start the remote desktop before forwarding a device");
        if (type == "attach_device" && !j.value("ready", false)) {
          if (devices.count(bus))
            throw std::runtime_error(
                "Device is already attached or connecting");
          auto state = std::make_shared<DeviceState>();
          state->token = random_token(32);
          state->local_port = tunnels->prepare(
              control_session, bus, state->token, ch->peer_fingerprint());
          devices[bus] = state;
          ch->send({{"type", "open_tunnel"},
                    {"bus_id", bus},
                    {"session_id", control_session},
                    {"token", state->token},
                    {"port", tunnels->port()}});
          return;
        }
        const auto found = devices.find(bus);
        if (found == devices.end())
          throw std::runtime_error("Device is not part of this session");
        const auto state = found->second;
        if (state->busy)
          throw std::runtime_error("A device operation is already pending");
        const bool attaching = type == "attach_device";
        if (attaching && (j.value("session_id", "") != control_session ||
                          state->imported_port >= 0))
          throw std::runtime_error(
              "Device session is stale or already imported");
        state->busy = true;
        state->detaching = !attaching;
        state->closed_during_detach.clear();
        asio::post(device_workers, [this, state, bus, attaching,
                                    weak = std::weak_ptr<net::Channel>(ch)] {
          std::string error;
          int imported = state->imported_port;
          try {
#if !defined(_WIN32) && !defined(__APPLE__)
            if (attaching)
              imported = int(devices::import_device(bus, state->local_port));
            else if (imported >= 0)
              devices::detach_device(unsigned(imported));
#else
            throw std::runtime_error("Windows USB safe detach is not qualified");
#endif
          } catch (const std::exception &e) {
            error = e.what();
          }
          if (attaching && imported >= 0)
            state->imported_port = imported;
          if (!attaching && error.empty())
            state->imported_port = -1;
          if (state->cancelled) {
            cleanup_device(bus, state);
            return;
          }
          asio::post(io, [this, state, bus, attaching, imported, error, weak] {
            if (state->cancelled)
              return;
            state->busy = false;
            state->detaching = false;
            auto peer = weak.lock();
            if (!peer || peer != active)
              return;
            if (!error.empty()) {
              if (!state->closed_during_detach.empty()) {
                device_tunnel_closed(control_session, bus, state->token,
                                     state->closed_during_detach + "; " +
                                         error);
                return;
              }
              if (attaching) {
                tunnels->revoke_device(control_session, bus);
                devices.erase(bus);
              }
              peer->send({{"type", "status"},
                          {"bus_id", bus},
                          {"device_error", error}});
              return;
            }
            if (attaching) {
              state->imported_port = imported;
              send_devices(peer);
            } else {
              tunnels->revoke_device(control_session, bus);
              devices.erase(bus);
              peer->send({{"type", "detach_device"},
                          {"bus_id", bus},
                          {"detached", true}});
            }
          });
        });
      } catch (const std::exception &e) {
        ch->send(
            {{"type", "status"}, {"bus_id", bus}, {"device_error", e.what()}});
      }
      return;
    }
    throw std::runtime_error("operation unavailable: " + type);
  }
  void make_offer(const std::shared_ptr<net::Channel> &ch) {
    // Starting new encoders/decoders produces transient drops and zero rate
    // samples. Do not feed those back into another immediate restart.
    adaptation_not_before = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    last_feedback = {};
    offer = {{"type", "offer"},
             {"session", control_session},
             {"host", ch->peer_address()},
             {"host_fingerprint",
              certificate_fingerprint(std::filesystem::path(identity) /
                                      "certificate.pem")},
             {"client_fingerprint", ch->peer_fingerprint()},
             {"displays", displays},
             {"streams", Json::array()},
             {"audio", false}};
    offer["host_mode"] = application_mode ? "application" : "service";
    offer["bandwidth_limit_bps"] = session_bandwidth;
    offer["match_host_resolution"] = match_resolution;
    offer["media_transport"] = media_transport;
    offer["jitter_ms"] = jitter_ms;
    for (std::size_t i = 0; i < displays.size(); ++i) {
      Json stream = {{"id", displays[i].id},
                                  {"kind", "video"},
                                  {"host_port", 47020 + int(i)},
                                  {"client_port", 48020 + int(i)},
                                  {"profile", displays[i]},
                                  {"jitter_ms", displays[i].codec == "mpeg2" ? video_reorder_ms : jitter_ms}};
      if (displays[i].codec == "mpeg2") stream["payload"] = frame_payload ? "gst-frame" : "mpv";
      if (application_mode) {
        stream["source_width"] = desktop_sources.at(i).width;
        stream["source_height"] = desktop_sources.at(i).height;
      }
      offer["streams"].push_back(stream);
    }
    auto effective_audio = audio_config;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (effective_audio.value("audio_source", "") == "pipewire-user" &&
        !media_capabilities().value("user_playback", false)) {
      effective_audio["audio_source"] = "none";
      offer["audio_status"] = "Install the GStreamer PipeWire playback plugin to share audio";
    }
    if (effective_audio.value("audio_source", "") == "pipewire") {
      if (audio_fd >= 0)
        effective_audio["audio_pipewire_fd"] = audio_fd;
      else {
        effective_audio["audio_source"] = "none";
        offer["audio_status"] = audio_reason;
      }
    }
#endif
    const bool audio = effective_audio.value("audio_source", "none") != "none";
    offer["audio"] = audio;
    if (audio)
      offer["streams"].push_back({{"id", "playback"},
                                  {"kind", "audio"},
                                  {"host_port", 47022},
                                  {"client_port", 48022}});
    const auto available = resources.empty()?session_bandwidth:std::max<std::uint64_t>(8000000,session_bandwidth>12000000?session_bandwidth-12000000:8000000);
    const auto wire_budget = available * 90 / 100 - (audio ? 500000 : 0);
    limit_video_bandwidth(displays, available);
    const bool automatic_mpeg = automatic_video_bitrate &&
        std::all_of(displays.begin(), displays.end(), [](const Display &d) { return d.codec == "mpeg2"; });
    for(auto& stream:offer["streams"])if(stream["kind"]=="video")for(const auto& display:displays)if(stream["id"]==display.id)stream["profile"]=display;
    std::uint64_t total_weight = 0;
    for (const auto& display : displays) total_weight += display.codec == "raw-i420" ?
        std::uint64_t(display.encoded_width) * display.encoded_height * display.fps * 12 :
        automatic_mpeg ? std::uint64_t(display.encoded_width) * display.encoded_height : display.bitrate;
    for (auto& stream : offer["streams"]) {
      if (media_transport == "rtp-udp") {
        stream["ssrc"] = std::stoul(random_token(4), nullptr, 16);
        if (stream["ssrc"] == 0) stream["ssrc"] = 1u;
      }
      if (stream["kind"] == "audio") stream["wire_limit_bps"] = 500000;
      else {
        const auto display = stream["profile"].get<Display>();
        const auto weight = display.codec == "raw-i420" ? std::uint64_t(display.encoded_width) * display.encoded_height * display.fps * 12 :
        automatic_mpeg ? std::uint64_t(display.encoded_width) * display.encoded_height : display.bitrate;
        const auto ceiling = wire_budget * weight / total_weight;
        stream["wire_ceiling_bps"] = ceiling;
        // Average codec bitrate controls sustained traffic. Pace large I pictures
        // at the reserved wire share so lowering quality does not add latency.
        stream["wire_limit_bps"] = ceiling;
      }
    }
    auto sender_offer = offer;
    sender_offer.update(effective_audio);
    media.start(sender_offer, combined_identity_pem(identity),
                [this, weak = std::weak_ptr<net::Channel>(ch)](Json event) {
                  asio::post(io, [this, weak, event] {
                    if (auto peer = weak.lock(); peer && peer == active) {
                      if (event.value("type", "") == "media_verified")
                        peer->send({{"type", "media_ready"}});
                      else
                        peer->send(event);
                    }
                  });
                });
  }
  void run(const Json &config) {
    application_mode = config.value("application_mode", false);
    mpeg2_max_fps = config.value("mpeg2_max_fps", 30);
    if (mpeg2_max_fps != 15 && mpeg2_max_fps != 20 && mpeg2_max_fps != 30 && mpeg2_max_fps != 60)
      throw std::runtime_error("MPEG-2 frame-rate limit must be 15, 20, 30 or 60");
    preferred_media_transport = config.value("media_transport", std::string("dtls-srtp"));
    if (preferred_media_transport != "dtls-srtp" && preferred_media_transport != "rtp-udp")
      throw std::runtime_error("Choose dtls-srtp or rtp-udp media transport");
    configured_jitter_ms = config.value("jitter_ms", 40u);
    if (configured_jitter_ms < 5 || configured_jitter_ms > 200)
      throw std::runtime_error("Media jitter budget must be 5 to 200 ms");
    video_reorder_ms = config.value("video_reorder_ms", std::max(150u, configured_jitter_ms));
    if (video_reorder_ms < 5 || video_reorder_ms > 200)
      throw std::runtime_error("Video packet repair window must be 5 to 200 ms");
    bandwidth_limit = config.value("bandwidth_limit_bps", default_bandwidth_bps);
    std::vector<Display> check_budget;
    limit_video_bandwidth(check_budget, bandwidth_limit);
    if (application_mode) {
      if (config.value("usb_enabled", false)) throw std::runtime_error("Application hosting does not import USB devices or load drivers");
      platform = platform::make_desktop_host_platform();
    }
    identity = config.at("identity_dir");
    ensure_identity(identity, "RemoteOS host");
    net::configure(tls, identity);
    if (std::filesystem::exists(std::filesystem::path(identity) /
                                "clients.json"))
      trusted = Json::parse(
          read_private_file(std::filesystem::path(identity) / "clients.json"));
    pair_file = config.value("pairing_code_file", "");
    if (!pair_file.empty() && std::filesystem::exists(pair_file)) {
      pair_code = read_private_file(pair_file);
      while (!pair_code.empty() &&
             std::isspace(static_cast<unsigned char>(pair_code.back())))
        pair_code.pop_back();
      pair_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    }
    test_source = config.value("test_source", false);
    audio_config = {
        {"audio_source", config.value("audio_source", std::string("none"))}};
    for (const auto *key : {"audio_target"})
      if (config.contains(key))
        audio_config[key] = config[key];
    if (audio_config["audio_source"] == "test" && !test_source)
      throw std::runtime_error(
          "Synthetic audio requires explicit test_source mode");
#if !defined(_WIN32) && !defined(__APPLE__)
    if (config.value("usb_enabled", false))
      tunnels = std::make_unique<devices::TunnelServer>(
          io, identity, config.value("bind", std::string("127.0.0.1")),
          config.value("usb_port", std::uint16_t(47011)),
          [this](const std::string &fp, const std::string &session,
                 const std::string &bus, const std::string &token) {
            const auto found = devices.find(bus);
            return authorized && active && active->peer_fingerprint() == fp &&
                   session == control_session && found != devices.end() &&
                   found->second->token == token;
          },
          [this](const std::string &session, const std::string &bus,
                 const std::string &token, const std::string &reason) {
            device_tunnel_closed(session, bus, token, reason);
          });
#endif
    bool preparing = application_mode;
    asio::thread_pool setup_worker{1};
    std::string startup_error;
    signals.async_wait([this, &preparing](std::error_code error, int) {
      if (error) return;
      platform->cancel_prepare();
      std::error_code ec;
      acceptor.close(ec);
      if (!preparing) {
        if (active) active->close("host stopping");
        stop_session(); clear_devices();
        if (tunnels) tunnels->stop();
        if(resource_server)resource_server->stop();
      }
      io.stop();
    });
    if (application_mode) {
      std::cout << Json{{"event", "preparing"}}.dump() << std::endl;
      asio::post(setup_worker, [this, &preparing, &startup_error, config] {
        std::string error;
        try {
          const auto source = config.value("desktop_source", std::string("monitors"));
          if (source != "monitors" && source != "virtual" && source != "kde-native" && source != "independent")
            throw std::runtime_error("Unknown desktop_source; choose independent, monitors, virtual or kde-native");
          independent_desktop = source == "independent";
          if (independent_desktop && (!std::getenv("REMOTEOS_INDEPENDENT_DESKTOP") ||
              std::string(std::getenv("REMOTEOS_INDEPENDENT_DESKTOP")) != "1"))
            throw std::runtime_error("Start the independent desktop through remoteos-independent-desktop.py");
          platform->select_desktop_source(source == "kde-native" ? platform::DesktopSource::kde_native_output :
              source == "virtual" ? platform::DesktopSource::virtual_output : platform::DesktopSource::monitors);
          if (source == "kde-native")
            platform->set_virtual_display_size(config.value("virtual_width", 1024u), config.value("virtual_height", 576u));
          native_resolution = source == "kde-native" || independent_desktop;
          platform->prepare(config.value("portal_parent_window", std::string{}));
          const auto capabilities = platform->capabilities();
          if (!test_source && (!capabilities.capture || !capabilities.input)) {
            error = "Desktop capture and input are unavailable";
            for (const auto& reason : capabilities.diagnostics) error += "; " + reason;
          }
        } catch (const std::exception& e) { error = e.what(); }
        asio::post(io, [this, &preparing, &startup_error, config, error] {
          preparing = false;
          try {
            if (!error.empty()) throw std::runtime_error(error);
            listen(config);
          } catch (const std::exception& e) {
            startup_error = e.what();
            std::cout << Json{{"event", "sharing-failed"}, {"message", startup_error}}.dump() << std::endl;
            io.stop();
          }
        });
      });
    } else listen(config);
    io.run();
    if (discovery) discovery->stop();
    // A stop during the permission dialog cancels/Closes the request before
    // freeing the platform. No listener opens before consent succeeds.
    setup_worker.join();
    stop_session();
    clear_devices();
    if (tunnels) tunnels->stop();
        if(resource_server)resource_server->stop();
    device_workers.join();
#if !defined(_WIN32) && !defined(__APPLE__)
    audio_poll.cancel();
    audio_worker.join();
#endif
#ifdef _WIN32
    if (stop_event) CloseHandle(stop_event);
#endif
    if (!startup_error.empty()) throw std::runtime_error(startup_error);
  }
  void poll_desktop() {
    desktop_poll.expires_after(std::chrono::milliseconds(250));
    desktop_poll.async_wait([this](std::error_code error) {
      if (error) return;
      const auto failure = platform->session_error();
      if (!failure.empty()) {
        std::cout << Json{{"event", "sharing-failed"}, {"message", failure}}.dump() << std::endl;
        if (active) active->close(failure);
        stop_session();
        std::error_code ec; acceptor.close(ec); io.stop();
      } else poll_desktop();
    });
  }
  void listen(const Json& config) {
    if(config.value("userspace_devices",application_mode))
      resource_server=std::make_unique<devices::TunnelServer>(io,identity,config.value("bind",std::string("127.0.0.1")),config.value("resource_port",std::uint16_t(0)),
        [this](const auto& fp,const auto& session,const auto& resource,const auto& token){auto r=resources.find(resource);return authorized&&active&&active->peer_fingerprint()==fp&&session==control_session&&r!=resources.end()&&r->second.token==token;});

    auto endpoint = net::tcp::endpoint(
        asio::ip::make_address(config.value("bind", std::string("127.0.0.1"))),
        config.value("port", default_port));
    acceptor.open(endpoint.protocol());
    acceptor.set_option(net::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    if (config.value("discovery_enabled", application_mode)) {
      try {
        discovery = std::make_unique<DiscoveryResponder>(io,
            config.value("bind", std::string("127.0.0.1")), acceptor.local_endpoint().port(),
            config.value("host_name", asio::ip::host_name()),
            certificate_fingerprint(std::filesystem::path(identity) / "certificate.pem"));
      } catch (const std::exception& error) {
        std::cerr << "Host discovery unavailable: " << error.what() << '\n';
      }
    }
    if (!pair_code.empty()) pair_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    Json source_displays = Json::array();
    for (const auto& d : platform->desktop_displays())
      source_displays.push_back({{"id", d.id}, {"width", d.width}, {"height", d.height}});
    std::cout << Json{{"event", "listening"},
                      {"port", acceptor.local_endpoint().port()},
                      {"desktop_source", config.value("desktop_source", std::string("monitors"))},
                      {"source_displays", std::move(source_displays)},
                      {"fingerprint",
                       certificate_fingerprint(std::filesystem::path(identity) /
                                               "certificate.pem")}}
                     .dump()
              << std::endl;
    accept();
    if (application_mode) poll_desktop();
#if !defined(_WIN32) && !defined(__APPLE__)
    poll_audio();
#endif
#ifdef _WIN32
    const auto name =
        std::wstring(application_mode ? L"Local\\RemoteOS.Stop." : L"Global\\RemoteOS.Stop.") + std::to_wstring(GetCurrentProcessId());
    stop_event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (stop_event)
      poll_stop();
#endif
  }
};
#include "portable_runtime.hpp"
int main(int argc, char **argv) {
  try {
    remoteos::app::portable_runtime();
    Json cfg = {{"identity_dir", "./host-identity"},
                {"bind", "127.0.0.1"},
                {"port", default_port}};
    bool init = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto value = [&]() {
        if (++i >= argc)
          throw std::runtime_error("missing argument");
        return std::string(argv[i]);
      };
      if (a == "--config") {
        std::ifstream f(value());
        if (!f)
          throw std::runtime_error("cannot read config");
        cfg = Json::parse(f);
      } else if (a == "--identity")
        cfg["identity_dir"] = value();
      else if (a == "--bind")
        cfg["bind"] = value();
      else if (a == "--port")
        cfg["port"] = std::stoul(value());
      else if (a == "--pair-code-file")
        cfg["pairing_code_file"] = value();
      else if (a == "--test-source")
        cfg["test_source"] = true;
      else if (a == "--desktop")
        cfg["application_mode"] = true;
      else if (a == "--init")
        init = true;
      else if (a == "--help") {
        std::cout
            << "remoteos-host --config host.json | --identity DIR [--bind IP] "
               "[--port N] [--pair-code-file FILE] [--init] [--test-source] [--desktop]\n";
        return 0;
      } else
        throw std::runtime_error("unknown argument: " + a);
    }
    if (init) {
      std::filesystem::path dir = cfg.at("identity_dir").get<std::string>();
      ensure_identity(dir, "RemoteOS host");
      auto codepath = std::filesystem::path(
          cfg.value("pairing_code_file", (dir / "pairing-code").string()));
      write_private_file(codepath, random_pairing_pin() + "\n");
      std::cout << Json{{"fingerprint",
                         certificate_fingerprint(dir / "certificate.pem")},
                        {"pairing_code_file", codepath.string()}}
                       .dump(2)
                << '\n';
      return 0;
    }
    Server server;
    server.run(cfg);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "remoteos-host: " << e.what() << '\n';
    return 1;
  }
}
