#include "remoteos/identity.hpp"
#include "remoteos/media.hpp"
#include <asio.hpp>
#include <gst/gst.h>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace remoteos;
using Clock = std::chrono::steady_clock;
using udp = asio::ip::udp;
void require(bool value, const char *text) { if (!value) throw std::runtime_error(text); }
unsigned free_port() {
  asio::io_context io; udp::socket socket(io, {asio::ip::address_v4::loopback(), 0});
  return socket.local_endpoint().port();
}
// An owned loopback relay adds bounded delay and actual packet reordering. All
// delayed datagrams own their storage; cancellation and destruction join the IO
// thread even when a test assertion throws. It never contacts a real peer.
class Relay {
public:
  explicit Relay(unsigned client)
      : socket_(io_, {asio::ip::address_v4::loopback(), 0}), timer_(io_),
        client_(asio::ip::address_v4::loopback(), client),
        port_(socket_.local_endpoint().port()) {
    socket_.set_option(asio::socket_base::receive_buffer_size(1024 * 1024));
    receive(); thread_ = std::thread([this] { io_.run(); });
  }
  ~Relay() {
    asio::post(io_, [this] { socket_.close(); timer_.cancel(); pending_.clear(); });
    thread_.join();
  }
  unsigned port() const { return port_; }
  std::atomic<bool> wrong_ssrc{false}, reordering{false};
  std::atomic<unsigned> packets{0}, forwarded{0}, non_rtp{0}, oversized{0}, errors{0}, reordered{0};
  std::atomic<std::uint64_t> max_timer_lateness_us{0};
private:
  void forward(const std::vector<unsigned char>& bytes) {
    std::error_code error;
    socket_.send_to(asio::buffer(bytes), client_, 0, error);
    if (error) { ++errors; return; }
    ++forwarded;
    if (bytes.size() >= 12) {
      const auto sequence = std::uint16_t((unsigned(bytes[2]) << 8) | bytes[3]);
      if (have_sequence_ && std::int16_t(sequence - last_sequence_) < 0) ++reordered;
      last_sequence_ = sequence; have_sequence_ = true;
    }
  }
  void schedule() {
    if (pending_.empty()) return;
    timer_.expires_at(pending_.begin()->first);
    timer_.async_wait([this](std::error_code error) {
      if (error) return;
      const auto now = Clock::now();
      while (!pending_.empty() && pending_.begin()->first <= now) {
        const auto late = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - pending_.begin()->first).count());
        max_timer_lateness_us = std::max(max_timer_lateness_us.load(), late);
        forward(pending_.begin()->second); pending_.erase(pending_.begin());
      }
      schedule();
    });
  }
  void receive() {
    socket_.async_receive_from(asio::buffer(bytes_), peer_, [this](std::error_code error, std::size_t n) {
      if (error) return;
      const auto index = packets.fetch_add(1);
      if (n < 12 || (bytes_[0] & 0xc0) != 0x80) ++non_rtp;
      if (n > 1200) ++oversized;
      std::vector<unsigned char> packet(bytes_.begin(), bytes_.begin() + n);
      if (wrong_ssrc && n >= 12) packet[8] ^= 0x80;
      if (reordering) {
        // 5/15 ms alternating delays reorder packets without dropping any.
        const auto due = Clock::now() + std::chrono::milliseconds(index % 2 ? 5 : 15);
        const bool earliest = pending_.empty() || due < pending_.begin()->first;
        pending_.emplace(due, std::move(packet));
        if (earliest) schedule();
      } else forward(packet);
      receive();
    });
  }
  asio::io_context io_; udp::socket socket_; asio::steady_timer timer_;
  udp::endpoint client_, peer_;
  bool have_sequence_ = false; unsigned port_; std::uint16_t last_sequence_ = 0;
  std::multimap<Clock::time_point, std::vector<unsigned char>> pending_;
  std::array<unsigned char, 65536> bytes_{}; std::thread thread_;
};

