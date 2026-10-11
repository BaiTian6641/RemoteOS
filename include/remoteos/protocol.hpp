#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace remoteos {
using Json = nlohmann::json;
constexpr unsigned protocol_version = 1;
constexpr std::size_t max_control_frame = 65536;
constexpr unsigned default_port = 47010;
constexpr std::uint64_t default_bandwidth_bps = 40000000;
struct Display {
  std::string id;
  int width = 1366, height = 768, x = 0, y = 0;
  int encoded_width = 1366, encoded_height = 768, fps = 30, bitrate = 6000000;
  std::string codec = "mpeg2";
};
void to_json(Json &, const Display &);
void from_json(const Json &, Display &);
void validate_message(const Json &);
std::vector<Display> parse_displays(const Json &);
std::pair<double, double> map_pointer(const Display &, double, double);
std::string normalize_fingerprint(std::string);
std::string random_token(std::size_t bytes = 24);
std::string random_pairing_pin();
// Reserve twenty percent for audio, control, RTP/IP and tunnel overhead. The
// remaining video allocation is shared by all monitors, never per monitor.
void limit_video_bandwidth(std::vector<Display> &, std::uint64_t bandwidth_bps);
class Adaptation {
public:
  // An explicit profile keeps its requested bitrate ceiling. Automatic quality
  // profiles may separately authorize a higher ceiling within the wire budget.
  explicit Adaptation(Display initial, bool preserve_resolution = false,
                      int maximum_bitrate = 0);
  bool sample(double cpu, double loss, double queue_ms, double drop_fraction = 0,
              double timeline_ms = 0, double decode_ms = 0, double seconds = 1,
              double presentation_drop_fraction = -1,
              double recovery_drop_fraction = 0);
  const Display &profile() const { return current_; }
  const std::string &reason() const { return reason_; }
  int requested_fps() const { return native_.fps; }

private:
  Display current_, native_;
  bool preserve_resolution_ = false;
  int maximum_bitrate_ = 0;
  double bad_ = 0, good_ = 0;
  double elapsed_ = 0, fps_recovery_after_ = 0;
  std::string pressure_, reason_ = "Requested profile";
};
} // namespace remoteos
