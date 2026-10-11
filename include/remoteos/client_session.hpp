#pragma once
#include "remoteos/protocol.hpp"
#include <functional>
#include <memory>

namespace remoteos {
struct ClientOptions {
  std::string host;
  unsigned port = default_port;
  std::string identity_dir;
  std::string host_fingerprint;
  std::string pairing_code;
  std::uint64_t bandwidth_limit_bps = default_bandwidth_bps;
  bool match_host_resolution = false;
  bool automatic_video_bitrate = false;
  bool live_video_reconfigure = false;
  std::vector<Display> displays{
      {"internal", 1366, 768, 0, 0, 1366, 768, 30, 6000000}};
};
// Callbacks run on a worker thread. GTK callers must marshal to the main loop.
// Events: state{state}, paired{fingerprint}, capabilities{...}, offer{...},
// error{message}.
class ClientSession {
public:
  using Handler = std::function<void(const Json &)>;
  ClientSession();
  ~ClientSession();
  ClientSession(const ClientSession &) = delete;
  ClientSession &operator=(const ClientSession &) = delete;
  void connect(ClientOptions, Handler);
  void send(const Json &);
  void disconnect();
  bool running() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace remoteos
