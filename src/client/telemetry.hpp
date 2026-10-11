#pragma once
#include "remoteos/protocol.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace remoteos::client {
// A single private RAM snapshot, never a growing log. The qualification sampler
// joins it with hardware counters; it contains no host identities or
// credentials.
class Telemetry {
public:
  Telemetry() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    struct stat info{};
    if (runtime && *runtime && ::lstat(runtime, &info) == 0 &&
        S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
        (info.st_mode & 0077) == 0)
      path_ = std::filesystem::path(runtime) / "remoteos-metrics.csv";
  }

  void publish(bool connected, const std::vector<Display> &displays,
               const Json &stats, double round_trip_ms, std::size_t usb) const {
    if (path_.empty())
      return;
    double uptime = 0;
    std::ifstream("/proc/uptime") >> uptime;
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "uptime_seconds,connected,internal_codec,internal_width,"
            "internal_height,internal_target_fps,internal_fps,vga_codec,"
            "vga_width,vga_height,vga_target_fps,vga_fps,control_rtt_ms,"
            "rtp_loss_fraction,queue_ms,usb_exported,"
            "internal_bitrate_bps,internal_decoded_fps,internal_decode_ms,"
            "internal_receiver_timeline_delay_ms,internal_presentation_drop_fraction,"
            "internal_recovery_drop_fraction,internal_receive_mbps,internal_compressed_queue_drops,"
            "vga_bitrate_bps,vga_decoded_fps,vga_decode_ms,vga_receiver_timeline_delay_ms,"
            "vga_presentation_drop_fraction,vga_recovery_drop_fraction,vga_receive_mbps,"
            "vga_compressed_queue_drops\n";
    text << std::fixed << std::setprecision(4) << uptime << ',' << connected;
    for (const char *id : {"internal", "vga"}) {
      const Display *display = nullptr;
      if (connected)
        for (const auto &candidate : displays)
          if (candidate.id == id)
            display = &candidate;
      double fps = 0;
      if (display && stats.contains("streams") && stats["streams"].is_array())
        for (const auto &stream : stats["streams"])
          if (stream.value("id", std::string{}) == id)
            fps = number(stream, "fps", 0);
      if (display)
        text << ',' << display->codec << ',' << display->encoded_width << ','
             << display->encoded_height << ',' << display->fps << ',' << fps;
      else
        text << ",none,0,0,0,0";
    }
    text << ','
         << (connected && std::isfinite(round_trip_ms) && round_trip_ms >= 0
                 ? round_trip_ms
                 : 0)
         << ',' << number(stats, "loss", 0) << ','
         << number(stats, "queue_ms", 0) << ',' << usb;
    // Preserve the original 16 fields. The sampler recognizes this extended
    // header explicitly and records whether these stage counters were present.
    for (const char *id : {"internal", "vga"}) {
      const Display *display = nullptr;
      const Json *detail = nullptr;
      if (connected)
        for (const auto &candidate : displays)
          if (candidate.id == id)
            display = &candidate;
      if (display && stats.contains("streams") && stats["streams"].is_array())
        for (const auto &stream : stats["streams"])
          if (stream.value("id", std::string{}) == id)
            detail = &stream;
      text << ',' << (display ? display->bitrate : 0);
      for (const char *field : {"decoded_fps", "decode_ms", "receiver_timeline_delay_ms",
                                "presentation_drop_fraction", "recovery_drop_fraction",
                                "receive_mbps", "compressed_queue_drops"})
        text << ',' << (detail ? number(*detail, field, 0) : 0);
    }
    text << '\n';
    auto temporary = path_.string() + ".XXXXXX";
    const int fd = ::mkstemp(temporary.data()); // exclusive, mode0600
    if (fd < 0)
      return;
    const auto bytes = text.str();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count =
          ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count <= 0)
        break;
      offset += static_cast<std::size_t>(count);
    }
    const int closed = ::close(fd);
    if (offset == bytes.size() && closed == 0)
      (void)::rename(temporary.c_str(), path_.c_str());
    (void)::unlink(temporary.c_str());
  }

private:
  static double number(const Json &value, const char *field, double fallback) {
    if (!value.contains(field) || !value[field].is_number())
      return fallback;
    const double number = value[field].get<double>();
    return std::isfinite(number) && number >= 0 ? number : fallback;
  }
  std::filesystem::path path_;
};
} // namespace remoteos::client
