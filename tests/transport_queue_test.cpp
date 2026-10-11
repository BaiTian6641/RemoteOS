#include "transport.hpp"
#include "remoteos/identity.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#include <cstdlib>

using namespace remoteos;
using namespace remoteos::net;

int main() {
  char pattern[] = "/tmp/remoteos-input-queue-XXXXXX";
  const char *directory = mkdtemp(pattern);
  if (!directory) return 2;
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
  try {
    const auto root = cleanup.path;
    ensure_identity(root / "server", "private queue server");
    ensure_identity(root / "client", "private queue client");
    asio::io_context io;
    asio::ssl::context server_context(asio::ssl::context::tls), client_context(asio::ssl::context::tls);
    configure(server_context, (root / "server").string());
    configure(client_context, (root / "client").string());
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto server = std::make_shared<Channel>(io, server_context);
    auto client = std::make_shared<Channel>(io, client_context);
    bool complete = false;
    std::string failure;
    std::vector<Json> observed;
    asio::steady_timer timeout(io, std::chrono::seconds(10));
    timeout.async_wait([&](std::error_code ec) { if (!ec) { failure = "input queue timed out"; io.stop(); } });
    auto closed = [&](std::string reason) { if (!complete) failure = std::move(reason); io.stop(); };
    server->on_close = closed; client->on_close = closed;
    server->on_message = [&](Json message) {
      observed.push_back(message);
      if (message.value("type", "") == "release_all") { complete = true; io.stop(); }
    };
    acceptor.async_accept(server->stream.next_layer(), [&](std::error_code ec) {
      if (ec) throw std::runtime_error(ec.message());
      server->handshake(asio::ssl::stream_base::server, [&] {
        if (server->peer_fingerprint() != certificate_fingerprint(root / "client/certificate.pem"))
          throw std::runtime_error("unexpected test client identity");
      });
    });
    client->stream.next_layer().async_connect(acceptor.local_endpoint(), [&](std::error_code ec) {
      if (ec) throw std::runtime_error(ec.message());
      client->handshake(asio::ssl::stream_base::client, [&] {
        if (client->peer_fingerprint() != certificate_fingerprint(root / "server/certificate.pem"))
          throw std::runtime_error("unexpected test server identity");
        for (int x = 0; x < 1000; ++x)
          client->send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"}, {"x", x}, {"y", 0}});
        client->send({{"type", "input"}, {"kind", "button"}, {"code", 0x110}, {"down", true}});
        for (int x = 1000; x < 2000; ++x)
          client->send({{"type", "input"}, {"kind", "motion"}, {"display", "internal"}, {"x", x}, {"y", 0}});
        client->send({{"type", "input"}, {"kind", "button"}, {"code", 0x110}, {"down", false}});
        for (int dx = 1; dx <= 3; ++dx)
          client->send({{"type", "input"}, {"kind", "motion"}, {"dx", dx}, {"dy", -dx}});
        client->send({{"type", "input"}, {"kind", "scroll"}, {"dx", 1}, {"dy", -1}});
        client->send({{"type", "input"}, {"kind", "key"}, {"code", 42}, {"down", true}});
        client->send({{"type", "release_all"}});
      });
    });
    io.run();
    if (!complete || !failure.empty()) throw std::runtime_error(failure.empty() ? "input did not complete" : failure);
    unsigned absolute = 0, relative = 0, buttons = 0;
    int last_position = -1;
    std::vector<std::string> boundaries;
    for (const auto &message : observed) {
      const auto kind = message.value("kind", "");
      if (kind == "motion" && message.contains("display")) { ++absolute; last_position = message.at("x"); }
      else if (kind == "button") {
        const bool down = message.at("down");
        if (last_position != (down ? 999 : 1999)) throw std::runtime_error("motion crossed a button boundary");
        ++buttons; boundaries.push_back(down ? "down" : "up");
      } else if (kind == "motion") {
        ++relative;
        if (message.at("dx") != relative || message.at("dy") != -int(relative))
          throw std::runtime_error("relative motion was coalesced or reordered");
        boundaries.push_back("relative");
      } else boundaries.push_back(kind.empty() ? message.value("type", "") : kind);
    }
    if (absolute < 2 || absolute > 6 || buttons != 2 || relative != 3 ||
        boundaries != std::vector<std::string>{"down", "up", "relative", "relative", "relative", "scroll", "key", "release_all"})
      throw std::runtime_error("stale motion accumulated or ordered input changed");
    std::cout << "2,000 absolute motions reduced to " << absolute
              << " current positions; button, relative motion, scroll, key and release order preserved over real TLS\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
