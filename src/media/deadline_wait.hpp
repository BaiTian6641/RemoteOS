#pragma once
#include <chrono>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace remoteos::media_detail {
// Owned by one RTP stream. Windows' ordinary sleeps can round a submillisecond
// packet deadline up to a scheduler tick. This timer changes no global clock
// resolution and requires no elevation.
class DeadlineWaiter {
#ifdef _WIN32
  HANDLE timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
public:
  DeadlineWaiter() = default;
  DeadlineWaiter(const DeadlineWaiter&) = delete;
  DeadlineWaiter& operator=(const DeadlineWaiter&) = delete;
  ~DeadlineWaiter() {
#ifdef _WIN32
    if (timer_) CloseHandle(timer_);
#endif
  }
  bool high_resolution() const noexcept {
#ifdef _WIN32
    return timer_ != nullptr;
#else
    return true;
#endif
  }
  void until(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return;
    LARGE_INTEGER due{}; due.QuadPart = -((left + 99) / 100);
    if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer_, INFINITE);
      return;
    }
#endif
    std::this_thread::sleep_until(deadline);
  }
};
}
