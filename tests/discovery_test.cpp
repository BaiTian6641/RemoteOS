#include "remoteos/discovery.hpp"
#include <iostream>
#include <thread>

int main() {
  try {
    asio::io_context io;
    const auto fingerprint = std::string(64, 'a');
    remoteos::DiscoveryResponder server(io, "127.0.0.1", 48123, "Private test host", fingerprint);
    std::thread worker([&] { io.run(); });
    auto hosts = remoteos::discover_hosts({"127.0.0.1"}, false, std::chrono::milliseconds(300));
    asio::post(io, [&] { server.stop(); }); worker.join();
    if (hosts.size() != 1 || hosts[0].address != "127.0.0.1" || hosts[0].port != 48123 || hosts[0].fingerprint != fingerprint)
      throw std::runtime_error("Loopback discovery did not preserve the advertised TLS identity and control port");
    hosts = remoteos::discover_hosts({"127.0.0.1"}, false, std::chrono::milliseconds(50));
    if (!hosts.empty()) throw std::runtime_error("Stopped host was still advertised");
    std::cout << "private UDP host discovery, nonce matching and stop lifecycle passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
