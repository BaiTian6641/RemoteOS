#pragma once
#include "remoteos/identity.hpp"
#include "remoteos/protocol.hpp"
#include <cstdlib>
#include <charconv>
#include <filesystem>
#include <stdexcept>

namespace remoteos::app {
inline unsigned parse_port(const std::string& value) {
  unsigned port = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port < 1024 || port > 65535)
    throw std::runtime_error("Use a port between 1024 and 65535");
  return port;
}
struct HostState {
  std::filesystem::path directory, executable;
  std::string fingerprint, code;
  explicit HostState(std::filesystem::path host, std::filesystem::path state = {}) : executable(std::move(host)) {
    if (!state.empty()) directory = std::move(state);
    else {
#ifdef _WIN32
      const auto *base = std::getenv("LOCALAPPDATA");
      if (!base) throw std::runtime_error("LOCALAPPDATA is unavailable");
      directory = std::filesystem::path(base) / "RemoteOS" / "Host";
#else
      if (const auto *base = std::getenv("XDG_DATA_HOME"); base && *base)
        directory = std::filesystem::path(base) / "remoteos-host";
      else {
        const auto *home = std::getenv("HOME");
        if (!home) throw std::runtime_error("The user home directory is unavailable");
        directory = std::filesystem::path(home) / ".local/share/remoteos-host";
      }
#endif
    }
    directory = std::filesystem::absolute(directory);
  }
  std::filesystem::path prepare(const std::string& bind, unsigned port, bool audio, const std::string& parent_window = "",
                                const std::string& desktop_source = "monitors", unsigned virtual_width = 1024, unsigned virtual_height = 576,
                                unsigned mpeg2_max_fps = 30, const std::string& media_transport = "rtp-udp",
                                const std::string& independent_environment = "auto") {
    if (port < 1024 || port > 65535) throw std::runtime_error("Use a port between 1024 and 65535");
    if (mpeg2_max_fps != 15 && mpeg2_max_fps != 20 && mpeg2_max_fps != 30 && mpeg2_max_fps != 60)
      throw std::runtime_error("Choose a 15, 20, 30 or 60 fps limit");
    if (media_transport != "rtp-udp" && media_transport != "dtls-srtp")
      throw std::runtime_error("Choose trusted or encrypted UDP media");
    if (desktop_source != "monitors" && desktop_source != "virtual" && desktop_source != "kde-native" && desktop_source != "independent")
      throw std::runtime_error("Choose an independent desktop, existing monitors or a virtual display");
    if (independent_environment != "auto" && independent_environment != "minimal" && independent_environment != "plasma")
      throw std::runtime_error("Choose the logged-in desktop or Minimal desktop");
    if (!std::filesystem::is_regular_file(executable)) throw std::runtime_error("Place remoteos-host beside this application");
    const auto identity = directory / "identity";
    ensure_identity(identity.string(), "RemoteOS desktop host");
    fingerprint = certificate_fingerprint(identity / "certificate.pem");
    code = random_pairing_pin();
    const auto code_file = directory / "pairing-code";
    write_private_file(code_file, code + "\n");
    Json configuration{{"identity_dir", identity.u8string()}, {"pairing_code_file", code_file.u8string()},
                       {"bind", bind}, {"port", port}, {"application_mode", true}, {"usb_enabled", false},
                       {"desktop_source", desktop_source}, {"resource_port", port<=65533?port+2:port-2},
                       {"independent_environment", independent_environment},
                       {"virtual_width", virtual_width}, {"virtual_height", virtual_height},
                       {"mpeg2_max_fps", mpeg2_max_fps},
                       {"media_transport", media_transport}, {"jitter_ms", 40},
                       {"bandwidth_limit_bps", default_bandwidth_bps}, {"discovery_enabled", true},
#ifdef _WIN32
                       {"audio_source", audio ? "wasapi" : "none"}};
#elif defined(__APPLE__)
                       {"audio_source", "none"}};
#else
                       {"audio_source", audio ? "pipewire-user" : "none"}};
#endif
    write_private_file(directory / "resources.json","[]");
    const auto config = directory / "application-host.json";
    if (!parent_window.empty()) configuration["portal_parent_window"] = parent_window;
    write_private_file(config, configuration.dump(2) + "\n");
    return config;
  }
};
} // namespace remoteos::app
