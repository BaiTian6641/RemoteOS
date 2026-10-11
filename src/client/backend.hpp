#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace remoteos::client {
struct AccessPoint {
  std::string bssid;
  std::string ssid;
  std::string flags;
  int signal_dbm = 0;
  bool supported = false;
  bool secure = false;
};
struct MachineStatus {
  double cpu_percent = 0;
  double busiest_cpu_percent = 0;
  std::uint64_t used_kib = 0;
  std::uint64_t total_kib = 0;
  int battery_percent = -1;
  std::string battery_state;
};
struct NetworkStatus {
  std::string interface, ssid, state, address, error;
  int signal_dbm = -100;
  bool signal_known = false, associated = false, wired = false, usable = false;
};
NetworkStatus parse_wifi_status(const std::string &status, const std::string &signal);
NetworkStatus read_network_status();
std::vector<AccessPoint> parse_scan_results(const std::string &text);
std::string
find_wireless_interface(const std::string &sys_net = "/sys/class/net");
bool valid_http_url(const std::string &url);
std::string hex_encode(const std::string &bytes);
class WpaControl {
public:
  explicit WpaControl(std::string interface);
  ~WpaControl();
  WpaControl(const WpaControl &) = delete;
  WpaControl &operator=(const WpaControl &) = delete;
  std::string command(const std::string &request);
  // Empty password deliberately means an open network. Reject unsupported
  // networks in the caller rather than silently falling back to plaintext.
  void connect(const std::string &ssid, std::string password, bool secure);

private:
  int fd_ = -1;
  std::string local_;
  std::string directory_;
};
class StatusReader {
public:
  MachineStatus read(const std::string& proc_stat = "/proc/stat");

private:
  std::uint64_t previous_total_ = 0;
  std::uint64_t previous_idle_ = 0;
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> previous_cpus_;
};
} // namespace remoteos::client