// This instrumentation exists only in the test executable. A GObject emission
// hook finds codecs as they are added to their bins, so no production stall
// option or pipeline-access API is needed. Sleeping on the decoder sink pad
// simulates a scheduling pause while the network continues receiving packets;
// encoder pad timings distinguish encoding CPU work from paced transmission.
class DecoderStall {
public:
  DecoderStall() {
    auto *klass = g_type_class_ref(GST_TYPE_BIN);
    signal_ = g_signal_lookup("element-added", GST_TYPE_BIN);
    hook_ = g_signal_add_emission_hook(signal_, 0, added, this, nullptr);
    g_type_class_unref(klass);
    require(hook_ != 0, "Cannot install test-only decoder instrumentation");
  }
  ~DecoderStall() {
    g_signal_remove_emission_hook(signal_, hook_);
    for (const auto& entry : pads_) {
      gst_pad_remove_probe(entry.first, entry.second); gst_object_unref(entry.first);
    }
  }
  void arm() { require(!pads_.empty(), "The decoder stall probe was not attached"); armed_ = true; }
  std::atomic<unsigned> stalls{0};
  std::atomic<double> elapsed_ms{0};
  std::atomic<double> encoder_ms{0};
  Json encoder_timings() {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    return encoder_timings_;
  }
private:
  static gboolean added(GSignalInvocationHint *, guint count, const GValue *values, gpointer data) {
    auto *self = static_cast<DecoderStall *>(data);
    if (count < 2) return TRUE;
    auto *element = GST_ELEMENT(g_value_get_object(values + 1));
    auto *factory = gst_element_get_factory(element);
    if (!factory) return TRUE;
    const std::string name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (name == "avenc_mpeg2video") {
      ++self->encoder_generation_;
      for (const auto pad_name : {"sink", "src"}) {
        auto *pad = gst_element_get_static_pad(element, pad_name);
        const auto id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
          [](GstPad *pad, GstPadProbeInfo *info, gpointer data) {
            auto *self = static_cast<DecoderStall *>(data);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            if (GST_PAD_DIRECTION(pad) == GST_PAD_SINK) {
              self->encoder_started_ns_ = now;
              auto *encoder = gst_pad_get_parent_element(pad);
              if (auto *clock = gst_element_get_clock(encoder)) {
                const auto time = gst_clock_get_time(clock), base = gst_element_get_base_time(encoder);
                const auto pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
                self->encoder_input_age_ms_ = GST_CLOCK_TIME_IS_VALID(pts) && time >= base + pts ? double(time - base - pts) / GST_MSECOND : 0;
                gst_object_unref(clock);
              }
              gst_object_unref(encoder);
            } else if (self->encoder_started_ns_) {
              const double elapsed = double(now - self->encoder_started_ns_) / 1000000;
              self->encoder_ms = self->encoder_ms * .9 + elapsed * .1;
              std::lock_guard<std::mutex> lock(self->encoder_mutex_);
              if (self->encoder_generation_ > 1 && self->encoder_timings_.size() < 24)
                self->encoder_timings_.push_back({{"encode_ms", elapsed}, {"input_age_ms", self->encoder_input_age_ms_}});
            }
            return GST_PAD_PROBE_OK;
          }, self, nullptr);
        self->pads_.emplace_back(pad, id);
      }
      return TRUE;
    }
    if (name != "avdec_mpeg2video") return TRUE;
    auto *pad = gst_element_get_static_pad(element, "sink");
    const auto id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, stall, self, nullptr);
    self->pads_.emplace_back(pad, id);
    return TRUE;
  }
  static GstPadProbeReturn stall(GstPad *, GstPadProbeInfo *info, gpointer data) {
    auto *self = static_cast<DecoderStall *>(data);
    auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    // Pause an I picture: evicting the following P pictures must exercise
    // reference recovery rather than accidentally meeting the next GOP's I.
    if (buffer && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT) &&
        self->armed_.exchange(false)) {
      const auto start = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      self->elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      ++self->stalls;
    }
    return GST_PAD_PROBE_OK;
  }
  guint signal_ = 0; gulong hook_ = 0;
  std::vector<std::pair<GstPad *, gulong>> pads_;
  std::atomic<bool> armed_{false};
  std::atomic<std::int64_t> encoder_started_ns_{0};
  unsigned encoder_generation_ = 0;
  double encoder_input_age_ms_ = 0;
  std::mutex encoder_mutex_;
  Json encoder_timings_ = Json::array();
};

