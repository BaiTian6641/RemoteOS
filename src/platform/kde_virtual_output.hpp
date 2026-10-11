#pragma once
#include <memory>
#include <string>

namespace remoteos::platform {
// KDE-specific adapter. It only creates and owns a virtual output. Capture and
// all input remain scoped to the application's RemoteDesktop portal grant.
class KdeVirtualOutput {
public:
    KdeVirtualOutput(unsigned width, unsigned height);
    ~KdeVirtualOutput();
    KdeVirtualOutput(const KdeVirtualOutput&) = delete;
    KdeVirtualOutput& operator=(const KdeVirtualOutput&) = delete;
    void pump();
    bool ready() const;
    const std::string& output_name() const;
    unsigned width() const;
    unsigned height() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace remoteos::platform
