#include "backend.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace remoteos::client {
namespace {
std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string item; std::getline(in, item, delimiter);)
    out.push_back(item);
  return out;
}
std::string read_first(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}
std::string decode_ssid(const std::string &text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out += text[i];
      continue;
    }
    const char escaped = text[++i];
    if (escaped == 'x' && i + 2 < text.size()) {
      const auto n = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int a = n(text[i + 1]), b = n(text[i + 2]);
      if (a >= 0 && b >= 0) {
        out += static_cast<char>(a * 16 + b);
        i += 2;
        continue;
      }
    }
    if (escaped == 'n')
      out += '\n';
    else if (escaped == 'r')
      out += '\r';
    else if (escaped == 't')
      out += '\t';
    else
      out += escaped;
  }
  return out;
}
} // namespace
bool valid_http_url(const std::string &url) {
  if (url.size() > 8192 ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return c <= 32 || c == 127 || c == '\\';
      }))
    return false;
  std::size_t start = 0;
  if (url.compare(0, 7, "http://") == 0)
    start = 7;
  else if (url.compare(0, 8, "https://") == 0)
    start = 8;
  else
    return false;
  const auto end = url.find_first_of("/?#", start);
  const auto authority =
      url.substr(start, end == std::string::npos ? end : end - start);
  return !authority.empty() && authority.find('@') == std::string::npos;
}
std::string hex_encode(const std::string &bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}
std::vector<AccessPoint> parse_scan_results(const std::string &text) {
  std::vector<AccessPoint> out;
  for (const auto &row : split(text, '\n')) {
    auto fields = split(row, '\t');
    if (fields.size() != 5 || fields[0].size() != 17)
      continue;
    AccessPoint ap;
    ap.bssid = fields[0];
    ap.flags = fields[3];
    ap.ssid = decode_ssid(fields[4]);
    try {
      ap.signal_dbm = std::stoi(fields[2]);
    } catch (...) {
      continue;
    }
    ap.secure = ap.flags.find("WPA") != std::string::npos ||
                ap.flags.find("RSN") != std::string::npos ||
                ap.flags.find("WEP") != std::string::npos;
    const bool personal = ap.flags.find("PSK") != std::string::npos;
    const bool wpa2 = ap.flags.find("WPA2") != std::string::npos ||
                      ap.flags.find("RSN") != std::string::npos;
    const bool forbidden = ap.flags.find("EAP") != std::string::npos ||
                           ap.flags.find("WEP") != std::string::npos ||
                           ap.flags.find("OWE") != std::string::npos;
    ap.supported = !forbidden && (!ap.secure || (personal && wpa2));
    if (ap.ssid.empty() || ap.ssid.size() > 32)
      continue;
    out.push_back(std::move(ap));
  }
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return a.signal_dbm > b.signal_dbm;
  });
  return out;
}
std::string find_wireless_interface(const std::string &sys_net) {
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(sys_net, ec)) {
    if (std::filesystem::exists(entry.path() / "wireless", ec))
      return entry.path().filename().string();
  }
  return {};
}
NetworkStatus parse_wifi_status(const std::string &text, const std::string &signal) {
  NetworkStatus status;
  for (const auto &line : split(text, '\n')) {
    const auto equal = line.find('=');
    if (equal == std::string::npos) continue;
    const auto key = line.substr(0, equal), value = line.substr(equal + 1);
    if (key == "wpa_state") status.state = value;
    else if (key == "ssid") status.ssid = decode_ssid(value);
  }
  status.associated = status.state == "COMPLETED";
  for (const auto &line : split(signal, '\n')) {
    if (line.compare(0, 5, "RSSI=") != 0) continue;
    try {
      const int value = std::stoi(line.substr(5));
      if (value >= -127 && value <= 0) {
        status.signal_dbm = value;
        status.signal_known = true;
      }
    } catch (...) {}
  }
  return status;
}
NetworkStatus read_network_status() {
  NetworkStatus status;
  const auto wifi = find_wireless_interface();
  if (!wifi.empty()) {
    try {
      WpaControl control(wifi);
      const auto response = control.command("STATUS");
      status = parse_wifi_status(response, "");
      try { status = parse_wifi_status(response, control.command("SIGNAL_POLL")); } catch (...) {}
    } catch (const std::exception &error) { status.error = error.what(); }
    status.interface = wifi;
  }
  ifaddrs *addresses = nullptr;
  if (getifaddrs(&addresses) != 0) return status;
  for (auto *entry = addresses; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
        !(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK)) continue;
    const std::string interface = entry->ifa_name;
    // Tailscale/bridge addresses cannot establish the physical network's
    // readiness. Ethernet and Wi-Fi must also have their carrier/association.
    if (!std::filesystem::exists(std::filesystem::path("/sys/class/net") / interface / "device")) continue;
    const auto *address = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
    const auto number = ntohl(address->sin_addr.s_addr);
    if (!number || (number >> 16) == 0xa9fe) continue;
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip))) continue;
    if (interface == wifi && status.associated) {
      status.address = ip;
      status.interface = wifi;
      status.wired = false;
      status.usable = true;
      break;
    }
    if (interface != wifi && read_first(std::filesystem::path("/sys/class/net") / interface / "carrier") == "1") {
      status.address = ip;
      status.interface = interface;
      status.wired = status.usable = true;
    }
  }
  freeifaddrs(addresses);
  return status;
}
WpaControl::WpaControl(std::string interface) {
  if (interface.empty() || interface.size() > 15 ||
      interface.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR"
                                  "STUVWXYZ0123456789_.-") != std::string::npos)
    throw std::runtime_error("Invalid wireless interface");
  char directory[] = "/tmp/remoteos-wpa-XXXXXX";
  if (!mkdtemp(directory))
    throw std::runtime_error("Cannot create private Wi-Fi socket directory");
  directory_ = directory;
  local_ = directory_ + "/control";
  fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  sockaddr_un local{}, remote{};
  local.sun_family = remote.sun_family = AF_UNIX;
  const auto remote_path = "/run/wpa_supplicant/" + interface;
  std::strncpy(local.sun_path, local_.c_str(), sizeof(local.sun_path) - 1);
  std::strncpy(remote.sun_path, remote_path.c_str(),
               sizeof(remote.sun_path) - 1);
  if (fd_ < 0 ||
      bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0 ||
      ::connect(fd_, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) <
          0) {
    const std::string why = std::strerror(errno);
    if (fd_ >= 0)
      close(fd_);
    unlink(local_.c_str());
    rmdir(directory_.c_str());
    throw std::runtime_error("Cannot access Wi-Fi control socket: " + why);
  }
}
WpaControl::~WpaControl() {
  if (fd_ >= 0)
    close(fd_);
  unlink(local_.c_str());
  rmdir(directory_.c_str());
}
std::string WpaControl::command(const std::string &request) {
  if (request.size() > 4096 ||
      request.find_first_of("\r\n") != std::string::npos)
    throw std::runtime_error("Invalid Wi-Fi request");
  if (send(fd_, request.data(), request.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(request.size()))
    throw std::runtime_error("Wi-Fi request failed");
  pollfd p{fd_, POLLIN, 0};
  int result;
  do {
    result = poll(&p, 1, 1500);
  } while (result < 0 && errno == EINTR);
  if (result != 1 || !(p.revents & POLLIN))
    throw std::runtime_error("Wi-Fi service did not respond");
  std::array<char, 32768> buffer{};
  const auto count = recv(fd_, buffer.data(), buffer.size() - 1, 0);
  if (count < 0)
    throw std::runtime_error("Cannot read Wi-Fi response");
  std::string response(buffer.data(), static_cast<std::size_t>(count));
  while (!response.empty() &&
         (response.back() == '\n' || response.back() == '\r'))
    response.pop_back();
  return response;
}
void WpaControl::connect(const std::string &ssid, std::string password,
                         bool secure) {
  if (ssid.empty() || ssid.size() > 32)
    throw std::runtime_error("SSID must contain 1 to 32 bytes");
  std::string psk;
  if (secure) {
    if (password.size() < 8 || password.size() > 63)
      throw std::runtime_error("WPA2 password must contain 8 to 63 bytes");
    std::array<unsigned char, 32> derived{};
    const auto ok = PKCS5_PBKDF2_HMAC_SHA1(
        password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char *>(ssid.data()),
        static_cast<int>(ssid.size()), 4096, static_cast<int>(derived.size()),
        derived.data());
    OPENSSL_cleanse(password.data(), password.size());
    if (!ok)
      throw std::runtime_error("Cannot derive WPA2 key");
    psk = hex_encode(std::string(reinterpret_cast<const char *>(derived.data()),
                                 derived.size()));
    OPENSSL_cleanse(derived.data(), derived.size());
  }
  auto id = command("ADD_NETWORK");
  if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Cannot add Wi-Fi network");
  auto checked = [&](const std::string &text) {
    if (command(text) != "OK")
      throw std::runtime_error("Wi-Fi configuration was rejected");
  };
  try {
    checked("SET_NETWORK " + id + " ssid " + hex_encode(ssid));
    checked("SET_NETWORK " + id + " key_mgmt " + (secure ? "WPA-PSK" : "NONE"));
    if (secure) {
      checked("SET_NETWORK " + id + " proto RSN");
      checked("SET_NETWORK " + id + " psk " + psk);
    }
    OPENSSL_cleanse(psk.data(), psk.size());
    checked("SET_NETWORK " + id + " scan_ssid 1");
    checked("SELECT_NETWORK " + id);
    checked("SAVE_CONFIG");
  } catch (...) {
    OPENSSL_cleanse(psk.data(), psk.size());
    try {
      command("REMOVE_NETWORK " + id);
    } catch (...) {
    }
    throw;
  }
}
MachineStatus StatusReader::read(const std::string& proc_stat) {
  MachineStatus s;
  std::ifstream stat(proc_stat);
  std::string cpu;
  std::array<std::uint64_t, 10> values{};
  stat >> cpu;
  for (auto &value : values)
    stat >> value;
  // guest and guest_nice are already included in user and nice.
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < 8; ++i)
    total += values[i];
  const auto idle = values[3] + values[4];
  if (previous_total_ && total > previous_total_ && idle >= previous_idle_)
    s.cpu_percent =
        100.0 * (1.0 - static_cast<double>(idle - previous_idle_) /
                           static_cast<double>(total - previous_total_));
  previous_total_ = total;
  previous_idle_ = idle;
  // A single saturated decoder worker can appear as only 50% total CPU on
  // the N270's two logical processors. Report that pressure separately.
  for (std::string line; std::getline(stat, line);) {
    std::istringstream row(line);
    std::string name;
    row >> name;
    if (name.size() < 4 || name.compare(0, 3, "cpu") || !std::isdigit(static_cast<unsigned char>(name[3])))
      continue;
    std::uint64_t sum = 0;
    std::array<std::uint64_t, 8> ticks{};
    for (auto& tick : ticks) { row >> tick; sum += tick; }
    const auto unused = ticks[3] + ticks[4];
    const auto previous = previous_cpus_[name];
    if (previous.first && sum > previous.first && unused >= previous.second)
      s.busiest_cpu_percent = std::max(s.busiest_cpu_percent,
          100.0 * (1.0 - double(unused - previous.second) / double(sum - previous.first)));
    previous_cpus_[name] = {sum, unused};
  }
  std::ifstream memory("/proc/meminfo");
  std::uint64_t available = 0;
  for (std::string line; std::getline(memory, line);) {
    std::istringstream row(line);
    std::string key;
    std::uint64_t value = 0;
    row >> key >> value;
    if (key == "MemTotal:")
      s.total_kib = value;
    if (key == "MemAvailable:")
      available = value;
  }
  s.used_kib = s.total_kib >= available ? s.total_kib - available : 0;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
    if (read_first(entry.path() / "type") != "Battery")
      continue;
    try {
      s.battery_percent = std::stoi(read_first(entry.path() / "capacity"));
    } catch (...) {
    }
    s.battery_state = read_first(entry.path() / "status");
    break;
  }
  return s;
}
} // namespace remoteos::client
