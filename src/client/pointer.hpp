#pragma once
#include "remoteos/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace remoteos::client {
struct ScreenRectangle { int x, y, width, height; };
// Xorg/libinput has already converted device units to desktop pixels. Match
// those pixels to the actual displayed viewport, including VGA letterboxing.
inline std::optional<Json> pointer_message(const std::vector<Display>& displays,
                                          const std::vector<ScreenRectangle>& screens,
                                          double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y)) return {};
  for (std::size_t i = 0; i < displays.size() && i < screens.size(); ++i) {
    const auto& d = displays[i]; const auto& screen = screens[i];
    if (screen.width < 1 || screen.height < 1 || x < screen.x || x >= screen.x + screen.width || y < screen.y || y >= screen.y + screen.height) continue;
    const auto factor = std::min(double(screen.width) / d.encoded_width, double(screen.height) / d.encoded_height);
    const auto width = d.encoded_width * factor, height = d.encoded_height * factor;
    const auto left = screen.x + (screen.width - width) / 2, top = screen.y + (screen.height - height) / 2;
    const auto px = std::clamp((x - left) * (d.encoded_width - 1) / std::max(1.0, width - 1), 0.0, double(d.encoded_width - 1));
    const auto py = std::clamp((y - top) * (d.encoded_height - 1) / std::max(1.0, height - 1), 0.0, double(d.encoded_height - 1));
    return Json{{"type", "input"}, {"kind", "motion"}, {"display", d.id}, {"x", px}, {"y", py}};
  }
  return {};
}
}
