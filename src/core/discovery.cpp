#include "remoteos/discovery.hpp"
#include <array>
#include <algorithm>
#include <set>
#include <thread>

namespace remoteos {
namespace {
using udp = asio::ip::udp;
constexpr std::size_t max_packet = 1400;
bool printable(const std::string& value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool nonce_valid(const std::string& nonce) {
  return nonce.size() == 24 && std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}
void to_json(Json& j, const HostEntry& h) {
  j = {{"name", h.name}, {"address", h.address}, {"port", h.port}, {"fingerprint", h.fingerprint}};
}
void from_json(const Json& j, HostEntry& h) {
  h.name = j.at("name"); h.address = j.at("address"); h.port = j.value("port", default_port);
  h.fingerprint = normalize_fingerprint(j.at("fingerprint"));
  if (!printable(h.name, 80) || !printable(h.address, 253) || h.port < 1024 || h.port > 65535)
    throw std::invalid_argument("Invalid saved or discovered host");
}
HostEntry parse_discovery_reply(const std::string& packet, const std::string& nonce, const std::string& sender) {
  if (packet.size() > max_packet || !nonce_valid(nonce)) throw std::invalid_argument("Invalid discovery packet");
  const auto j = Json::parse(packet);
  if (j.value("type", "") != "remoteos-host" || j.value("version", 0u) != protocol_version || j.value("nonce", "") != nonce)
    throw std::invalid_argument("Unsolicited or incompatible discovery reply");
  std::error_code error; const auto address = asio::ip::make_address(sender, error);
  if (error || address.is_unspecified() || address.is_multicast()) throw std::invalid_argument("Invalid discovery source");
  auto host = Json{{"name", j.at("name")}, {"address", sender}, {"port", j.at("port")}, {"fingerprint", j.at("fingerprint")}}.get<HostEntry>();
  host.available = true; return host;
}
std::vector<HostEntry> merge_hosts(const std::vector<HostEntry>& saved, const std::vector<HostEntry>& discovered) {
  std::vector<HostEntry> result;
  for (auto host : saved) { host.saved = true; host.available = false; host.identity_changed = false; result.push_back(std::move(host)); }
  for (const auto& host : discovered) {
    auto found = std::find_if(result.begin(), result.end(), [&](const auto& item) { return item.address == host.address && item.port == host.port; });
    if (found != result.end()) {
      // Never replace a stored identity with an unauthenticated advertisement.
      found->identity_changed = found->fingerprint != host.fingerprint;
      found->available = true;
    } else if (result.size() < 64) result.push_back(host);
  }
  return result;
}
std::vector<HostEntry> discover_hosts(const std::vector<std::string>& extra, bool local, std::chrono::milliseconds timeout) {
  asio::io_context io; udp::socket socket(io, udp::endpoint(udp::v4(), 0));
  socket.non_blocking(true); socket.set_option(asio::socket_base::broadcast(true));
  socket.set_option(asio::ip::multicast::hops(1));
  const auto nonce = random_token(12);
  const auto packet = Json{{"type", "remoteos-discover"}, {"version", protocol_version}, {"nonce", nonce}}.dump();
  std::set<std::string> targets;
  if (local) { targets.insert("255.255.255.255"); targets.insert("239.255.47.10"); }
  for (const auto& address : extra) { if (targets.size() >= 66) break; targets.insert(address); }
  for (const auto& target : targets) {
    std::error_code error; const auto address = asio::ip::make_address_v4(target, error);
    if (!error && !address.is_unspecified()) socket.send_to(asio::buffer(packet), udp::endpoint(address, discovery_port), 0, error);
  }
  std::vector<HostEntry> result;
  const auto deadline = std::chrono::steady_clock::now() + std::clamp(timeout, std::chrono::milliseconds(1), std::chrono::milliseconds(3000));
  std::array<char, max_packet + 1> buffer{};
  unsigned received = 0;
  while (std::chrono::steady_clock::now() < deadline && received < 256) {
    udp::endpoint source; std::error_code error;
    const auto size = socket.receive_from(asio::buffer(buffer), source, 0, error);
    if (error == asio::error::would_block || error == asio::error::try_again) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    ++received;
    if (error) continue;
    try {
      const auto host = parse_discovery_reply(std::string(buffer.data(), size), nonce, source.address().to_string());
      if (std::none_of(result.begin(), result.end(), [&](const auto& entry) { return entry.address == host.address && entry.port == host.port; }) && result.size() < 64)
        result.push_back(host);
    } catch (...) {}
  }
  return result;
}
struct DiscoveryResponder::Impl : std::enable_shared_from_this<Impl> {
  udp::socket socket;
  udp::endpoint sender;
  std::array<char, max_packet + 1> data{};
  unsigned control_port, requests = 0;
  std::string name, fingerprint;
  std::chrono::steady_clock::time_point window{};
  Impl(asio::io_context& io, const std::string& bind, unsigned port, std::string host_name, std::string fp)
      : socket(io), control_port(port), name(std::move(host_name)), fingerprint(normalize_fingerprint(std::move(fp))) {
    if (!printable(name, 80)) name = "RemoteOS host";
    const auto address = asio::ip::make_address(bind);
    if (!address.is_v4()) throw std::runtime_error("IPv4 discovery requires an IPv4 listen address");
    socket.open(udp::v4()); socket.bind(udp::endpoint(address, discovery_port));
    if (address.is_unspecified()) {
      std::error_code error;
      socket.set_option(asio::ip::multicast::join_group(asio::ip::make_address_v4("239.255.47.10")), error);
    }
  }
  void receive() {
    socket.async_receive_from(asio::buffer(data), sender, [self = shared_from_this()](std::error_code error, std::size_t size) {
      if (!self->socket.is_open()) return;
      if (!error && size <= max_packet) {
        try {
          const auto now = std::chrono::steady_clock::now();
          if (now - self->window > std::chrono::seconds(1)) { self->window = now; self->requests = 0; }
          const auto query = Json::parse(std::string(self->data.data(), size));
          const auto nonce = query.value("nonce", std::string{});
          if (query.value("type", "") == "remoteos-discover" && query.value("version", 0u) == protocol_version && nonce_valid(nonce) && ++self->requests <= 20) {
            auto reply = std::make_shared<std::string>(Json{{"type", "remoteos-host"}, {"version", protocol_version}, {"nonce", nonce},
                  {"name", self->name}, {"port", self->control_port}, {"fingerprint", self->fingerprint}}.dump());
            self->socket.async_send_to(asio::buffer(*reply), self->sender, [self, reply](std::error_code, std::size_t) {});
          }
        } catch (...) {}
      }
      self->receive();
    });
  }
};
DiscoveryResponder::DiscoveryResponder(asio::io_context& io, std::string bind, unsigned port, std::string name, std::string fingerprint)
    : impl_(std::make_shared<Impl>(io, bind, port, std::move(name), std::move(fingerprint))) { impl_->receive(); }
DiscoveryResponder::~DiscoveryResponder() { stop(); }
void DiscoveryResponder::stop() noexcept { std::error_code error; if (impl_) impl_->socket.close(error); }
}
