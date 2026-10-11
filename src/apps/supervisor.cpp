#include "remoteos/devices.hpp"
#include "remoteos/protocol.hpp"
#include "suspend.hpp"
#include "sd_card.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
struct Lease {
  pid_t owner;
  Clock::time_point expiry;
  Clock::time_point retry_after{};
};
std::map<std::string, Lease> exports;
pid_t exporter_pid = -1;
volatile sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }
void prepare_tailscale() {
  // Reapply the fixed appliance operator to the current profile. The caller
  // cannot supply a user, socket, command, enrollment token or preferences.
  const auto child = fork();
  if (child < 0)
    throw std::runtime_error("Cannot prepare Tailscale enrollment permissions");
  if (!child) {
    clearenv();
    setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
    execl("/usr/bin/tailscale", "tailscale", "--socket=/run/tailscale/tailscaled.sock",
          "set", "--operator=remoteos", static_cast<char *>(nullptr));
    _exit(127);
  }
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  int status = 0;
  for (;;) {
    const auto result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return;
      throw std::runtime_error("Tailscale could not grant enrollment access; check its service status");
    }
    if ((result < 0 && errno != EINTR) || Clock::now() >= deadline) {
      kill(child, SIGKILL);
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
      throw std::runtime_error("Tailscale permission setup timed out");
    }
    usleep(10000);
  }
}
void stop_exporter() {
  if (exporter_pid > 0) {
    kill(exporter_pid, SIGTERM);
    while (waitpid(exporter_pid, nullptr, 0) < 0 && errno == EINTR) {
    }
    exporter_pid = -1;
  }
}
void recover_stale_exports() {
  const std::filesystem::path driver = "/sys/bus/usb/drivers/usbip-host";
  if (!std::filesystem::exists(driver))
    return;
  for (const auto &entry : std::filesystem::directory_iterator(driver)) {
    const auto bus = entry.path().filename().string();
    if (!remoteos::devices::valid_bus_id(bus))
      continue;
    try {
      remoteos::devices::unbind_usb(bus);
    } catch (const std::exception &e) {
      std::cerr << "Stale USB export: " << e.what() << '\n';
      exports[bus] = {0, Clock::now(), Clock::now() + std::chrono::seconds(5)};
    }
  }
}
remoteos::devices::UsbPolicy policy() {
  remoteos::devices::UsbPolicy result;
  std::ifstream file("/etc/remoteos/usb-policy.json");
  if (!file)
    return result; // Nothing is approved without provisioning.
  auto config = remoteos::Json::parse(file);
  for (const auto &entry : config.value("approved", remoteos::Json::array()))
    result.approved.push_back(
        {entry.at("vid"), entry.at("pid"), entry.value("serial", "")});
  result.protected_block_devices =
      config.value("protected_block_devices", std::vector<std::string>{});
  return result;
}
void start_exporter() {
  if (exporter_pid > 0 && waitpid(exporter_pid, nullptr, WNOHANG) == 0)
    return;
  exporter_pid = -1;
  if (!std::filesystem::exists("/usr/share/remoteos/usbip-loopback-only"))
    throw std::runtime_error(
        "This image has no qualified loopback-only USB/IP exporter");
  const auto parent = getpid();
  const auto child = fork();
  if (child < 0)
    throw std::runtime_error("Cannot start local USB exporter");
  if (!child) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
      _exit(125);
    clearenv();
    setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
    execl("/usr/sbin/usbipd", "usbipd", "--ipv4", "--tcp-port", "3240",
          static_cast<char *>(nullptr));
    _exit(127);
  }
  exporter_pid = child;
  // Confirm the patched daemon remains alive; kernel USB/IP binding is still
  // performed only after the policy below approves this exact device.
  usleep(100000);
  if (waitpid(child, nullptr, WNOHANG) != 0) {
    exporter_pid = -1;
    throw std::runtime_error("Local USB exporter exited during startup");
  }
}
void expire_leases() {
  for (auto it = exports.begin(); it != exports.end();) {
    if (Clock::now() < it->second.retry_after) {
      ++it;
      continue;
    }
    if (Clock::now() >= it->second.expiry ||
        (kill(it->second.owner, 0) < 0 && errno == ESRCH)) {
      try {
        remoteos::devices::unbind_usb(it->first);
      } catch (const std::exception &e) {
        std::cerr << "USB lease cleanup: " << e.what() << '\n';
        stop_exporter();
        it->second.retry_after = Clock::now() + std::chrono::seconds(5);
        ++it;
        continue;
      }
      it = exports.erase(it);
    } else
      ++it;
  }
}
remoteos::Json usb_action(const std::string &action,
                          const remoteos::Json &request, pid_t owner) {
  using namespace remoteos;
  if (action == "usb_list") {
    Json list = Json::array();
    for (const auto &device : devices::enumerate_usb(policy()))
      list.push_back({{"bus_id", device.bus_id},
                      {"vid", device.vid},
                      {"pid", device.pid},
                      {"serial", device.serial},
                      {"label", device.label},
                      {"eligible", device.eligible},
                      {"reason", device.reason},
                      {"exported", exports.count(device.bus_id) != 0}});
    return {{"ok", true}, {"devices", list}};
  }
  if (action == "usb_keepalive") {
    for (auto &entry : exports)
      if (entry.second.owner == owner)
        entry.second.expiry = Clock::now() + std::chrono::seconds(30);
    return {{"ok", true}};
  }
  const auto bus = request.at("bus_id").get<std::string>();
  if (!devices::valid_bus_id(bus))
    throw std::runtime_error("Invalid USB bus identifier");
  const auto found = exports.find(bus);
  if (found != exports.end() && found->second.owner != owner)
    throw std::runtime_error("Device is owned by another local client process");
  if (action == "usb_export") {
    if (found == exports.end()) {
      if (exports.size() >= 8)
        throw std::runtime_error("Device limit reached");
      start_exporter();
      devices::bind_usb(bus, policy());
    }
    exports[bus] = {owner, Clock::now() + std::chrono::seconds(30)};
    return {{"ok", true}, {"local_port", 3240}};
  }
  if (action == "usb_unexport") {
    if (found != exports.end()) {
      devices::unbind_usb(bus);
      exports.erase(found);
    }
    return {{"ok", true}};
  }
  throw std::runtime_error("USB action not permitted");
}
} // namespace

