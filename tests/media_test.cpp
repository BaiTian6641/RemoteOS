#include "../src/media/raw_frame.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <openssl/evp.h>
#include <thread>
using namespace remoteos;
bool white_pixel(unsigned x, unsigned y) {
  return (((x / 2) * 17 + (y / 2) * 11) % 23) < 11;
}
std::string expected_i420(unsigned width, unsigned height, unsigned white_u) {
  std::vector<unsigned char> pixels(std::size_t(width) * height * 3 / 2, 128);
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x)
      pixels[std::size_t(y) * width + x] = white_pixel(x, y) ? 235 : 16;
  for (unsigned y = 0; y < height / 2; ++y)
    for (unsigned x = 0; x < width / 2; ++x)
      pixels[std::size_t(width) * height + std::size_t(y) * (width / 2) + x] =
          white_pixel(x * 2, y * 2) ? white_u : 128;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (!EVP_Digest(pixels.data(), pixels.size(), digest, &size, EVP_sha256(),
                  nullptr))
    throw std::runtime_error("cannot hash expected I420 pattern");
  std::string result;
  for (unsigned i = 0; i < size; ++i) {
    result += "0123456789abcdef"[digest[i] >> 4];
    result += "0123456789abcdef"[digest[i] & 15];
  }
  return result;
}
// The SRTP RTP header remains visible and authenticated. This local test relay
// only drops datagrams; it never decrypts, edits, fabricates or bypasses DTLS.
class LossRelay {
public:
  LossRelay(unsigned host, unsigned client)
      : socket_(io_, {asio::ip::address_v4::loopback(), 0}),
        host_(asio::ip::address_v4::loopback(), host),
        client_(asio::ip::address_v4::loopback(), client) {
    socket_.set_option(asio::socket_base::receive_buffer_size(4 * 1024 * 1024));
    receive();
    thread_ = std::thread([this] { io_.run(); });
  }
  ~LossRelay() {
    asio::post(io_, [this] { socket_.close(); });
    if (thread_.joinable())
      thread_.join();
  }
  unsigned port() const { return socket_.local_endpoint().port(); }
  void drop_boundary() { stage_ = 1; }
  unsigned dropped() const { return dropped_; }
  std::uint64_t charged_bytes() const { return charged_bytes_; }

private:
  void receive() {
    socket_.async_receive_from(
        asio::buffer(data_), from_,
        [this](std::error_code error, std::size_t count) {
          if (error)
            return;
          bool drop = false;
          if (from_ == host_) charged_bytes_ += count + 128;
          if (from_ == host_ && count >= 12 && (data_[0] & 0xc0) == 0x80 &&
              (data_[1] & 0x7f) == 96) {
            const auto timestamp = (std::uint32_t(data_[4]) << 24) |
                                   (std::uint32_t(data_[5]) << 16) |
                                   (std::uint32_t(data_[6]) << 8) | data_[7];
            if (stage_ == 1 && (data_[1] & 0x80)) {
              last_timestamp_ = timestamp;
              stage_ = 2;
              drop = true;
            } else if (stage_ == 2 && timestamp != last_timestamp_) {
              stage_ = 3;
              drop = true;
            }
          }
          if (drop)
            ++dropped_;
          else if (from_ == host_ || from_ == client_) {
            std::error_code ignored;
            socket_.send_to(asio::buffer(data_.data(), count),
                            from_ == host_ ? client_ : host_, 0, ignored);
          }
          receive();
        });
  }
  asio::io_context io_;
  asio::ip::udp::socket socket_;
  asio::ip::udp::endpoint host_, client_, from_;
  std::array<unsigned char, 65536> data_{};
  std::thread thread_;
  std::atomic<unsigned> stage_{0}, dropped_{0};
  std::atomic<std::uint64_t> charged_bytes_{0};
  std::uint32_t last_timestamp_ = 0;
};
void coverage_tests() {
  using remoteos::media_detail::RawFrameCoverage;
  const auto packet = [](unsigned line, unsigned column, unsigned bytes) {
    std::vector<std::uint8_t> p(8 + bytes, 0);
    p[2] = bytes >> 8;
    p[3] = bytes;
    p[4] = line >> 8;
    p[5] = line;
    p[6] = column >> 8;
    p[7] = column;
    return p;
  };
  const auto top = packet(0, 0, 18), bottom = packet(2, 0, 18);
  auto add = [](RawFrameCoverage &coverage, unsigned timestamp, bool marker,
                const std::vector<std::uint8_t> &bytes) {
    coverage.packet(timestamp, marker, bytes.data(), bytes.size());
  };
  RawFrameCoverage c(6, 4);
  add(c, 10, false, top);
  add(c, 10, true, bottom);
  if (!c.output())
    throw std::runtime_error("complete raw frame rejected");
  // Lose frame A's bottom/marker and frame B's beginning. The next B packet
  // makes the depayloader flush A then B; both must independently be rejected.
  add(c, 20, false, top);
  add(c, 30, true, bottom);
  if (c.output() || c.output())
    throw std::runtime_error("two consecutive partial raw frames accepted");
  add(c, 40, false, top);
  add(c, 40, true, bottom);
  if (!c.output())
    throw std::runtime_error("raw coverage did not recover");
  add(c, 50, false, top);
  add(c, 50, true, top);
  if (c.output())
    throw std::runtime_error("overlapping raw segments accepted");
  auto short_payload = bottom;
  short_payload.pop_back();
  add(c, 60, false, top);
  add(c, 60, true, short_payload);
  if (c.output())
    throw std::runtime_error("truncated raw payload accepted");
  add(c, 70, false, top);
  add(c, 70, false, bottom);
  add(c, 80, false, top);
  if (c.output())
    throw std::runtime_error("missing raw end marker accepted");
  add(c, 80, true, bottom);
  if (!c.output())
    throw std::runtime_error("raw marker recovery wrong");
  RawFrameCoverage skipped(6, 4);
  add(skipped, 90, false, top);
  add(skipped, 90, true, bottom);
  // Simulate the depayloader declining to emit its expected complete frame.
  add(skipped, 100, true, bottom);
  if (!skipped.failed() || skipped.output())
    throw std::runtime_error("raw verdict drift did not fail closed");
  RawFrameCoverage unexpected(6, 4);
  if (unexpected.output() || !unexpected.failed())
    throw std::runtime_error("unexpected raw output did not fail closed");
}
unsigned free_port() {
  asio::io_context io;
  asio::ip::udp::socket socket(io, {asio::ip::address_v4::loopback(), 0});
  return socket.local_endpoint().port();
}
int main() {
  try {
    coverage_tests();
    auto root = std::filesystem::temp_directory_path() /
                ("remoteos-media-" + random_token(6));
    ensure_identity(root / "host", "host");
    ensure_identity(root / "client", "client");
    std::atomic<bool> sender_verified{false}, receiver_verified{false},
        failed{false};
    std::string failure;
    std::mutex mutex;
    MediaSender sender;
    MediaReceiver receiver;
    std::unique_ptr<LossRelay> relay;
    std::map<std::string, std::unique_ptr<LossRelay>> rate_relays;
    Display internal{"internal", 320, 240, 0, 0, 320, 240, 30, 1000000};
    Display external = internal;
    external.id = "vga";
    external.x = 320;
    Json offer = {{"session", random_token(12)},
                  {"host", "127.0.0.1"},
                  {"host_fingerprint",
                   certificate_fingerprint(root / "host/certificate.pem")},
                  {"client_fingerprint",
                   certificate_fingerprint(root / "client/certificate.pem")},
                  {"audio_source", "test"},
                  {"streams", Json::array()}};
    for (const auto &display : {internal, external})
      offer["streams"].push_back({{"id", display.id},
                                  {"kind", "video"},
                                  {"host_port", free_port()},
                                  {"client_port", free_port()},
                                  {"wire_limit_bps", 2000000},
                                  {"profile", display}});
    offer["streams"].push_back({{"id", "playback"},
                                {"kind", "audio"},
                                {"host_port", free_port()},
                                {"wire_limit_bps", 500000},
                                {"client_port", free_port()}});
    auto handler = [&](bool sending, const Json &event) {
      if (event.at("type") == "media_verified")
        (sending ? sender_verified : receiver_verified) = true;
      else if (event.at("type") == "error") {
        std::lock_guard<std::mutex> lock(mutex);
        failure = event.value("message", "");
        failed = true;
      }
    };
    auto start = [&] {
      auto sender_offer = offer, receiver_offer = offer;
      for (std::size_t i = 0; i < offer["streams"].size(); ++i) {
        const auto id = offer["streams"][i].at("id").get<std::string>();
        if (rate_relays.count(id)) {
          sender_offer["streams"][i]["client_port"] = rate_relays.at(id)->port();
          receiver_offer["streams"][i]["host_port"] = rate_relays.at(id)->port();
        }
      }
      if (relay) {
        sender_offer["streams"][0]["client_port"] = relay->port();
        receiver_offer["streams"][0]["host_port"] = relay->port();
      }
      sender.start(sender_offer, combined_identity_pem(root / "host"),
                   [&](const auto &event) { handler(true, event); });
      // Explicit zero windows select non-rendering/non-audible test sinks.
      receiver.start(receiver_offer, {0, 0},
                     combined_identity_pem(root / "client"),
                     [&](const auto &event) { handler(false, event); });
    };
    auto fail_reason = [&] {
      std::lock_guard<std::mutex> lock(mutex);
      return failure;
    };
    start();
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!failed && !(sender_verified && receiver_verified) &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (failed || !sender_verified || !receiver_verified)
      throw std::runtime_error("media authentication: " + fail_reason());
    std::map<std::string, std::vector<std::uint8_t>> pixel_buffers;
    unsigned feed_interval_ms = 35;
    bool noise = false;
    std::uint32_t random_pixels = 27;
    auto feed = [&](unsigned milliseconds) {
      auto until = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(milliseconds);
      while (std::chrono::steady_clock::now() < until) {
        for (const auto &stream : offer.at("streams")) {
          if (stream.at("kind") != "video")
            continue;
          const auto display = stream.at("profile").get<Display>();
          auto &pixels = pixel_buffers[display.id];
          const auto bytes = std::size_t(display.width) * display.height * 4;
          if (pixels.size() != bytes) {
            pixels.resize(bytes, 128);
            if (display.codec == "raw-i420")
              for (unsigned y = 0; y < unsigned(display.height); ++y)
                for (unsigned x = 0; x < unsigned(display.width); ++x) {
                  const auto offset = (std::size_t(y) * display.width + x) * 4;
                  pixels[offset] = pixels[offset + 1] = pixels[offset + 2] =
                      white_pixel(x, y) ? 255 : 0;
                  pixels[offset + 3] = 255;
                }
          }
          if (noise) for (auto& byte : pixels) { random_pixels = random_pixels * 1664525u + 1013904223u; byte = random_pixels >> 24; }
          sender.frame(display.id, display.width, display.height,
                       display.width * 4, pixels.data(), pixels.size());
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(feed_interval_ms));
      }
    };
    auto expect_silent = [&] {
      const auto stats = receiver.stats();
      if (stats.value("frames", 0) != 0 || stats.value("audio_buffers", 0) != 0)
        throw std::runtime_error("preauthorization video/audio leak");
    };
    feed(200);
    expect_silent();
    sender.authorize();
    feed(200);
    expect_silent();
    receiver.authorize();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    Json stats;
    do {
      feed(100);
      stats = receiver.stats();
    } while (!failed && std::chrono::steady_clock::now() < until &&
             (stats.value("frames", 0) < 20 ||
              stats.value("audio_buffers", 0) < 20));
    if (failed || stats.value("frames", 0) < 20 ||
        stats.value("audio_buffers", 0) < 20)
      throw std::runtime_error("MPEG2/Opus decode failed: " + fail_reason() +
                               " stats=" + stats.dump());
    for (const auto &stream : stats.at("streams"))
      if (stream.value("buffers", 0) < 5)
        throw std::runtime_error("one media stream did not decode: " +
                                 stream.dump());
    if (!stats.contains("loss") || !stats.contains("queue_ms"))
      throw std::runtime_error("measured RTP statistics unavailable");
    // An idle desktop must still present smoothly, without MTU batching,
    // dual cadence gates, decoder reordering or unprocessed LATENCY messages.
    receiver.stats();
    feed_interval_ms = 3;
    feed(1800);
    stats = receiver.stats();
    for (const auto &stream : stats.at("streams")) if (stream.at("kind") == "video") {
      if (stream.at("fps").get<double>() < 25 ||
          stream.at("playout_latency_ms").get<double>() > 80 ||
          stream.at("drop_fraction").get<double>() > 0.08)
        throw std::runtime_error("native MPEG-2 presentation cadence/latency regressed: " + stream.dump());
    }
    std::cout << "Steady low-delay MPEG-2 presentation: " << stats.dump() << '\n';
    feed_interval_ms = 35;
    const auto decoded = stats;
    sender.stop();
    receiver.stop();

    // Saturate independently reserved streams through a real loopback UDP
    // observer. It observes encrypted datagrams only, including DTLS/RTCP.
    offer["session"] = random_token(12);
    for (auto& stream : offer["streams"]) {
      stream["wire_limit_bps"] = 500000;
      const auto id = stream.at("id").get<std::string>();
      rate_relays[id] = std::make_unique<LossRelay>(stream["host_port"], stream["client_port"]);
    }
    sender_verified = receiver_verified = failed = false;
    start();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!failed && !(sender_verified && receiver_verified) && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (failed || !sender_verified || !receiver_verified) throw std::runtime_error("paced DTLS session failed: " + fail_reason());
    sender.authorize(); receiver.authorize(); noise = true;
    std::map<std::string, std::uint64_t> initial_bytes;
    for (const auto& entry : rate_relays) initial_bytes[entry.first] = entry.second->charged_bytes();
    const auto rate_started = std::chrono::steady_clock::now();
    feed(2200); noise = false;
    const auto rate_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_started).count();
    for (const auto& entry : rate_relays) {
      const auto bytes = entry.second->charged_bytes() - initial_bytes[entry.first];
      const auto measured = bytes * 8 / rate_seconds;
      if (measured > 500000 * 1.08 || (entry.first != "playback" && measured < 150000))
        throw std::runtime_error("encrypted UDP pacing did not constrain a saturated stream: " + entry.first + " " + std::to_string(measured));
      std::cout << Json{{"paced_stream", entry.first}, {"limit_bps", 500000}, {"charged_wire_bps", measured}, {"seconds", rate_seconds}}.dump() << '\n';
    }
    const auto paced = receiver.stats();
    if (failed || paced.value("audio_buffers", 0) < 60) throw std::runtime_error("saturated video delayed the independently reserved playback stream: " + fail_reason());
    sender.stop(); receiver.stop(); rate_relays.clear();
    for (auto& stream : offer["streams"]) stream.erase("wire_limit_bps");

    // Even authorizing both endpoints early must not bypass the DTLS pin.
    sender_verified = receiver_verified = failed = false;
    offer["session"] = random_token(12);
    offer["host_fingerprint"] = std::string(64, '0');
    start();
    sender.authorize();
    receiver.authorize();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!failed && std::chrono::steady_clock::now() < until)
      feed(50);
    const auto rejected = receiver.stats();
    sender.stop();
    receiver.stop();
    if (!failed || receiver_verified || rejected.value("frames", 0) != 0 ||
        rejected.value("audio_buffers", 0) != 0)
      throw std::runtime_error(
          "DTLS wrong-identity rejection or media gating failed");

    // Full native panel dimensions exercise RFC4175 row/plane handling at the
    // non-multiple-of-four width 1366. This is a development-host throughput
    // sanity check, not an HP Atom or Wi-Fi performance claim.
    offer["session"] = random_token(12);
    offer["host_fingerprint"] =
        certificate_fingerprint(root / "host/certificate.pem");
    auto &raw = offer["streams"][0]["profile"];
    raw["codec"] = "raw-i420";
    raw["width"] = raw["encoded_width"] = 1366;
    raw["height"] = raw["encoded_height"] = 768;
    raw["fps"] = 60;
    raw["bitrate"] = 0;
    offer["streams"][1]["profile"]["x"] = 1366;
    sender_verified = receiver_verified = failed = false;
    feed_interval_ms = 2;
    start();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!failed && !(sender_verified && receiver_verified) &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (failed || !sender_verified || !receiver_verified)
      throw std::runtime_error("raw media authentication: " + fail_reason());
    feed(100);
    expect_silent();
    sender.authorize();
    feed(100);
    expect_silent();
    receiver.authorize();
    const auto raw_started = std::chrono::steady_clock::now();
    feed(5000);
    const auto raw_elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - raw_started)
                                 .count();
    const auto raw_stats = receiver.stats();
    sender.stop();
    receiver.stop();
    if (failed)
      throw std::runtime_error("mixed raw/MPEG2/Opus failed: " + fail_reason());
    for (const auto &stream : raw_stats.at("streams"))
      if (stream.value("buffers", 0u) < 10)
        throw std::runtime_error("mixed stream produced too few frames: " +
                                 raw_stats.dump());
    if (raw_stats.at("streams")[0].value("codec", "") != "raw-i420")
      throw std::runtime_error("raw profile did not remain selected");
    const auto &raw_result = raw_stats.at("streams")[0];
    // Different SIMD/converter versions round neutral white chroma by one LSB.
    const auto digest = raw_result.value("headless_sha256", "");
    if ((digest != expected_i420(1366, 768, 127) &&
         digest != expected_i420(1366, 768, 128)) ||
        raw_result.value("headless_checked_frames", 0u) < 10 ||
        raw_result.value("headless_changed_frames", 1u) != 0)
      throw std::runtime_error("raw I420 row/plane pixel integrity failed: " +
                               raw_result.dump());
    const auto raw_fps =
        raw_stats.at("streams")[0].at("buffers").get<double>() / raw_elapsed;
    std::cout << "Development loopback raw 1366x768 target60 mixed with MPEG2 "
                 "and Opus, measured "
              << raw_fps << " raw frames/s over " << raw_elapsed
              << " seconds: " << raw_stats.dump() << '\n';

    // Exercise loss across two frames through the real encrypted transport and
    // actual GStreamer depayloader, not only the isolated coverage classifier.
    relay = std::make_unique<LossRelay>(offer["streams"][0]["host_port"],
                                        offer["streams"][0]["client_port"]);
    offer["session"] = random_token(12);
    offer["streams"][0]["profile"]["fps"] = 20;
    sender_verified = receiver_verified = failed = false;
    start();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!failed && !(sender_verified && receiver_verified) &&
           std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (failed || !sender_verified || !receiver_verified)
      throw std::runtime_error("loss relay DTLS authentication failed");
    sender.authorize();
    receiver.authorize();
    feed(500);
    const auto before_loss = receiver.stats().at("streams")[0];
    relay->drop_boundary();
    feed(700);
    const auto after_loss = receiver.stats().at("streams")[0];
    sender.stop();
    receiver.stop();
    if (failed || relay->dropped() != 2 ||
        after_loss.value("incomplete_frames", 0u) <
            before_loss.value("incomplete_frames", 0u) + 2 ||
        after_loss.value("headless_sha256", "") != digest ||
        after_loss.value("headless_changed_frames", 1u) != 0 ||
        after_loss.value("headless_checked_frames", 0u) <=
            before_loss.value("headless_checked_frames", 0u) + 5)
      throw std::runtime_error("two-frame raw packet loss/recovery failed: " +
                               after_loss.dump());
    relay.reset();

    // Raw reassembly has the same identity gate as compressed decoding.
    sender_verified = receiver_verified = failed = false;
    offer["session"] = random_token(12);
    offer["host_fingerprint"] = std::string(64, '0');
    start();
    sender.authorize();
    receiver.authorize();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!failed && std::chrono::steady_clock::now() < until)
      feed(50);
    const auto raw_rejected = receiver.stats();
    sender.stop();
    receiver.stop();
    if (!failed || receiver_verified || raw_rejected.value("frames", 0) ||
        raw_rejected.value("audio_buffers", 0))
      throw std::runtime_error("raw DTLS identity gate failed");
#ifndef _WIN32
    offer["audio_source"] = "pipewire";
    offer["audio_pipewire_fd"] = -1;
    bool rejected_fd = false;
    try {
      sender.start(offer, combined_identity_pem(root / "host"),
                   [](const Json &) {});
    } catch (const std::exception &e) {
      rejected_fd =
          std::string(e.what()).find("audio_pipewire_fd") != std::string::npos;
    }
    sender.stop();
    if (!rejected_fd)
      throw std::runtime_error(
          "PipeWire capture accepted missing user-session authorization");
#endif
    std::filesystem::remove_all(root);
    std::cout << "encrypted MPEG2, mixed raw-I42060 + MPEG2 + Opus, "
                 "authorization gates, "
                 "and wrong-peer rejection passed; "
              << decoded.dump() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
