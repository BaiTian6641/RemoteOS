#pragma once
#include "remoteos/host_platform.hpp"
#include <set>
#include <stdexcept>

namespace remoteos::platform::detail {
// Capture works in BGRA and is independent of the negotiated wire codec.
// MPEG-2's lower rate limit is enforced by the control protocol.
inline void validate_displays(const std::vector<DisplayConfig>& displays) {
    if(displays.empty() || displays.size()>2)
        throw std::invalid_argument("One or two displays are required");
    std::set<std::uint32_t> ids;
    for(const auto& d:displays)
        if(!ids.insert(d.id).second || d.id>255 || d.width<320 || d.width>1920 ||
           d.height<200 || d.height>1080 || d.fps<1 || d.fps>60 ||
           d.x<0 || d.y<0 || d.x>3840 || d.y>2160)
            throw std::invalid_argument("Invalid logical display configuration");
}
inline std::chrono::microseconds capture_period(unsigned fps) {
    if(fps<1 || fps>60)throw std::invalid_argument("Invalid capture frame rate");
    return std::chrono::microseconds((1000000+fps-1)/fps);
}
inline std::chrono::steady_clock::time_point next_capture(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point previous,unsigned fps) {
    const auto period=capture_period(fps);
    // Keep the target phase despite small event/poll jitter; never catch up a
    // long stall by producing a burst of old frames.
    return now-previous>=period ? now+period : previous+period;
}
} // namespace remoteos::platform::detail
