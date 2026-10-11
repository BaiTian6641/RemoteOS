#pragma once
#include "remoteos/tunnel.hpp"
#include <asio.hpp>
#include <future>
#include <map>
#include <memory>
#include <thread>

namespace remoteos::client {
// GTK uses this wrapper to keep every TunnelClient operation on one executor.
// stop() returns only after all network relays have stopped.
class UsbTunnels {
public:
  UsbTunnels()
      : work_(asio::make_work_guard(io_)), worker_([this] { io_.run(); }) {}
  ~UsbTunnels() {
    stop();
    work_.reset();
    io_.stop();
    if (worker_.joinable())
      worker_.join();
  }
  void open(const std::string &identity, const std::string &host, unsigned port,
            const std::string &fingerprint, const std::string &session,
            const std::string &bus, const std::string &token,
            unsigned local_port, remoteos::devices::TunnelStatus callback) {
    asio::post(io_, [=] {
      try {
        if (port < 1 || port > 65535 || local_port < 1 || local_port > 65535)
          throw std::runtime_error("Invalid USB relay port");
        auto tunnel = std::make_shared<remoteos::devices::TunnelClient>(
            io_, identity, host, std::uint16_t(port), fingerprint, session, bus,
            token, std::uint16_t(local_port));
        tunnels_[bus] = tunnel;
        tunnel->start(callback);
      } catch (const std::exception &error) {
        callback(false, error.what());
      }
    });
  }
  void close(const std::string &bus) {
    asio::post(io_, [this, bus] {
      auto found = tunnels_.find(bus);
      if (found != tunnels_.end()) {
        found->second->stop();
        tunnels_.erase(found);
      }
    });
  }
  void stop() {
    auto finished = std::make_shared<std::promise<void>>();
    auto future = finished->get_future();
    asio::post(io_, [this, finished] {
      for (auto &entry : tunnels_)
        entry.second->stop();
      tunnels_.clear();
      finished->set_value();
    });
    future.wait();
  }

private:
  asio::io_context io_;
  asio::executor_work_guard<asio::io_context::executor_type> work_;
  std::map<std::string, std::shared_ptr<remoteos::devices::TunnelClient>>
      tunnels_;
  std::thread worker_;
};
} // namespace remoteos::client
