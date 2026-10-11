#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace remoteos::audio {
struct Session {
    bool available=false;
    std::uint32_t uid=0;
    std::uint64_t generation=0;
    std::string id,reason;
};
// Linux seat0 Xorg user audio. Greeters and tty sessions are excluded. Locked
// user sessions retain the same identity. Methods are serialized by the host.
class SessionSource {
public:
    explicit SessionSource(std::string helper="/usr/libexec/remoteos-audio-session");
    ~SessionSource();
    SessionSource(const SessionSource&)=delete;
    SessionSource& operator=(const SessionSource&)=delete;
    Session refresh();
    // New connected PipeWire socket acquired as the active user's UID/GID.
    // Caller owns this descriptor. Verify plugin fd-ownership semantics before
    // handing it to GStreamer; retain a duplicate when necessary. Throws on
    // unavailable socket, changed session, timeout or failed credential checks.
    int connect(const Session& expected);
private:struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace remoteos::audio
