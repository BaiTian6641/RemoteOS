#pragma once
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>
namespace remoteos::client {
// EV_KEY includes buttons and keyboard keys in disjoint ranges. Dedicated
// keypad, consumer-control and WMI devices need not advertise KEY_A.
constexpr bool keyboard_key(unsigned code) {
  return code != KEY_POWER &&
         ((code > KEY_RESERVED && code < BTN_MISC) ||
          (code >= KEY_OK && code < BTN_DPAD_UP) ||
          (code >= KEY_ALS_TOGGLE && code < BTN_TRIGGER_HAPPY));
}
struct InputEvent {
  unsigned short type, code;
  int value;
};
// Owns evdev keyboard descriptors and rescans for hotplug. Xorg keeps pointing
// devices so GTK receives processed motion. The ACPI power button stays local.
class InputBroker {
public:
  using EventHandler = std::function<void(InputEvent)>;
  InputBroker(EventHandler, std::function<void()> power,
              std::function<void()> release,
              std::function<void(const std::string &)> failure,
              std::function<void(bool)> lid = {});
  ~InputBroker();
  void set_forwarding(bool enabled);
  bool forwarding() const;
  void rescan();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace remoteos::client
