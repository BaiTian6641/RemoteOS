#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace remoteos::media_detail {
// RFC4175 progressive 8-bit YCbCr-4:2:0. Each six-byte pixel group covers a
// 2x2 luma block and one U/V sample. The jitterbuffer delivers ordered packets;
// validating contiguous segment coverage rejects gaps, overlaps and truncated
// frames without writing another depayloader or copying the pixel data.
class RawFrameCoverage {
public:
  RawFrameCoverage(unsigned width, unsigned height)
      : width_(width), height_(height),
        total_(std::size_t(width) * height / 4) {}
  void packet(std::uint32_t timestamp, bool marker, const std::uint8_t *payload,
              std::size_t size) {
    // Sink and source probes surround one synchronous depayloader chain call.
    // A verdict left from its previous input must never slide to a later frame
    // if the depayloader skipped an output (reset/allocation/malformed input).
    if (verdict_count_) {
      overflow_ = true;
      verdict_count_ = 0;
    }
    if (active_ && timestamp != timestamp_) {
      verdict(false); // upstream depayloader flushes this incomplete old frame
      active_ = false;
    }
    if (!active_) {
      active_ = true;
      timestamp_ = timestamp;
      groups_ = 0;
      valid_ = true;
    }
    std::size_t offset = 2, pixel_bytes = 0;
    bool more = true;
    while (more) {
      if (offset + 6 > size) {
        valid_ = false;
        break;
      }
      const auto *header = payload + offset;
      const unsigned length = unsigned(header[0]) * 256 + header[1];
      const unsigned line = unsigned(header[2] & 0x7f) * 256 + header[3];
      const unsigned column = unsigned(header[4] & 0x7f) * 256 + header[5];
      more = (header[4] & 0x80) != 0;
      const std::size_t position =
          std::size_t(line / 2) * (width_ / 2) + column / 2;
      if (!length || length % 6 || (header[2] & 0x80) || line % 2 ||
          column % 2 || line >= height_ || column >= width_ ||
          position != groups_ || length / 6 > (width_ - column) / 2)
        valid_ = false;
      groups_ += length / 6;
      pixel_bytes += length;
      offset += 6;
    }
    if (offset > size || pixel_bytes != size - offset || groups_ > total_)
      valid_ = false;
    if (marker) {
      verdict(valid_ && groups_ == total_);
      active_ = false;
    }
  }
  bool output() {
    if (overflow_ || !verdict_count_) {
      overflow_ = true;
      return false;
    }
    const bool result = verdicts_[verdict_front_];
    verdict_front_ = (verdict_front_ + 1) % verdicts_.size();
    --verdict_count_;
    return result;
  }
  bool failed() const { return overflow_; }

private:
  void verdict(bool valid) {
    if (verdict_count_ == verdicts_.size()) {
      overflow_ = true;
      verdict_count_ = 0;
    } else if (!overflow_) {
      verdicts_[(verdict_front_ + verdict_count_) % verdicts_.size()] = valid;
      ++verdict_count_;
    }
  }
  unsigned width_, height_;
  std::size_t total_, groups_ = 0;
  std::uint32_t timestamp_ = 0;
  bool active_ = false, valid_ = false, overflow_ = false;
  std::array<bool, 4> verdicts_{};
  unsigned verdict_front_ = 0, verdict_count_ = 0;
};
} // namespace remoteos::media_detail
