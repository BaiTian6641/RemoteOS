#pragma once
#include "remoteos/protocol.hpp"
#include <functional>
#include <memory>

namespace remoteos {
// GStreamer implementation is optional in native development, mandatory in
// appliance.
bool media_available();
Json media_capabilities();
class MediaReceiver {
public:
  MediaReceiver();
  ~MediaReceiver();
  using Handler = std::function<void(const Json &)>;
  void start(const Json &offer, const std::vector<std::uintptr_t> &windows,
             const std::string &identity_pem, Handler);
  void stop();
  void authorize();
  // Refresh application-owned video windows after their native size changes.
  void refresh_windows();
  // Local presentation selection; never supplied by an untrusted host offer.
  void set_desktop_output(bool enabled);
  bool update_profiles(const std::vector<Display> &profiles);
  Json stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class MediaSender {
public:
  MediaSender();
  ~MediaSender();
  void start(const Json &offer, const std::string &identity_pem,
             std::function<void(const Json &)>);
  void frame(const std::string &id, int width, int height, int stride,
             const std::uint8_t *bytes, std::size_t size);
  void authorize();
  bool reconfigure(const std::vector<Display> &profiles);
  void request_keyframe(const std::string &id = {});
  void stop();
  Json stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace remoteos
