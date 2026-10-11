#include "power_policy.hpp"
#include "suspend.hpp"
#include "sd_card.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
  using namespace remoteos::client;
  char path[] = "/tmp/remoteos-power-fixture-XXXXXX";
  auto* root = mkdtemp(path); if (!root) return 2;
  struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove_all(p); } } cleanup{root};
  try {
    BatteryAlerts alerts;
    require(alerts.observe(-1,"Discharging") == BatteryWarning::none,"Unknown battery warned");
    require(alerts.observe(15,"Discharging") == BatteryWarning::low,"Low threshold missing");
    require(alerts.observe(14,"Discharging") == BatteryWarning::none,"Low warning repeated");
    require(alerts.observe(5,"Discharging") == BatteryWarning::critical,"Critical threshold missing");
    require(alerts.observe(4,"Discharging") == BatteryWarning::none,"Critical warning repeated");
    require(alerts.observe(4,"Charging") == BatteryWarning::none,"Charging warning");
    require(alerts.observe(4,"Discharging") == BatteryWarning::critical,"Next discharge did not rearm");
    require(alerts.observe(21,"Discharging") == BatteryWarning::none,"Recovery hysteresis");
    require(alerts.observe(15,"Discharging") == BatteryWarning::low,"Recovered battery did not rearm");
    std::ofstream(cleanup.p/"state") << "freeze mem disk";
    std::ofstream(cleanup.p/"mem_sleep") << "[s2idle] deep";
    remoteos::power::suspend(cleanup.p); // Only regular files in a private fixture.
    std::string state, mode;
    std::ifstream(cleanup.p/"state") >> state; std::ifstream(cleanup.p/"mem_sleep") >> mode;
    require(state == "mem" && mode == "deep","Supported deep suspend not selected");
    std::ofstream(cleanup.p/"state") << "freeze";
    bool rejected=false;
    try { remoteos::power::suspend(cleanup.p); } catch (const std::exception&) { rejected=true; }
    require(rejected,"Unsupported suspend accepted");
    const auto sys=cleanup.p/"sys",mounts=cleanup.p/"mountinfo";
    for(const auto* disk:{"mmcblk0","mmcblk1"}){
      const auto base=sys/"devices"/disk;std::filesystem::create_directories(base/(std::string(disk)+"p1"));std::filesystem::create_directories(sys/"class/block");
      std::ofstream(base/(std::string(disk)+"p1")/"partition")<<"1";
      std::filesystem::create_symlink(base/(std::string(disk)+"p1"),sys/"class/block"/(std::string(disk)+"p1"));
    }
    std::filesystem::create_directories(sys/"dev/block");std::filesystem::create_symlink(sys/"devices/mmcblk0/mmcblk0p1",sys/"dev/block/179:1");
    std::ofstream(mounts)<<"20 1 179:1 / / ro - squashfs /dev/root ro\n";
    require(remoteos::sd::system_volume("mmcblk0p1",sys/"class/block",mounts,sys/"dev/block"),"Boot card alias escaped protection");
    require(!remoteos::sd::system_volume("mmcblk1p1",sys/"class/block",mounts,sys/"dev/block"),"Other SD card incorrectly protected");
    std::cout << "Battery thresholds, hysteresis and isolated suspend-file contract passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
