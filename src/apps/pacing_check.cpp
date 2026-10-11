#include "../media/deadline_wait.hpp"
#include "../media/packet_pacer.hpp"
#include "remoteos/protocol.hpp"
#include <iostream>

int main() {
  using Clock = std::chrono::steady_clock;
  remoteos::media_detail::DeadlineWaiter wait;
  remoteos::media_detail::PacketPacer pacer(32000000);
  const auto start = Clock::now();
  constexpr unsigned packets = 2000, payload = 1200;
  for (unsigned i = 0; i < packets; ++i) wait.until(pacer.reserve(payload, Clock::now()));
  const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const auto mbps = packets * (payload + 128) * 8.0 / seconds / 1000000;
  const bool passed = wait.high_resolution() && mbps >= 16000000.0 / 1000000;
  std::cout << remoteos::Json{{"status", passed ? "passed" : "failed"},
      {"high_resolution_wait", wait.high_resolution()}, {"packets", packets},
      {"paced_equivalent_mbps", mbps}, {"elapsed_ms", seconds * 1000},
      {"physical_devices_used", false}, {"network_packets_sent", false}}.dump(2) << '\n';
  return passed ? 0 : 1;
}
