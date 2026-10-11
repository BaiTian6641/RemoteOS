#pragma once
#include "remoteos/protocol.hpp"
#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <deque>
#include <functional>

namespace remoteos::net {
using tcp = asio::ip::tcp;
std::string fingerprint(X509 *);
void configure(asio::ssl::context &, const std::string &identity_dir);
class Channel : public std::enable_shared_from_this<Channel> {
public:
  asio::ssl::stream<tcp::socket> stream;
  std::function<void(Json)> on_message;
  std::function<void(std::string)> on_close;
  explicit Channel(asio::io_context &io, asio::ssl::context &ctx)
      : stream(io, ctx), timer_(io) {}
  void handshake(asio::ssl::stream_base::handshake_type, std::function<void()>);
  void send(Json);
  void close(const std::string &reason);
  std::string peer_fingerprint() const;
  std::string peer_address() const;

private:
  asio::steady_timer timer_;
  std::array<unsigned char, 4> header_{};
  std::vector<char> incoming_;
  struct Packet { std::shared_ptr<std::string> bytes; bool absolute_motion = false; };
  std::deque<Packet> queue_;
  std::size_t queued_bytes_ = 0;
  bool closed_ = false;
  void deadline();
  void read_header();
  void write_next();
};
} // namespace remoteos::net
