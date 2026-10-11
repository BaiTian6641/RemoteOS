#include "transport.hpp"
#include "crypto_policy.hpp"
#include "remoteos/identity.hpp"
#include <filesystem>
#include <openssl/x509.h>

namespace remoteos::net {
std::string fingerprint(X509 *cert) {
  if (!cert)
    throw std::runtime_error("peer certificate missing");
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned n = 0;
  if (!X509_digest(cert, EVP_sha256(), bytes, &n))
    throw std::runtime_error("certificate digest failed");
  std::string out;
  const char *h = "0123456789abcdef";
  for (unsigned i = 0; i < n; ++i) {
    out += h[bytes[i] >> 4];
    out += h[bytes[i] & 15];
  }
  return out;
}
void configure(asio::ssl::context &c, const std::string &dir) {
  if (SSL_CTX_set_min_proto_version(c.native_handle(), TLS1_3_VERSION) != 1)
    throw std::runtime_error("TLS1.3 unavailable");
  if (SSL_CTX_set_ciphersuites(c.native_handle(),
      tls_ciphers(hardware_gcm_available())) != 1)
    throw std::runtime_error("TLS cipher configuration failed");
  // The connecting client chooses its CPU-appropriate suite. A modern host
  // must not override an Atom client's software-friendly ChaCha preference.
  SSL_CTX_clear_options(c.native_handle(), SSL_OP_CIPHER_SERVER_PREFERENCE);
  SSL_CTX_set_mode(c.native_handle(), SSL_MODE_RELEASE_BUFFERS);
  const auto identity = std::filesystem::u8path(dir);
  const auto certificate = read_private_file(identity / "certificate.pem");
  const auto key = read_private_file(identity / "private-key.pem");
  c.use_certificate_chain(asio::buffer(certificate));
  c.use_private_key(asio::buffer(key), asio::ssl::context::pem);
  c.set_verify_mode(asio::ssl::verify_peer |
                    asio::ssl::verify_fail_if_no_peer_cert);
  // Trust comes from explicit application pairing/pinning, never arbitrary CAs.
  c.set_verify_callback([](bool, asio::ssl::verify_context &vc) {
    auto *ctx = vc.native_handle();
    auto *cert = X509_STORE_CTX_get_current_cert(ctx);
    if (X509_STORE_CTX_get_error_depth(ctx) != 0 || !cert)
      return false;
    if (X509_cmp_current_time(X509_get0_notBefore(cert)) > 0 ||
        X509_cmp_current_time(X509_get0_notAfter(cert)) < 0)
      return false;
    const auto error = X509_STORE_CTX_get_error(ctx);
    return error == X509_V_OK ||
           error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT;
  });
}
void Channel::deadline() {
  timer_.expires_after(std::chrono::seconds(15));
  auto self = shared_from_this();
  timer_.async_wait([self](std::error_code ec) {
    if (!ec)
      self->close("peer timeout");
  });
}
void Channel::handshake(asio::ssl::stream_base::handshake_type role,
                        std::function<void()> ready) {
  std::error_code ec;
  stream.next_layer().set_option(tcp::no_delay(true), ec);
  deadline();
  auto self = shared_from_this();
  stream.async_handshake(role, [self, ready](std::error_code e) {
    if (e) {
      self->close("TLS handshake failed: " + e.message());
      return;
    }
    try {
      ready();
      if (!self->closed_)
        self->read_header();
    } catch (const std::exception &x) {
      self->close(x.what());
    }
  });
}
std::string Channel::peer_fingerprint() const {
  auto *c = SSL_get1_peer_certificate(
      const_cast<Channel *>(this)->stream.native_handle());
  auto result = fingerprint(c);
  X509_free(c);
  return result;
}
std::string Channel::peer_address() const {
  return stream.next_layer().remote_endpoint().address().to_string();
}
void Channel::read_header() {
  if (closed_)
    return;
  auto self = shared_from_this();
  asio::async_read(
      stream, asio::buffer(header_), [self](std::error_code ec, std::size_t) {
        if (ec) {
          self->close(ec.message());
          return;
        }
        std::size_t n = 0;
        for (auto b : self->header_)
          n = (n << 8) | b;
        if (n == 0 || n > max_control_frame) {
          self->close("invalid frame length");
          return;
        }
        self->incoming_.resize(n);
        asio::async_read(
            self->stream, asio::buffer(self->incoming_),
            [self](std::error_code e, std::size_t) {
              if (e) {
                self->close(e.message());
                return;
              }
              try {
                auto j = Json::parse(
                    self->incoming_.begin(), self->incoming_.end(),
                    [](int depth, Json::parse_event_t, Json &) {
                      if (depth > 16)
                        throw std::invalid_argument("JSON nesting limit");
                      return true;
                    });
                validate_message(j);
                self->deadline();
                auto callback = self->on_message;
                if (callback)
                  callback(std::move(j));
              } catch (const std::exception &x) {
                self->close(std::string("protocol error: ") + x.what());
                return;
              }
              self->read_header();
            });
      });
}
void Channel::send(Json j) {
  auto self = shared_from_this();
  asio::post(stream.get_executor(), [self, j = std::move(j)] {
    if (self->closed_)
      return;
    try {
      validate_message(j);
      auto payload = j.dump();
      auto packet = std::make_shared<std::string>(4, '\0');
      auto n = payload.size();
      for (int i = 3; i >= 0; --i) {
        (*packet)[std::size_t(i)] = char(n & 255);
        n >>= 8;
      }
      *packet += payload;
      const bool motion = j.value("type", "") == "input" && j.value("kind", "") == "motion" && j.contains("display");
      // Absolute motion is replaceable state. Keep the newest adjacent
      // pending position, without replacing the in-flight packet or crossing
      // a key/button/scroll/release boundary.
      if (motion && self->queue_.size() > 1 && self->queue_.back().absolute_motion) {
        self->queued_bytes_ -= self->queue_.back().bytes->size();
        self->queue_.pop_back();
      }
      if (self->queued_bytes_ + packet->size() > 256 * 1024) {
        self->close("control backpressure limit");
        return;
      }
      self->queued_bytes_ += packet->size();
      self->queue_.push_back({packet, motion});
      if (self->queue_.size() == 1)
        self->write_next();
    } catch (const std::exception &x) {
      self->close(x.what());
    }
  });
}
void Channel::write_next() {
  if (closed_ || queue_.empty())
    return;
  auto self = shared_from_this();
  auto packet = queue_.front().bytes;
  asio::async_write(stream, asio::buffer(*packet),
                    [self, packet](std::error_code e, std::size_t) {
                      if (e) {
                        self->close(e.message());
                        return;
                      }
                      self->queued_bytes_ -= packet->size();
                      self->queue_.pop_front();
                      self->write_next();
                    });
}
void Channel::close(const std::string &why) {
  if (closed_)
    return;
  closed_ = true;
  std::error_code ec;
  timer_.cancel();
  stream.lowest_layer().cancel(ec);
  stream.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
  stream.lowest_layer().close(ec);
  auto callback = std::move(on_close);
  on_message = {};
  if (callback)
    callback(why);
}
} // namespace remoteos::net
