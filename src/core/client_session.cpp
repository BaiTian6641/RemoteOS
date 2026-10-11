#include "remoteos/client_session.hpp"
#include "remoteos/identity.hpp"
#include "transport.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <openssl/crypto.h>
#include <thread>

namespace remoteos {
struct ClientSession::Impl {
  asio::io_context io;
  asio::ssl::context ctx{asio::ssl::context::tls};
  asio::steady_timer heartbeat{io};
  std::shared_ptr<net::Channel> channel;
  std::unique_ptr<net::tcp::resolver> resolver;
  std::thread thread;
  std::atomic<bool> active{false};
  ClientOptions options;
  Handler handler;
  std::string ping_nonce;
  std::chrono::steady_clock::time_point ping_sent;
  void emit(const Json &j) {
    if (handler)
      handler(j);
  }
  void ping() {
    heartbeat.expires_after(std::chrono::seconds(3));
    heartbeat.async_wait([this](std::error_code e) {
      if (!e && active) {
        ping_nonce = random_token(8);
        ping_sent = std::chrono::steady_clock::now();
        channel->send({{"type", "ping"}, {"nonce", ping_nonce}});
        ping();
      }
    });
  }
  void start() {
    try {
      ensure_identity(options.identity_dir, "RemoteOS client");
      net::configure(ctx, options.identity_dir);
      auto pinpath = std::filesystem::path(options.identity_dir) / "host.json";
      if (options.host_fingerprint.empty() &&
          std::filesystem::exists(pinpath)) {
        auto saved = Json::parse(read_private_file(pinpath));
        if (saved.value("host", "") == options.host &&
            saved.value("port", 0u) == options.port)
          options.host_fingerprint = saved.at("fingerprint");
      }
      options.host_fingerprint =
          normalize_fingerprint(options.host_fingerprint);
      channel = std::make_shared<net::Channel>(io, ctx);
      channel->on_close = [this](std::string why) {
        active = false;
        heartbeat.cancel();
        emit({{"type", "error"}, {"message", why}});
        emit({{"type", "state"}, {"state", "disconnected"}});
      };
      channel->on_message = [this, pinpath](Json j) {
        std::string type = j.at("type");
        if (type == "paired") {
          write_private_file(pinpath,
                             Json{{"host", options.host},
                                  {"port", options.port},
                                  {"fingerprint", options.host_fingerprint}}
                                 .dump());
          channel->send({{"type", "hello"}, {"version", protocol_version}});
        }
        if (type == "capabilities" && !options.displays.empty()) {
          auto requested = options.displays;
          const auto max_fps = j.value("media", Json::object()).value("mpeg2_max_fps", 30);
          for (auto &display : requested)
            if (display.codec == "mpeg2") display.fps = std::min(display.fps, max_fps);
          channel->send({{"type", "start"},
                         {"displays", requested},
                         {"media_transports", Json::array({"dtls-srtp", "rtp-udp"})},
                         {"mpeg2_payloads", Json::array({"gst-frame", "mpv"})},
                         {"low_latency", true},
                         {"live_video_reconfigure", options.live_video_reconfigure},
                         {"automatic_video_bitrate", options.automatic_video_bitrate},
                         {"bandwidth_limit_bps", options.bandwidth_limit_bps},
                         {"match_host_resolution", options.match_host_resolution},
                         {"client_fingerprint",
                          certificate_fingerprint(
                              std::filesystem::path(options.identity_dir) /
                              "certificate.pem")}});
        }
        if (type == "offer")
          j["host"] = channel->peer_address();
        if (type == "ping")
          channel->send(
              {{"type", "pong"}, {"nonce", j.value("nonce", std::string{})}});
        if (type == "pong" && !ping_nonce.empty() &&
            j.value("nonce", std::string{}) == ping_nonce) {
          const auto elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - ping_sent)
                                   .count();
          ping_nonce.clear();
          emit({{"type", "status"},
                {"latency_ms", elapsed},
                {"host_address", channel->peer_address()}});
        }
        if (type != "pong")
          emit(j);
      };
      resolver = std::make_unique<net::tcp::resolver>(io);
      emit({{"type", "state"}, {"state", "connecting"}});
      heartbeat.expires_after(std::chrono::seconds(10));
      heartbeat.async_wait([this](std::error_code e) {
        if (!e) {
          resolver->cancel();
          channel->close("connection timeout");
        }
      });
      resolver->async_resolve(
          options.host, std::to_string(options.port),
          [this](std::error_code e,
                 net::tcp::resolver::results_type endpoints) {
            if (e) {
              channel->close("DNS resolution failed: " + e.message());
              return;
            }
            asio::async_connect(
                channel->stream.next_layer(), endpoints,
                [this](std::error_code ec, const net::tcp::endpoint &) {
                  if (ec) {
                    channel->close("connect failed: " + ec.message());
                    return;
                  }
                  channel->handshake(asio::ssl::stream_base::client, [this] {
                    if (channel->peer_fingerprint() != options.host_fingerprint)
                      throw std::runtime_error(
                          "host fingerprint changed or did not match; pairing "
                          "required");
                    emit({{"type", "state"}, {"state", "authenticated"},
                          {"tls_cipher", SSL_get_cipher_name(channel->stream.native_handle())}});
                    if (options.pairing_code.empty())
                      channel->send(
                          {{"type", "hello"}, {"version", protocol_version}});
                    else {
                      channel->send(
                          {{"type", "pair"}, {"code", options.pairing_code}});
                      OPENSSL_cleanse(options.pairing_code.data(),
                                      options.pairing_code.size());
                      options.pairing_code.clear();
                    }
                    ping();
                  });
                });
          });
      io.run();
    } catch (const std::exception &e) {
      active = false;
      emit({{"type", "error"}, {"message", e.what()}});
    }
  }
};
ClientSession::ClientSession() : impl_(std::make_unique<Impl>()) {}
ClientSession::~ClientSession() { disconnect(); }
void ClientSession::connect(ClientOptions options, Handler handler) {
  disconnect();
  impl_ = std::make_unique<Impl>();
  impl_->options = std::move(options);
  impl_->handler = std::move(handler);
  impl_->active = true;
  impl_->thread = std::thread([p = impl_.get()] { p->start(); });
}
void ClientSession::send(const Json &j) {
  validate_message(j);
  auto *p = impl_.get();
  if (!p->active)
    return;
  asio::post(p->io, [p, j] {
    if (p->channel)
      p->channel->send(j);
  });
}
void ClientSession::disconnect() {
  auto *p = impl_.get();
  if (!p)
    return;
  p->active = false;
  asio::post(p->io, [p] {
    if (p->resolver)
      p->resolver->cancel();
    p->heartbeat.cancel();
    if (p->channel)
      p->channel->close("client disconnected");
  });
  if (p->thread.joinable())
    p->thread.join();
}
bool ClientSession::running() const { return impl_->active; }
} // namespace remoteos
