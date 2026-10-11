#include "input.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <glib-unix.h>
#include <linux/input.h>
#include <map>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace remoteos::client {
namespace {
constexpr std::size_t word_bits = sizeof(unsigned long) * 8;
template <std::size_t N>
bool bit(const std::array<unsigned long, N> &bits, unsigned code) {
  return code / word_bits < bits.size() &&
         ((bits[code / word_bits] >> (code % word_bits)) & 1UL);
}
} // namespace
struct InputBroker::Impl {
  struct Device {
    Impl *owner;
    int fd;
    guint watch;
    std::string path;
    bool power;
    bool lid;
    bool input;
    bool grabbed = false;
    bool dropping = false;
  };
  std::map<std::string, std::unique_ptr<Device>> devices;
  EventHandler event;
  std::function<void()> power, release;
  std::function<void(bool)> lid;
  std::function<void(const std::string &)> failure;
  std::string inaccessible;
  bool active = false;
  guint timer = 0;
  Impl(EventHandler e, std::function<void()> p, std::function<void()> r,
       std::function<void(const std::string &)> f, std::function<void(bool)> l)
      : event(std::move(e)), power(std::move(p)), release(std::move(r)),
        failure(std::move(f)), lid(std::move(l)) {}
  static gboolean ready(gint fd, GIOCondition condition, gpointer data) {
    auto *d = static_cast<Device *>(data);
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
      d->owner->release();
      d->watch = 0;
      return G_SOURCE_REMOVE;
    }
    input_event e{};
    while (::read(fd, &e, sizeof(e)) == sizeof(e)) {
      if (e.type == EV_SYN && e.code == SYN_DROPPED) {
        d->dropping = true;
        d->owner->release();
        continue;
      }
      if (d->dropping) {
        if (e.type == EV_SYN && e.code == SYN_REPORT)
          d->dropping = false;
        continue;
      }
      if (e.type == EV_KEY && e.code == KEY_POWER) {
        if (e.value == 1)
          d->owner->power();
        continue;
      }
      if (e.type == EV_SW && e.code == SW_LID) {
        if (d->owner->lid) d->owner->lid(e.value != 0);
        continue;
      }
      if (d->owner->active && d->grabbed)
        d->owner->event({e.type, e.code, e.value});
    }
    return G_SOURCE_CONTINUE;
  }
  void scan() {
    std::error_code error;
    std::vector<std::string> seen;
    inaccessible.clear();
    std::string grab_failure;
    for (const auto &file :
         std::filesystem::directory_iterator("/dev/input", error)) {
      if (file.path().filename().string().rfind("event", 0) != 0)
        continue;
      auto path = file.path().string();
      seen.push_back(path);
      if (devices.count(path))
        continue;
      const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) {
        inaccessible = path;
        continue;
      }
      std::array<unsigned long, (KEY_MAX / word_bits) + 1> keys{};
      std::array<unsigned long, (REL_MAX / word_bits) + 1> rel{};
      std::array<unsigned long, (ABS_MAX / word_bits) + 1> abs{};
      std::array<unsigned long, (SW_MAX / word_bits) + 1> switches{};
      ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys.data());
      ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel.data());
      ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs.data());
      ioctl(fd, EVIOCGBIT(EV_SW, sizeof(switches)), switches.data());
      const bool has_power = bit(keys, KEY_POWER);
      const bool has_lid = bit(switches, SW_LID);
      bool has_keyboard = false;
      for (unsigned code = 1; code <= KEY_MAX; ++code)
        if (keyboard_key(code) && bit(keys, code)) {
          has_keyboard = true;
          break;
        }
      const bool pointer = bit(keys, BTN_LEFT) || bit(keys, BTN_TOUCH) ||
                           bit(rel, REL_X) || bit(abs, ABS_X);
      // Xorg/libinput owns pointing devices. Raw touchpad units are neither
      // pixels nor relative mouse motion and must not be forwarded as such.
      // evdev nodes for keyboards and consumer keys remain exclusive.
      const bool has_input = has_keyboard && !pointer;
      if (!has_power && !has_lid && !has_input) {
        close(fd);
        continue;
      }
      auto d = std::make_unique<Device>();
      d->owner = this;
      d->fd = fd;
      d->path = path;
      d->power = has_power;
      d->lid = has_lid;
      d->input = has_input;
      // A dedicated power device can remain grabbed even during setup;
      // mixed keyboard devices only become exclusive for the session.
      d->grabbed =
          ioctl(fd, EVIOCGRAB, active || (!has_input && (has_power || has_lid))) == 0 &&
          (active || (!has_input && (has_power || has_lid)));
      if (active && !d->grabbed)
        grab_failure = path;
      d->watch = g_unix_fd_add(
          fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP), ready,
          d.get());
      devices.emplace(path, std::move(d));
    }
    for (auto it = devices.begin(); it != devices.end();) {
      if (std::find(seen.begin(), seen.end(), it->first) == seen.end() ||
          !it->second->watch) {
        release();
        if (it->second->watch)
          g_source_remove(it->second->watch);
        ioctl(it->second->fd, EVIOCGRAB, 0);
        close(it->second->fd);
        it = devices.erase(it);
      } else
        ++it;
    }
    if (active && (!grab_failure.empty() || !inaccessible.empty())) {
      active = false;
      release();
      for (auto &entry : devices) {
        ioctl(entry.second->fd, EVIOCGRAB, 0);
        entry.second->grabbed = false;
      }
      failure("Cannot exclusively access hotplugged input: " +
              (!grab_failure.empty() ? grab_failure : inaccessible));
    }
  }
};
InputBroker::InputBroker(EventHandler event, std::function<void()> power,
                         std::function<void()> release,
                         std::function<void(const std::string &)> failure,
                         std::function<void(bool)> lid)
    : impl_(std::make_unique<Impl>(std::move(event), std::move(power),
                                   std::move(release), std::move(failure), std::move(lid))) {
  impl_->scan();
  impl_->timer = g_timeout_add_seconds(
      2,
      [](gpointer p) -> gboolean {
        static_cast<Impl *>(p)->scan();
        return G_SOURCE_CONTINUE;
      },
      impl_.get());
}
InputBroker::~InputBroker() {
  set_forwarding(false);
  if (impl_->timer)
    g_source_remove(impl_->timer);
  for (auto &entry : impl_->devices) {
    if (entry.second->watch)
      g_source_remove(entry.second->watch);
    ioctl(entry.second->fd, EVIOCGRAB, 0);
    close(entry.second->fd);
  }
}
void InputBroker::set_forwarding(bool enabled) {
  if (impl_->active == enabled)
    return;
  impl_->release();
  impl_->active = enabled;
  std::string failure;
  if (enabled && !impl_->inaccessible.empty())
    failure = "Cannot read input device " + impl_->inaccessible;
  bool ordinary = false;
  for (auto &entry : impl_->devices) {
    auto &device = *entry.second;
    ordinary = ordinary || device.input;
    if (!enabled) {
      const bool dedicated_power = (device.power || device.lid) && !device.input;
      device.grabbed = dedicated_power && ioctl(device.fd, EVIOCGRAB, 1) == 0;
      if (!dedicated_power) ioctl(device.fd, EVIOCGRAB, 0);
    } else {
      device.grabbed = device.grabbed || ioctl(device.fd, EVIOCGRAB, 1) == 0;
      if (!device.grabbed)
        failure = "Cannot exclusively grab " + device.path;
    }
  }
  if (enabled && !ordinary)
    failure = "No readable keyboard or pointing device; check input-group "
              "permissions";
  if (!failure.empty()) {
    impl_->active = false;
    for (auto &entry : impl_->devices) {
      ioctl(entry.second->fd, EVIOCGRAB, 0);
      entry.second->grabbed = false;
    }
    impl_->release();
    impl_->failure(failure);
  }
}
bool InputBroker::forwarding() const { return impl_->active; }
void InputBroker::rescan() { impl_->scan(); }
} // namespace remoteos::client
