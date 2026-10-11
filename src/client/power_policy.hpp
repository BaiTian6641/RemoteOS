#pragma once
#include <string>

namespace remoteos::client {
enum class BatteryWarning { none, low, critical };
// One warning per threshold per discharge cycle, with recovery hysteresis.
class BatteryAlerts {
public:
  BatteryWarning observe(int percent, const std::string& state) {
    if (percent < 0 || percent > 100) return BatteryWarning::none;
    if (state != "Discharging" || percent > 20) {
      low_ = critical_ = false;
      return BatteryWarning::none;
    }
    if (percent <= 5 && !critical_) {
      low_ = critical_ = true;
      return BatteryWarning::critical;
    }
    if (percent <= 15 && !low_) {
      low_ = true;
      return BatteryWarning::low;
    }
    return BatteryWarning::none;
  }
private:
  bool low_ = false, critical_ = false;
};
}
