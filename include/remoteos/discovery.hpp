#pragma once
#include "remoteos/protocol.hpp"
#include <asio.hpp>
#include <chrono>
#include <functional>
#include <memory>

namespace remoteos {
constexpr unsigned discovery_port = 47009;
struct HostEntry {
  std::string name, address, fingerprint;
  unsigned port = default_port;
  bool saved = false, available = false, identity_changed = false;
};
void to_json(Json &, const HostEntry &);
void from_json(const Json &, HostEntry &);
// Discovery is a hint, not a trust decision. Only the verified fingerprint
// saved during pairing authorizes subsequent TLS connections.
std::vector<HostEntry> merge_hosts(const std::vector<HostEntry>& saved,
                                 const std::vector<HostEntry>& discovered);
HostEntry parse_discovery_reply(const std::string&, const std::string& nonce,
                               const std::string& sender);
std::vector<HostEntry> discover_hosts(const std::vector<std::string>& extra_addresses = {},
                                    bool local_network = true,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds(1200));
class DiscoveryResponder {
public:
  DiscoveryResponder(asio::io_context&, std::string bind, unsigned control_port,
                     std::string name, std::string fingerprint);
  ~DiscoveryResponder();
  void stop() noexcept;
private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}