// Deliberately small root boundary. No arbitrary commands, paths or
// environment.
int main() {
  try {
    if (geteuid() != 0)
      throw std::runtime_error("supervisor must run as root");
    auto *user = getpwnam("remoteos");
    if (!user)
      throw std::runtime_error("remoteos account missing");
    auto uid = user->pw_uid;
    auto gid = user->pw_gid;
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    recover_stale_exports();
    std::filesystem::create_directories("/run/remoteos");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      throw std::runtime_error("socket failed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, "/run/remoteos/supervisor.sock");
    unlink(addr.sun_path);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        chmod(addr.sun_path, 0660) || chown(addr.sun_path, 0, gid) ||
        listen(fd, 4))
      throw std::runtime_error("cannot bind supervisor socket");
    while (!stopping) {
      expire_leases();
      pollfd ready{fd, POLLIN, 0};
      const auto available = poll(&ready, 1, 1000);
      if (available == 0 || (available < 0 && errno == EINTR))
        continue;
      if (available < 0)
        break;
      int peer = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (peer < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      ucred cred{};
      socklen_t n = sizeof(cred);
      timeval limit{2, 0};
      setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
      std::string response;
      std::string action;
      try {
        if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &n) ||
            !(cred.uid == uid || cred.uid == 0))
          throw std::runtime_error("unauthorized caller");
        std::string request;
        char buf[1024];
        while (request.find('\n') == std::string::npos) {
          const auto got = read(peer, buf, sizeof(buf));
          if (got <= 0)
            throw std::runtime_error("incomplete supervisor request");
          request.append(buf, got);
          if (request.size() > 4096)
            throw std::runtime_error("supervisor request too large");
        }
        auto j = remoteos::Json::parse(request);
        action = j.at("action").get<std::string>();
        if (action.rfind("usb_", 0) == 0)
          response = usb_action(action, j, cred.pid).dump() + "\n";
        else if(action=="sd_list")response=remoteos::sd::list().dump()+"\n";
        else if(action=="sd_mount")response=remoteos::sd::mount_card(j.at("device").get<std::string>(),j.value("writable",false)).dump()+"\n";
        else if(action=="sd_unmount"){remoteos::sd::unmount_all();response="{\"ok\":true}\n";}
        else if (action == "tailscale_prepare") {
          prepare_tailscale();
          response = "{\"ok\":true}\n";
        }
        else if (action == "suspend") {
          // The client releases input and closes its tunnels before requesting
          // sleep. Finish its local USB cleanup before freezing userspace.
          for (auto it = exports.begin(); it != exports.end();) {
            if (it->second.owner != cred.pid)
              throw std::runtime_error("Another client still owns a forwarded device");
            remoteos::devices::unbind_usb(it->first);
            it = exports.erase(it);
          }
          stop_exporter();
          remoteos::sd::unmount_all();
          remoteos::power::suspend();
          response = "{\"ok\":true,\"resumed\":true}\n";
        }
        else if (action != "status" && action != "poweroff" &&
                 action != "reboot")
          throw std::runtime_error("action not permitted");
        else
          response = "{\"ok\":true}\n";
      } catch (const std::exception &e) {
        response =
            remoteos::Json{{"ok", false}, {"error", e.what()}}.dump() + "\n";
        action.clear();
      }
      (void)send(peer, response.data(), response.size(), MSG_NOSIGNAL);
      close(peer);
      if (action == "poweroff" || action == "reboot") {
        pid_t child = fork();
        if (child == 0) {
          clearenv();
          setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
          execl(action == "reboot" ? "/sbin/reboot" : "/sbin/poweroff",
                action.c_str(), static_cast<char *>(nullptr));
          _exit(127);
        }
        if (child > 0)
          waitpid(child, nullptr, 0);
      }
    }
    close(fd);
    for (const auto &entry : exports) {
      try {
        remoteos::devices::unbind_usb(entry.first);
      } catch (const std::exception &e) {
        std::cerr << "USB shutdown cleanup: " << e.what() << '\n';
      }
    }
    stop_exporter();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
