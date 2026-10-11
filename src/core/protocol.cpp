#include "remoteos/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <openssl/rand.h>
#include <set>
#include <stdexcept>

namespace remoteos {
namespace {
// Integer MPEG-2 rates supported by the codec's frame-rate extension. In
// particular, accepting every integer would negotiate profiles the encoder
// cannot start (e.g.19 or29 fps).
constexpr int mpeg2_rates[] = {5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 30, 50, 60};
int mpeg2_rate_at_most(int requested) {
  int result = mpeg2_rates[0];
  for (const int rate : mpeg2_rates)
    if (rate <= requested)
      result = rate;
  return result;
}
} // namespace
void to_json(Json &j, const Display &d) {
  j = {{"id", d.id},
       {"width", d.width},
       {"height", d.height},
       {"x", d.x},
       {"y", d.y},
       {"encoded_width", d.encoded_width},
       {"encoded_height", d.encoded_height},
       {"fps", d.fps},
       {"bitrate", d.bitrate},
       {"codec", d.codec}};
}
void from_json(const Json &j, Display &d) {
  d.id = j.at("id").get<std::string>();
  d.codec = j.value("codec", std::string("mpeg2"));
  if (d.codec != "mpeg2" && d.codec != "raw-i420")
    throw std::invalid_argument("unsupported video codec");
  d.width = j.at("width");
  d.height = j.at("height");
  d.x = j.value("x", 0);
  d.y = j.value("y", 0);
  d.encoded_width = j.value("encoded_width", d.width);
  d.encoded_height = j.value("encoded_height", d.height);
  d.fps = j.value("fps", 30);
  d.bitrate = j.value("bitrate", d.codec == "raw-i420" ? 0 : 6000000);
  if (d.id.empty() || d.id.size() > 32 ||
      !std::all_of(d.id.begin(), d.id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
      }))
    throw std::invalid_argument("invalid display id");
  if (d.width < 320 || d.width > 1920 || d.height < 200 || d.height > 1080 ||
      d.x < 0 || d.y < 0 || d.x > 3840 || d.y > 2160 || d.encoded_width < 320 ||
      d.encoded_width > d.width || d.encoded_height < 200 ||
      d.encoded_height > d.height || d.encoded_width % 2 ||
      d.encoded_height % 2 || d.fps < 5 ||
      d.fps > 60 ||
      (d.codec == "mpeg2" && mpeg2_rate_at_most(d.fps) != d.fps) ||
      (d.codec == "mpeg2" && (d.bitrate < 250000 || d.bitrate > 30000000)) ||
      (d.codec == "raw-i420" && d.bitrate != 0))
    throw std::invalid_argument("unsupported display profile");
}
std::vector<Display> parse_displays(const Json &j) {
  if (!j.is_array() || j.empty() || j.size() > 2)
    throw std::invalid_argument("one or two displays required");
  auto ds = j.get<std::vector<Display>>();
  std::set<std::string> ids;
  for (const auto &d : ds)
    if (!ids.insert(d.id).second)
      throw std::invalid_argument("duplicate display id");
  if (ds.size() == 2) {
    const auto &a = ds[0];
    const auto &b = ds[1];
    if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
        b.y < a.y + a.height)
      throw std::invalid_argument("display rectangles overlap");
  }
  return ds;
}
void validate_message(const Json &j) {
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string())
    throw std::invalid_argument("message type required");
  const std::string type = j["type"];
  static const std::set<std::string> types = {"hello",
                                              "pair",
                                              "paired",
                                              "capabilities",
                                              "start",
                                              "offer",
                                              "media_ready",
                                              "media_verified",
                                              "input",
                                              "release_all",
                                              "display_changed",
                                              "video_profile",
                                              "keyframe",
                                              "device_list",
                                              "attach_device",
                                              "detach_device",
                                              "open_tunnel",
                                              "ping",
                                              "pong",
                                              "status",
                                              "disconnect",
                                              "error",
                                              "state",
                                              "stats"};
  if (!types.count(type))
    throw std::invalid_argument("unknown message type");
  if (j.dump().size() > max_control_frame)
    throw std::invalid_argument("control frame too large");
  if (type == "hello" && j.value("version", 0u) != protocol_version)
    throw std::invalid_argument("unsupported protocol version");
  if (type == "video_profile") (void)parse_displays(j.at("displays"));
  if (type == "start" || type == "display_changed") {
    (void)parse_displays(j.at("displays"));
    if (j.contains("bandwidth_limit_bps")) {
      if (!j["bandwidth_limit_bps"].is_number_unsigned() && !j["bandwidth_limit_bps"].is_number_integer())
        throw std::invalid_argument("Bandwidth must be an integer");
      const auto limit = j["bandwidth_limit_bps"].get<std::int64_t>();
      if (limit < 1000000 || limit > 1000000000) throw std::invalid_argument("Invalid bandwidth limit");
    }
    for (const auto *flag : {"live_video_reconfigure", "automatic_video_bitrate"})
      if (j.contains(flag) && !j[flag].is_boolean()) throw std::invalid_argument("Video capability must be boolean");
    if (j.contains("match_host_resolution") && !j["match_host_resolution"].is_boolean())
      throw std::invalid_argument("Resolution matching must be boolean");
  }
  if (type == "input") {
    auto kind = j.at("kind").get<std::string>();
    if (kind != "key" && kind != "button" && kind != "motion" &&
        kind != "scroll")
      throw std::invalid_argument("unsupported input kind");
    if (kind == "key" || kind == "button") {
      int code = j.at("code").get<int>();
      if (code < 0 || code > 767 || code == 116)
        throw std::invalid_argument("invalid or reserved input code");
      if (!j.at("down").is_boolean())
        throw std::invalid_argument("input down must be boolean");
      if (j.contains("repeat") && !j.at("repeat").is_boolean())
        throw std::invalid_argument("input repeat must be boolean");
    }
    for (auto field : {"x", "y", "dx", "dy"})
      if (j.contains(field)) {
        double v = j.at(field).get<double>();
        if (!std::isfinite(v) || std::abs(v) > 16384)
          throw std::invalid_argument("invalid input coordinate");
      }
  }
}
std::pair<double, double> map_pointer(const Display &d, double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y))
    throw std::invalid_argument("nonfinite pointer");
  return {
      d.x + std::clamp(x / (d.encoded_width - 1) * (d.width - 1), 0.0, double(d.width - 1)),
      d.y + std::clamp(y / (d.encoded_height - 1) * (d.height - 1), 0.0,
                       double(d.height - 1))};
}
std::string normalize_fingerprint(std::string fp) {
  std::string result;
  for (unsigned char c : fp) {
    if (c == ':' || c == ' ' || c == '\n')
      continue;
    if (!std::isxdigit(c))
      throw std::invalid_argument("invalid SHA256 fingerprint");
    result += char(std::tolower(c));
  }
  if (result.size() != 64)
    throw std::invalid_argument(
        "SHA256 fingerprint must contain 64 hex digits");
  return result;
}
std::string random_token(std::size_t bytes) {
  if (bytes > 128)
    throw std::invalid_argument("token too large");
  std::vector<unsigned char> data(bytes);
  if (RAND_bytes(data.data(), int(bytes)) != 1)
    throw std::runtime_error("secure random generation failed");
  const char *hex = "0123456789abcdef";
  std::string out;
  for (auto b : data) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}
