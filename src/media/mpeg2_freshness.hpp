#pragma once
#include <cstddef>
#include <cstdint>

namespace remoteos::media_detail {
// Dropping an arbitrary P picture breaks its successors' reference chain.
// After falling behind, resume only at a fresh I picture and ask the host for
// one. No codec work or copies are needed to inspect the picture header.
class Mpeg2Freshness {
public:
  bool accept(const std::uint8_t *bytes, std::size_t size, bool stale, bool discontinuity) {
    if (stale || discontinuity) waiting_ = true;
    if (stale) return false;
    if (!waiting_) return true;
    for (std::size_t i = 0; i + 6 <= size; ++i)
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 && bytes[i + 3] == 0 &&
          ((bytes[i + 5] >> 3) & 7) == 1) {
        waiting_ = false;
        return true;
      }
    return false;
  }
private:
  bool waiting_ = false;
};
} // namespace remoteos::media_detail
