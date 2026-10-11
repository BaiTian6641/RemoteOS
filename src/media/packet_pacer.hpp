#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace remoteos::media_detail {
// Pace outgoing datagrams. Charge an extra 128 bytes for UDP/IP and
// tunnel framing; this is intentionally conservative for the 40 Mbps profile.
// Each stream has a reserved share, so a video I-frame cannot queue audio.
class PacketPacer {
public:
  using Clock = std::chrono::steady_clock;
  explicit PacketPacer(std::uint64_t bits_per_second) : rate_(bits_per_second) {}
  void set_rate(std::uint64_t bits_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = std::max<std::uint64_t>(64000, bits_per_second);
    // Keep the current packet deadline. A profile change grants no burst credit.
  }
  Clock::time_point reserve(std::size_t payload_bytes, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto interval = std::chrono::nanoseconds(((payload_bytes + 128) * 8ULL * 1000000000ULL + rate_ - 1) / rate_);
    // Keep a virtual packet-start schedule. Restarting it from every late
    // wake-up charged scheduler overhead once per packet and reduced the
    // effective link rate. Permit only two packets of catch-up, never an
    // accumulated I-frame burst after idle time.
    if (next_ == Clock::time_point{}) next_ = now;
    next_ = std::max(next_, now - interval * 2);
    const auto deadline = std::max(next_, now);
    next_ += interval;
    return deadline;
  }
private:
  std::uint64_t rate_;
  Clock::time_point next_{};
  std::mutex mutex_;
};
}