std::string random_pairing_pin() {
  // Rejection sampling avoids modulo bias; leading zeroes are significant.
  std::uint32_t value = 0;
  constexpr std::uint32_t ceiling = UINT32_MAX - (UINT32_MAX % 1000000u);
  do {
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&value), sizeof(value)) != 1)
      throw std::runtime_error("secure PIN generation failed");
  } while (value >= ceiling);
  auto pin = std::to_string(value % 1000000u);
  return std::string(6 - pin.size(), '0') + pin;
}
void limit_video_bandwidth(std::vector<Display>& displays, std::uint64_t bandwidth) {
  if (bandwidth < 1000000 || bandwidth > 1000000000)
    throw std::invalid_argument("Bandwidth must be between 1 and 1000 Mbps");
  const auto budget = bandwidth * 80 / 100;
  std::uint64_t total = 0;
  for (const auto& display : displays) {
    if (display.codec == "raw-i420") {
      const auto raw = std::uint64_t(display.encoded_width) * display.encoded_height * 12 * display.fps;
      total += raw * 115 / 100;
    } else total += display.bitrate;
  }
  if (total <= budget) return;
  if (std::any_of(displays.begin(), displays.end(), [](const auto& d) { return d.codec == "raw-i420"; }))
    throw std::invalid_argument("Raw video exceeds the connection bandwidth limit; select MPEG-2");
  if (budget < displays.size() * 250000)
    throw std::invalid_argument("Insufficient bandwidth for the requested monitors");
  const auto flexible = budget - displays.size() * 250000;
  const auto requested = total - displays.size() * 250000;
  for (auto& display : displays)
    display.bitrate = 250000 + int(flexible * std::uint64_t(display.bitrate - 250000) / requested);
}
Adaptation::Adaptation(Display initial, bool preserve_resolution, int maximum_bitrate)
    : current_(std::move(initial)), native_(current_), preserve_resolution_(preserve_resolution),
      maximum_bitrate_(maximum_bitrate ? maximum_bitrate : current_.bitrate) {
  if (maximum_bitrate_ < current_.bitrate || maximum_bitrate_ > 30000000 ||
      (current_.codec == "raw-i420" && maximum_bitrate_ != 0))
    throw std::invalid_argument("Automatic bitrate ceiling must cover the initial codec budget");
}
bool Adaptation::sample(double cpu, double loss, double queue_ms, double drop_fraction,
                        double timeline_ms, double decode_ms, double seconds,
                        double presentation_drop_fraction, double recovery_drop_fraction) {
  if (!std::isfinite(cpu) || cpu < 0 || cpu > 100 || !std::isfinite(loss) || loss < 0 || loss > 1 ||
      !std::isfinite(queue_ms) || queue_ms < 0 || !std::isfinite(drop_fraction) ||
      drop_fraction < 0 || drop_fraction > 1 || !std::isfinite(timeline_ms) || timeline_ms < 0 ||
      !std::isfinite(decode_ms) || decode_ms < 0 || !std::isfinite(seconds) || seconds <= 0 ||
      !std::isfinite(presentation_drop_fraction) ||
      (presentation_drop_fraction < 0 && presentation_drop_fraction != -1) ||
      presentation_drop_fraction > 1 || !std::isfinite(recovery_drop_fraction) ||
      recovery_drop_fraction < 0 || recovery_drop_fraction > 1)
    return false;
  // Missing/rapid feedback cannot count as many seconds of sustained pressure.
  seconds = std::min(seconds, 2.0);
  elapsed_ += seconds;
  const double budget = 1000.0 / current_.fps;
  // Recovery discards are dependent pictures waiting for a replacement I
  // picture. They do not measure how quickly the display can present frames.
  const double presentation = presentation_drop_fraction >= 0 ? presentation_drop_fraction : drop_fraction;
  std::string pressure;
  if (cpu > 85 || decode_ms > budget * .9) pressure = "Receiver decode/CPU pressure";
  else if (loss > .03) pressure = "Video packet loss";
  else if (queue_ms > 80 || presentation > .08) pressure = "Video presentation pressure";
  else if (timeline_ms > 80) pressure = "Receiver delivery backlog";
  if (!pressure.empty()) {
    bad_ = pressure == pressure_ ? bad_ + seconds : seconds;
    pressure_ = pressure;
    good_ = 0;
  } else if (cpu < 70 && loss < .01 && queue_ms < 40 && presentation < .03 &&
             recovery_drop_fraction < .03 && timeline_ms < 60 && decode_ms < budget * .8) {
    good_ += seconds;
    bad_ = 0;
    pressure_.clear();
  } else {
    bad_ = good_ = 0;
    pressure_.clear();
  }
  if (bad_ >= 3) {
    bad_ = 0;
    // A receiver falling behind is not evidence that the network is full.
    // Lowering bitrate for decoder/presentation backlog destroys motion
    // quality while leaving the same number of pictures to decode.
    const bool network = pressure == "Video packet loss";
    // Network congestion needs fewer bits per second. Reducing fps at the same
    // bitrate made each picture larger, while leaving the congested link busy.
    if (network && current_.codec == "mpeg2") {
      const int floor = std::min(native_.bitrate, 500000);
      const auto bitrate = std::max(floor, current_.bitrate * 3 / 4);
      if (bitrate != current_.bitrate) {
        current_.bitrate = bitrate; reason_ = pressure + " · reduced bitrate"; return true;
      }
      reason_ = pressure + " · bitrate floor reached";
      return false;
    }
    if (current_.fps > 10) {
      auto fps = std::max(10, current_.fps - (current_.fps > 30 ? 10 : 5));
      if (decode_ms > budget * .9)
        fps = std::min(fps, std::max(10, int(std::floor(800.0 / decode_ms))));
      if (presentation > .08)
        fps = std::min(fps, std::max(10, int(std::floor(current_.fps * (1.0 - presentation) * .9))));
      if (current_.codec == "mpeg2") fps = mpeg2_rate_at_most(fps);
      if (fps != current_.fps) {
        fps_recovery_after_ = elapsed_ + 12;
        current_.fps = fps; reason_ = pressure + " · reduced frame rate"; return true;
      }
    }
    if (!preserve_resolution_ && current_.encoded_width == native_.width) {
      current_.encoded_width = std::max(320, (native_.width * 2 / 3) & ~1);
      current_.encoded_height = std::max(200, (native_.height * 2 / 3) & ~1);
      current_.bitrate = current_.codec == "raw-i420" ? 0 : std::max(500000, native_.bitrate / 2);
      reason_ = pressure + " · reduced encoded resolution";
      return true;
    }
    reason_ = pressure + " · frame-rate floor reached";
  }
  if (good_ >= 12) {
    // Require sustained stability between changes. In particular, an Atom
    // decoding 30 fps comfortably may still be too slow for the next legal
    // MPEG-2 rate of 50 fps; use that candidate's budget before probing it.
    if (current_.encoded_width != native_.width) {
      good_ = 0;
      current_.encoded_width = native_.width;
      current_.encoded_height = native_.height;
      reason_ = "Stable receiver · restored native pixels";
      return true;
    }
    if (current_.fps < native_.fps && elapsed_ >= fps_recovery_after_) {
      const auto previous = current_.fps;
      auto candidate = std::min(native_.fps, current_.fps + (current_.fps >= 30 ? 10 : 5));
      if (current_.codec == "mpeg2") {
        candidate = mpeg2_rate_at_most(candidate);
        if (candidate <= previous)
          for (const auto rate : mpeg2_rates)
            if (rate > previous && rate <= native_.fps) { candidate = rate; break; }
      }
      if (candidate > previous && decode_ms < 800.0 / candidate) {
        good_ = 0;
        current_.fps = candidate;
        reason_ = "Stable receiver · probing higher frame rate";
        return true;
      }
    }
    if (current_.bitrate < maximum_bitrate_) {
      good_ = 0;
      current_.bitrate = std::min(maximum_bitrate_, current_.bitrate + std::max(125000, maximum_bitrate_ / 8));
      reason_ = "Stable delivery · probing higher bitrate";
      return true;
    }
  }
  return false;
}
} // namespace remoteos