// Precompute moving, textured native-size pictures. Generating these during the
// timed section would conflate test-pattern CPU work with capture/codec timing.
// Every picture changes the full screen, unlike the old one-pixel toggle.
std::vector<std::vector<std::uint8_t>> motion_frames(unsigned width, unsigned height) {
  std::vector<std::vector<std::uint8_t>> frames(12, std::vector<std::uint8_t>(std::size_t(width) * height * 4));
  for (unsigned phase = 0; phase < frames.size(); ++phase)
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        const auto sx = (x + phase * 7) % width, sy = (y + phase * 3) % height;
        std::uint32_t texture = (sx / 8) * 0x45d9f3bu ^ (sy / 8) * 0x27d4eb2du;
        texture ^= texture >> 13; texture *= 0x85ebca6bu;
        const auto i = (std::size_t(y) * width + x) * 4;
        frames[phase][i] = std::uint8_t((sx + sy) & 255);
        frames[phase][i + 1] = std::uint8_t(((texture >> 8) & 127) + ((sx ^ sy) & 63));
        frames[phase][i + 2] = std::uint8_t(texture >> 24);
        frames[phase][i + 3] = 255;
      }
  return frames;
}

void run_case(const std::filesystem::path& root, const std::string& host_pem,
              const std::string& client_pem, const std::string& transport, int fps,
              bool reorder = false, bool stall_decoder = false) {
  const unsigned width = fps == 30 ? 1024 : 1366, height = fps == 30 ? 576 : 768;
  const auto pictures = motion_frames(width, height);
  const unsigned jitter = reorder || stall_decoder ? 40 : 10;
  // The repair deadline includes fresh-encoder VBV startup work and paced
  // picture serialization. Ordered video still presents immediately.
  const unsigned video_jitter = reorder || stall_decoder ? 150 : jitter;
  Display display{"internal", int(width), int(height), 0, 0, int(width), int(height), fps, 18000000};
  const auto hp = free_port(), cp = free_port(); Relay relay(cp);
  Json offer{{"session", random_token(12)}, {"host", "127.0.0.1"}, {"media_transport", transport}, {"jitter_ms", jitter},
    {"audio_source", "test"},
    {"host_fingerprint", certificate_fingerprint(root / "host/certificate.pem")},
    {"client_fingerprint", certificate_fingerprint(root / "client/certificate.pem")},
    {"streams", Json::array({{{"id", "internal"}, {"kind", "video"}, {"host_port", hp},
      {"client_port", cp}, {"jitter_ms", video_jitter}, {"wire_limit_bps", 32000000}, {"ssrc", 0x13579bdfu}, {"payload", "gst-frame"}, {"profile", display}}})}};
  offer["streams"].push_back({{"id", "playback"}, {"kind", "audio"}, {"host_port", free_port()},
    {"client_port", free_port()}, {"wire_limit_bps", 500000}, {"ssrc", 0x2468ace1u}});
  auto send_offer = offer, receive_offer = offer;
  if (transport == "rtp-udp") {
    send_offer["streams"][0]["client_port"] = relay.port();
    receive_offer["streams"][0]["host_port"] = relay.port();
  }
  // Callback state outlives both media engines, including exception unwinding.
  std::atomic<bool> sv{false}, rv{false};
  std::atomic<unsigned> keyframe_requests{0}, sender_verifications{0}, receiver_verifications{0};
  std::mutex mutex; std::string failure;
  DecoderStall instrumentation;
  MediaSender sender; MediaReceiver receiver;
  auto callback = [&](bool sending, const Json& event) {
    if (event.value("type", "") == "media_verified") {
      (sending ? sv : rv) = true; ++(sending ? sender_verifications : receiver_verifications);
    }
    if (event.value("type", "") == "error") { std::lock_guard<std::mutex> lock(mutex); failure = event.value("message", ""); }
    if (!sending && event.value("type", "") == "keyframe") {
      const auto id = event.value("id", std::string{});
      if (id != "internal") { std::lock_guard<std::mutex> lock(mutex); failure = "Keyframe recovery was not scoped to its video stream"; }
      else { ++keyframe_requests; sender.request_keyframe(id); }
    }
  };
  receiver.start(receive_offer, {0}, client_pem, [callback](const Json& event) { callback(false, event); });
  sender.start(send_offer, host_pem, [callback](const Json& event) { callback(true, event); });
  const auto handshake_deadline = Clock::now() + std::chrono::seconds(5);
  while ((!sv || !rv) && Clock::now() < handshake_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(sv && rv, "Media startup did not complete");
  const auto submit = [&](unsigned frame) {
    const auto& pixels = pictures[frame % pictures.size()];
    sender.frame("internal", width, height, width * 4, pixels.data(), pixels.size());
  };
  submit(0); std::this_thread::sleep_for(std::chrono::milliseconds(80));
  require(receiver.stats().at("frames") == 0, "Media passed the pre-authorization gate");
  receiver.authorize(); sender.authorize();
  const auto first = Clock::now(); submit(0);
  // Exactly ONE picture: no following picture or EOS may be needed to release it.
  const auto first_deadline = first + std::chrono::milliseconds(300);
  Json stats;
  do { std::this_thread::sleep_for(std::chrono::milliseconds(2)); stats = receiver.stats(); }
  while (stats.at("frames") == 0 && Clock::now() < first_deadline);
  require(stats.at("frames").get<unsigned>() == 1, "An isolated MPEG-2 picture waited for the next frame");
  const auto first_ms = std::chrono::duration<double, std::milli>(Clock::now() - first).count();
  unsigned frame = 1;
  const auto feed = [&](std::chrono::milliseconds duration) {
    const auto start = Clock::now(), end = start + duration;
    auto next = start;
    while (Clock::now() < end) {
      submit(frame++);
      next += std::chrono::nanoseconds(1000000000 / fps);
      std::this_thread::sleep_until(next);
    }
  };
  // Let the first long I-picture and pipeline startup settle before rate sampling.
  feed(std::chrono::milliseconds(400));
  // Establish the RTP sequence base before impairing transport. Reordering a
  // session's very first fragment is a separate startup-loss recovery case.
  if (reorder) { relay.reordering = true; feed(std::chrono::milliseconds(200)); }
  receiver.stats(); sender.stats();
  feed(std::chrono::seconds(2));
  stats = receiver.stats();
  const auto stream = stats["streams"][0];
  const auto host_stats = sender.stats();
  require(stream.at("jitter_ms") == video_jitter && stats["streams"][1].at("jitter_ms") == jitter,
          "Independent video repair and audio jitter budgets were not respected");
  require(!stream.at("clock_sync").get<bool>() && !stream.at("clock_qos").get<bool>() &&
          stream.at("decoder_dropped_frames").get<unsigned>() == 0,
          "Immediate video presentation enabled playback-clock decoder dropping");
  require(stream.at("fps").get<double>() >= fps * .85,
          ("Moving textured media fell below its native target cadence: " + stream.dump() + " relay_timer_lateness_us=" + std::to_string(relay.max_timer_lateness_us.load())).c_str());
  require(stream.at("decoded_fps").get<double>() >= fps * .85 &&
          host_stats["streams"][0].at("encoded_fps").get<double>() >= fps * .85,
          "Capture/encode/decode stage rates were not measured from real pictures");
  require(host_stats["streams"][0].at("rtp_mbps").get<double>() >= 5.0 &&
          stream.at("receive_mbps").get<double>() >= 5.0,
          "Motion fixture did not exercise a substantial encoded/network workload");
  require(stream.at("socket_receive_buffer_bytes").get<unsigned>() >= 128 * 1024,
          "Video socket did not expose its configured bounded receive-buffer capacity");
  require(stream.at("playout_latency_ms").get<double>() <= 60, "Frame-preserving media retained whole-picture lookahead");
  require(stats.at("audio_buffers").get<unsigned>() >= 50, "Independent Opus playback stalled during moving video");
  require(stream.at("packets_lost").get<unsigned>() == 0 && stream.at("packets_late").get<unsigned>() == 0,
          ("Bounded zero-loss relay motion was incorrectly treated as network loss: " + stream.dump() + " relay_timer_lateness_us=" + std::to_string(relay.max_timer_lateness_us.load())).c_str());
  Json stall_stats, live_profile_stats;
  if (stall_decoder) {
    const auto before = receiver.stats();
    // 120 ms spans seven 60 Hz pictures and guarantees more than one queue
    // eviction. The public count is deliberately a conservative lower bound
    // that excludes one in-flight buffer; an 80 ms stall may drop only one.
    instrumentation.arm(); sender.request_keyframe("internal"); feed(std::chrono::milliseconds(700));
    stall_stats = receiver.stats();
    require(instrumentation.stalls == 1 && instrumentation.elapsed_ms >= 115,
            "The deterministic 120 ms decoder scheduling stall did not execute");
    require(stall_stats["streams"][0].at("compressed_queue_drops").get<unsigned>() >
            before["streams"][0].at("compressed_queue_drops").get<unsigned>(),
            ("The decoder stall did not exercise the bounded complete-picture queue: " + stall_stats.dump()).c_str());
    require(stall_stats["streams"][0].at("packets_lost").get<unsigned>() == 0 &&
            stall_stats["streams"][0].at("packets_late").get<unsigned>() == 0,
            ("A decoder scheduling stall backed up the packet receiver: " + stall_stats.dump()).c_str());
    require(keyframe_requests > 0, "Complete-picture loss did not request scoped reference recovery");
    require(stall_stats.at("audio_buffers").get<unsigned>() > before.at("audio_buffers").get<unsigned>() + 20,
            "A video decoder stall blocked independent audio");
    receiver.stats(); sender.stats(); feed(std::chrono::seconds(1));
    const auto recovered = receiver.stats();
    require(recovered["streams"][0].at("fps").get<double>() >= fps * .85,
            ("Video did not recover its cadence after the decoder scheduling stall: " + recovered.dump()).c_str());
    stall_stats["recovered"] = recovered;
    const auto before_profile = receiver.stats();
    auto changed = display; changed.fps = 30; changed.bitrate = 12000000;
    require(receiver.update_profiles({changed}) && sender.reconfigure({changed}),
            "Public media API could not apply a live native-size FPS/bitrate change");
    // The source still submits at 60 Hz: the sender must enforce the new 30 Hz
    // profile without rebuilding the session, audio stream or RTP identity.
    feed(std::chrono::milliseconds(400)); receiver.stats(); sender.stats();
    feed(std::chrono::seconds(1));
    live_profile_stats = receiver.stats();
    const auto changed_host = sender.stats();
    const auto changed_fps = live_profile_stats["streams"][0].at("fps").get<double>();
    const auto changed_encoded_fps = changed_host["streams"][0].at("encoded_fps").get<double>();
    require(live_profile_stats["streams"][0].at("target_fps") == 30 &&
            changed_fps >= 25.5 && changed_fps <= 34.5 &&
            changed_encoded_fps >= 25.5 && changed_encoded_fps <= 34.5,
            ("Live 60-to-30 FPS profile did not reach the encoder and receiver: " +
              live_profile_stats.dump() + " host=" + changed_host.dump() +
              " encode_ms=" + std::to_string(instrumentation.encoder_ms.load())).c_str());
    require(live_profile_stats.at("audio_buffers").get<unsigned>() >=
            before_profile.at("audio_buffers").get<unsigned>() + 50,
            "Live video adaptation interrupted or reset the independent audio stream");
    require(sender_verifications == 1 && receiver_verifications == 1,
            "Live video adaptation restarted media authentication");
    require(live_profile_stats["streams"][0].at("packets_lost").get<unsigned>() == 0 &&
            live_profile_stats["streams"][0].at("packets_late").get<unsigned>() == 0,
            ("Live video adaptation caused false packet loss under bounded reordering: " +
             live_profile_stats.dump() + " relay_timer_lateness_us=" +
             std::to_string(relay.max_timer_lateness_us.load()) +
             " encoder_transition=" + instrumentation.encoder_timings().dump()).c_str());
    live_profile_stats["host_stats"] = changed_host;
  }
  if (transport == "rtp-udp") {
    require(relay.packets > 1 && relay.non_rtp == 0 && relay.oversized == 0 && relay.errors == 0,
            "Plain RTP used crypto framing, oversized datagrams or failed relay sends");
    if (reorder) require(relay.reordered > 100, "Relay did not actually reorder the motion stream");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto before = receiver.stats().at("frames").get<unsigned>();
    relay.wrong_ssrc = true; sender.request_keyframe("internal"); submit(frame++);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(receiver.stats().at("frames").get<unsigned>() == before, "A foreign-session SSRC reached the decoder");
  }
  { std::lock_guard<std::mutex> lock(mutex); require(failure.empty(), failure.c_str()); }
  std::cout << Json{{"transport", transport}, {"payload", "gst-frame"}, {"fixture", "precomputed-moving-texture"},
    {"width", width}, {"height", height}, {"fps_target", fps}, {"relay_reorder", reorder},
    {"relay_reordered_packets", relay.reordered.load()}, {"relay_max_timer_lateness_us", relay.max_timer_lateness_us.load()}, {"decoder_stall_ms", instrumentation.elapsed_ms.load()},
    {"host_encode_ms", instrumentation.encoder_ms.load()},
    {"encoder_transition_timings", instrumentation.encoder_timings()},
    {"keyframe_requests", keyframe_requests.load()}, {"first_single_picture_ms", first_ms},
    {"host_stats", host_stats}, {"stats", stats}, {"stall_stats", stall_stats},
    {"live_profile_stats", live_profile_stats}}.dump() << '\n';
  sender.stop(); receiver.stop();
}

int main(int argc, char **argv) {
  gst_init(nullptr, nullptr);
  const auto root = std::filesystem::temp_directory_path() / ("remoteos-interactive-" + random_token(8));
  try {
    require(argc == 1 || (argc == 2 && std::string(argv[1]) == "--stress-only"),
            "Only --stress-only is accepted by this media regression test");
    ensure_identity((root / "host").string(), "Interactive test host");
    ensure_identity((root / "client").string(), "Interactive test client");
    const auto host_pem = combined_identity_pem((root / "host").string());
    const auto client_pem = combined_identity_pem((root / "client").string());
    if (argc < 2)
    for (const auto transport : {"rtp-udp", "dtls-srtp"})
      for (const auto fps : {30, 60}) run_case(root, host_pem, client_pem, transport, fps);
    run_case(root, host_pem, client_pem, "rtp-udp", 60, true, true);
    std::filesystem::remove_all(root);
    std::cout << "Plain/encrypted moving-texture UDP, isolated first-picture presentation, native 30/60 cadence, bounded packet reordering, decoder-stall recovery, live FPS/bitrate adaptation, authorization and stale-session rejection passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(root); std::cerr << error.what() << '\n'; return 1;
  }
}
