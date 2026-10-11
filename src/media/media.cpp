#include "remoteos/media.hpp"
#include "packet_pacer.hpp"
#include "raw_frame.hpp"
#include "mpeg2_freshness.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef REMOTEOS_WITH_MEDIA
#include "window_refresh.hpp"
#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gst/net/gstnetaddressmeta.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video-event.h>
#include <gst/video/video-frame.h>
#include <gst/video/videooverlay.h>
#include "encoder_reconfigure.hpp"
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

namespace remoteos {
#ifdef REMOTEOS_WITH_MEDIA
namespace {
void init_gst() {
  static std::once_flag flag;
  std::call_once(flag, [] { gst_init(nullptr, nullptr); });
}
GstElement *element(GstElement *p, const char *name, const char *id = nullptr) {
  auto *e = gst_element_factory_make(name, id);
  if (!e)
    throw std::runtime_error(std::string("missing GStreamer element: ") + name);
  gst_bin_add(GST_BIN(p), e);
  return e;
}
void link(GstElement *a, GstElement *b) {
  if (!gst_element_link(a, b))
    throw std::runtime_error("GStreamer element link failed");
}
void padlink(GstElement *a, const char *ap, GstElement *b, const char *bp,
             bool req_a = false, bool req_b = false) {
  auto *x = req_a ? gst_element_request_pad_simple(a, ap)
                  : gst_element_get_static_pad(a, ap);
  auto *y = req_b ? gst_element_request_pad_simple(b, bp)
                  : gst_element_get_static_pad(b, bp);
  if (!x || !y) {
    if (x)
      gst_object_unref(x);
    if (y)
      gst_object_unref(y);
    throw std::runtime_error("media pad unavailable");
  }
  auto status = gst_pad_link(x, y);
  gst_object_unref(x);
  gst_object_unref(y);
  if (status != GST_PAD_LINK_OK)
    throw std::runtime_error("media pad link failed");
}
std::string pem_fingerprint(const char *pem) {
  BIO *b = BIO_new_mem_buf(pem, -1);
  X509 *cert = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
  BIO_free(b);
  if (!cert)
    throw std::runtime_error("invalid DTLS peer certificate");
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned n = 0;
  int ok = X509_digest(cert, EVP_sha256(), md, &n);
  X509_free(cert);
  if (!ok)
    throw std::runtime_error("DTLS digest failure");
  const char *h = "0123456789abcdef";
  std::string out;
  for (unsigned i = 0; i < n; ++i) {
    out += h[md[i] >> 4];
    out += h[md[i] & 15];
  }
  return out;
}
struct Pipe {
  GstElement *pipeline = nullptr, *enc = nullptr, *dec = nullptr,
             *gate = nullptr, *source = nullptr, *encoder = nullptr,
             *sink = nullptr, *udpout = nullptr, *queue = nullptr, *decoder = nullptr,
             *compressed_queue = nullptr, *raw_filter = nullptr, *pictures = nullptr;
  GSocket *socket = nullptr;
  GInetAddress *peer_address = nullptr;
  GstBufferPool *frame_pool = nullptr;
  std::string id, expected, kind, expected_address, codec;
  std::atomic<unsigned> peer_port{0};
  std::mutex jitter_mutex;
  std::vector<GstElement *> jitters;
  Display display;
  int source_width = 0, source_height = 0;
  std::atomic<bool> verified{false}, recovery_requested{false};
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> decoded_frames{0}, encoded_frames{0}, captured_frames{0}, submitted_frames{0}, wire_bytes{0};
  std::uint64_t queue_replaced = 0, compressed_replaced = 0;
  std::atomic<std::uint64_t> received_bytes{0}, compressed_received{0}, compressed_passed{0};
  std::atomic<bool> reference_lost{false};
  std::chrono::steady_clock::time_point last_recovery_request{};
  std::chrono::steady_clock::time_point last_recovery_discard{};
  std::atomic<std::uint64_t> decoder_dropped{0};
  std::atomic<std::uint64_t> recovery_discarded{0};
  std::atomic<double> decode_ms{0}, timeline_delay_ms{0};
  std::int64_t decode_started_ns = 0;
  media_detail::Mpeg2Freshness freshness;
  std::atomic<std::int64_t> lateness_ns{0};
  std::atomic<double> qos_proportion{1.0};
  std::unique_ptr<media_detail::RawFrameCoverage> raw_coverage;
  std::atomic<bool> raw_sync_failed{false};
  std::atomic<std::uint64_t> incomplete_frames{0};
  std::mutex digest_mutex;
  std::string headless_digest;
  std::uint64_t checked_frames = 0, changed_frames = 0;
  bool sender = false, plaintext = false;
  guint32 ssrc = 0;
  unsigned jitter_ms = 40;
  std::string payload = "mpv";
  std::atomic<bool> stopping{false};
  std::unique_ptr<media_detail::PacketPacer> pacer;
  std::uint64_t wire_ceiling_bps = 0;
  std::chrono::steady_clock::time_point next_frame{};
  ~Pipe() {
    stopping = true;
    if (pipeline) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
    for (auto *jitter : jitters)
      gst_object_unref(jitter);
    if (frame_pool) {
      gst_buffer_pool_set_active(frame_pool, FALSE);
      gst_object_unref(frame_pool);
    }
    if (socket)
      g_object_unref(socket);
    if (peer_address) g_object_unref(peer_address);
  }
};
GstFlowReturn send_packet(GstBuffer *buffer, Pipe *pipe) {
  if (pipe->stopping) return GST_FLOW_FLUSHING;
  const auto deadline = pipe->pacer->reserve(gst_buffer_get_size(buffer), std::chrono::steady_clock::now());
  while (!pipe->stopping && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));
  if (pipe->stopping) return GST_FLOW_FLUSHING;
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_FLOW_ERROR;
  auto *destination = g_inet_socket_address_new(pipe->peer_address, guint16(pipe->peer_port.load()));
  GError *error = nullptr;
  const auto sent = g_socket_send_to(pipe->socket, destination, reinterpret_cast<const char *>(map.data), map.size, nullptr, &error);
  if (sent > 0) pipe->wire_bytes.fetch_add(std::uint64_t(sent));
  g_object_unref(destination); gst_buffer_unmap(buffer, &map); g_clear_error(&error);
  return sent < 0 ? GST_FLOW_ERROR : GST_FLOW_OK;
}
GstFlowReturn paced_sample(GstAppSink *sink, gpointer data) {
  auto *pipe = static_cast<Pipe *>(data);
  auto *sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_FLUSHING;
  auto result = GST_FLOW_OK;
  // Payloader buffer lists still represent separate datagrams. Pace each
  // packet rather than sleeping once and emitting a whole I-frame burst.
  if (auto *list = gst_sample_get_buffer_list(sample)) {
    for (guint i = 0; i < gst_buffer_list_length(list) && result == GST_FLOW_OK; ++i)
      result = send_packet(gst_buffer_list_get(list, i), pipe);
  } else if (auto *buffer = gst_sample_get_buffer(sample)) {
    result = send_packet(buffer, pipe);
  }
  gst_sample_unref(sample); return result;
}
void remember_jitter(GstElement *, GstElement *jitter, guint, guint,
                     gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  g_object_set(jitter, "drop-on-latency", p->kind == "audio", nullptr);
  if (p->kind == "video" || p->jitter_ms < 40)
    g_object_set(jitter, "faststart-min-packets", 1u, nullptr);
  std::lock_guard<std::mutex> lock(p->jitter_mutex);
  p->jitters.push_back(GST_ELEMENT(gst_object_ref(jitter)));
}
GstPadProbeReturn source_allocation_query(GstPad *, GstPadProbeInfo *info,
                                         gpointer) {
  if (GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) == GST_QUERY_ALLOCATION)
    // Capture and RTP datagrams use their sources' own pools. Forwarding this
    // serialized query into rtpjitterbuffer can block the UDP receive thread
    // behind a missing packet, so that packet cannot arrive before its loss
    // deadline. A capture queue can likewise retain the caller-owned query
    // during encoder replacement. Keep decoded-frame allocation downstream.
    return GST_PAD_PROBE_HANDLED;
  return GST_PAD_PROBE_OK;
}
GstElement *audio_capture(GstElement *pipeline, const Json &offer) {
  const auto source = offer.value("audio_source", std::string("none"));
  if (source == "test") {
    auto *result = element(pipeline, "audiotestsrc");
    g_object_set(result, "is-live", TRUE, "samplesperbuffer", 960, nullptr);
    return result;
  }
#ifdef _WIN32
  if (source == "wasapi") {
    const char *factory = "wasapi2src";
    auto *probe = gst_element_factory_find(factory);
    if (probe)
      gst_object_unref(probe);
    else
      factory = "wasapisrc";
    auto *result = element(pipeline, factory);
    g_object_set(result, "loopback", TRUE, "low-latency", TRUE, nullptr);
    return result;
  }
#else
  if (source == "pipewire" || source == "pipewire-user") {
    // A privileged host must receive an already-connected PipeWire descriptor
    // from its authorized user-session integration. Never guess another user's
    // runtime directory or silently capture a microphone instead of playback.
    const int fd = offer.value("audio_pipewire_fd", -1);
    int type = 0;
    socklen_t length = sizeof(type);
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    if (source == "pipewire-user" && geteuid() == 0)
      throw std::runtime_error("User-session playback capture must run as the logged-in user");
    if (source == "pipewire" && (fd < 0 || fcntl(fd, F_GETFD) < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) < 0 ||
        type != SOCK_STREAM ||
        getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peer_length) < 0))
      throw std::runtime_error(
          "PipeWire playback capture requires an inherited connected "
          "user-session socket (audio_pipewire_fd)");
    auto *result = element(pipeline, "pipewiresrc");
    auto *props =
        gst_structure_new("props", "stream.capture.sink", G_TYPE_BOOLEAN, TRUE,
                          "media.type", G_TYPE_STRING, "Audio",
                          "media.category", G_TYPE_STRING, "Capture", nullptr);
    if (source == "pipewire") g_object_set(result, "fd", fd, nullptr);
    g_object_set(result, "stream-properties", props, "do-timestamp", TRUE, nullptr);
    gst_structure_free(props);
    const auto target = offer.value("audio_target", std::string{});
    if (!target.empty())
      g_object_set(result, "target-object", target.c_str(), nullptr);
    return result;
  }
#endif
  throw std::runtime_error("playback capture source is unavailable: " + source);
}
void dynamic_rtp(GstElement *, GstPad *pad, gpointer data) {
  if (!g_str_has_prefix(GST_PAD_NAME(pad), "recv_rtp_src_0_"))
    return;
  auto *sink =
      gst_element_get_static_pad(static_cast<GstElement *>(data), "sink");
  if (!gst_pad_is_linked(sink))
    gst_pad_link(pad, sink);
  gst_object_unref(sink);
}
GstPadProbeReturn packet_lost(GstPad *, GstPadProbeInfo *info, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  auto *event = GST_PAD_PROBE_INFO_EVENT(info);
  const auto *structure = event ? gst_event_get_structure(event) : nullptr;
  if (p->kind == "video" && structure &&
      gst_structure_has_name(structure, "GstRTPPacketLost")) {
    if (p->codec == "mpeg2") {
      p->reference_lost = true;
      p->recovery_requested = true;
    }
  }
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn raw_packet(GstPad *, GstPadProbeInfo *info, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), GST_MAP_READ, &rtp))
    return GST_PAD_PROBE_DROP;
  p->raw_coverage->packet(
      gst_rtp_buffer_get_timestamp(&rtp), gst_rtp_buffer_get_marker(&rtp),
      static_cast<const std::uint8_t *>(gst_rtp_buffer_get_payload(&rtp)),
      gst_rtp_buffer_get_payload_len(&rtp));
  if (p->raw_coverage->failed())
    p->raw_sync_failed = true;
  gst_rtp_buffer_unmap(&rtp);
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn raw_complete_frame(GstPad *, GstPadProbeInfo *info,
                                     gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  // RFC4175 reassembly may flush a partially received frame at the next RTP
  // timestamp. Match each output with the coverage verdict for that frame;
  // one loss flag cannot distinguish consecutive partially received frames.
  if (!p->raw_coverage->output() ||
      (buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED))) {
    if (p->raw_coverage->failed())
      p->raw_sync_failed = true;
    ++p->incomplete_frames;
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}
GstFlowReturn headless_sample(GstAppSink *sink, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  auto *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_EOS;
  GstVideoInfo info{};
  GstVideoFrame frame{};
  auto *buffer = gst_sample_get_buffer(sample);
  const bool mapped =
      gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I420 &&
      gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ);
  if (mapped) {
    auto *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    // Hash active pixels only. GStreamer I420 row padding differs from the
    // packed RFC4175 pixel groups when width (e.g.1366) is not divisible by4.
    for (unsigned plane = 0; plane != 3; ++plane) {
      const auto *base = static_cast<const guint8 *>(
          GST_VIDEO_FRAME_PLANE_DATA(&frame, plane));
      const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
      const int width = GST_VIDEO_FRAME_COMP_WIDTH(&frame, plane);
      const int height = GST_VIDEO_FRAME_COMP_HEIGHT(&frame, plane);
      for (int y = 0; y < height; ++y)
        g_checksum_update(checksum, base + y * stride, width);
    }
    {
      std::lock_guard<std::mutex> lock(p->digest_mutex);
      const std::string digest = g_checksum_get_string(checksum);
      if (p->headless_digest.empty())
        p->headless_digest = digest;
      else if (p->headless_digest != digest)
        ++p->changed_frames;
      ++p->checked_frames;
    }
    g_checksum_free(checksum);
    gst_video_frame_unmap(&frame);
  }
  gst_sample_unref(sample);
  return mapped ? GST_FLOW_OK : GST_FLOW_ERROR;
}
GstPadProbeReturn count_frame(GstPad *, GstPadProbeInfo *info, gpointer data) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
    static_cast<Pipe *>(data)->frames.fetch_add(1);
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn compressed_picture(GstPad *, GstPadProbeInfo *, gpointer data) {
  ++static_cast<Pipe *>(data)->compressed_received;
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn received_packet(GstPad *, GstPadProbeInfo *info, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  if (auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info)) p->received_bytes += gst_buffer_get_size(buffer);
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn decoder_input(GstPad *, GstPadProbeInfo *info, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  ++p->compressed_passed;
  p->decode_started_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  double age = 0;
  if (auto *clock = gst_element_get_clock(p->pipeline)) {
    const auto now = gst_clock_get_time(clock), base = gst_element_get_base_time(p->pipeline), pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts) && now >= base && now - base > pts) age = double(now - base - pts) / GST_MSECOND;
    gst_object_unref(clock);
  }
  p->timeline_delay_ms = age;
  // Allow packet reordering and a complete I-picture to arrive. The old
  // 80 ms cutoff treated picture assembly on WLAN as decoder overload.
  // Whole-picture queue bounds below keep this from becoming an unbounded
  // playout delay; stale reference chains are still discarded before decode.
  const bool stale = age > std::max(150u, p->jitter_ms + 100u);
  bool accepted = p->freshness.accept(nullptr, 0, stale,
      p->reference_lost.exchange(false) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT));
  if (!accepted && !stale) {
    // Avoid merging/copying the complete compressed picture on this Atom.
    // The host's sequence/matrix/picture headers fit within this small prefix.
    std::uint8_t header[2048];
    const auto size = gst_buffer_extract(buffer, 0, header, sizeof(header));
    accepted = p->freshness.accept(header, size, false, false);
  }
  if (accepted) {
    p->last_recovery_discard = {};
    return GST_PAD_PROBE_OK;
  }
  ++p->recovery_discarded;
  // Request once on entering recovery, then retry at most once per second.
  // Each dependent P picture is not a new packet-loss incident.
  const auto now = std::chrono::steady_clock::now();
  if (p->last_recovery_discard == std::chrono::steady_clock::time_point{} ||
      now - p->last_recovery_discard >= std::chrono::seconds(1)) {
    p->recovery_requested = true;
    p->last_recovery_discard = now;
  }
  return GST_PAD_PROBE_DROP;
}
GstPadProbeReturn decoder_output(GstPad *, GstPadProbeInfo *, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  ++p->decoded_frames;
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (p->decode_started_ns) p->decode_ms = p->decode_ms.load() * .9 + double(now - p->decode_started_ns) / 1000000.0 * .1;
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn encoded_frame(GstPad *, GstPadProbeInfo *, gpointer data) {
  ++static_cast<Pipe *>(data)->encoded_frames;
  return GST_PAD_PROBE_OK;
}
std::string canonical_address(GInetAddress *address) {
  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6) {
    const auto *bytes = g_inet_address_to_bytes(address);
    static const guint8 prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(bytes, prefix, sizeof(prefix)) == 0) {
      auto *v4 =
          g_inet_address_new_from_bytes(bytes + 12, G_SOCKET_FAMILY_IPV4);
      auto *text = g_inet_address_to_string(v4);
      std::string result(text);
      g_free(text);
      g_object_unref(v4);
      return result;
    }
  }
  auto *text = g_inet_address_to_string(address);
  std::string result(text);
  g_free(text);
  return result;
}
GstPadProbeReturn nat_peer(GstPad *, GstPadProbeInfo *info, gpointer data) {
  auto *p = static_cast<Pipe *>(data);
  auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer)
    return GST_PAD_PROBE_OK;
  auto *meta = gst_buffer_get_net_address_meta(buffer);
  if (!meta || !G_IS_INET_SOCKET_ADDRESS(meta->addr))
    return GST_PAD_PROBE_DROP;
  auto *endpoint = G_INET_SOCKET_ADDRESS(meta->addr);
  auto *address = g_inet_socket_address_get_address(endpoint);
  // Only the address authenticated by the control session may participate in
  // DTLS or port learning. Normalize mapped IPv4 addresses before comparison.
  if (g_inet_address_get_family(address) == g_inet_address_get_family(p->peer_address)
          ? !g_inet_address_equal(address, p->peer_address)
          : canonical_address(address) != p->expected_address)
    return GST_PAD_PROBE_DROP;
  const unsigned port = g_inet_socket_address_get_port(endpoint);
  if (p->plaintext) {
    if (port != p->peer_port) return GST_PAD_PROBE_DROP;
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) return GST_PAD_PROBE_DROP;
    const auto ssrc = gst_rtp_buffer_get_ssrc(&rtp);
    gst_rtp_buffer_unmap(&rtp);
    return ssrc == p->ssrc ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
  }
  if (p->verified)
    return port == p->peer_port ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
  if (p->sender) {
    p->peer_port = port;
    // Keep the configured, authenticated IP. NAT discovery changes only port.
    if (!p->pacer) g_object_set(p->udpout, "port", int(port), nullptr);
  } else if (port != p->peer_port)
    return GST_PAD_PROBE_DROP;
  return GST_PAD_PROBE_OK;
}
std::unique_ptr<Pipe> build_pipe(const Json &offer, const Json &stream,
                                 bool sender, std::uintptr_t window,
                                 const std::string &pem, bool desktop_output = false) {
  auto p = std::make_unique<Pipe>();
  p->sender = sender;
  const auto transport = offer.value("media_transport", std::string("dtls-srtp"));
  if (transport != "dtls-srtp" && transport != "rtp-udp")
    throw std::runtime_error("unsupported media transport");
  p->plaintext = transport == "rtp-udp";
  p->jitter_ms = stream.value("jitter_ms", offer.value("jitter_ms", 40u));
  if (p->jitter_ms < 5 || p->jitter_ms > 200)
    throw std::runtime_error("media jitter budget must be 5 to 200 ms");
  if (p->plaintext) {
    p->ssrc = stream.at("ssrc").get<std::uint32_t>();
    if (!p->ssrc) throw std::runtime_error("plain RTP needs a session SSRC");
    // This mode is explicitly negotiated over the paired control channel.
    // Address, port and SSRC isolate sessions; they do not encrypt/authenticate
    // LAN packets. Tailscale still supplies its own WireGuard protection.
    p->verified = true;
  }
  p->id = stream.at("id");
  p->kind = stream.at("kind").get<std::string>();
  const bool audio = p->kind == "audio";
  if (!audio) {
    p->display = stream.at("profile").get<Display>();
    p->source_width = sender ? stream.value("source_width", p->display.width) : p->display.width;
    p->source_height = sender ? stream.value("source_height", p->display.height) : p->display.height;
    if (p->source_width < 320 || p->source_width > 3840 || p->source_height < 200 || p->source_height > 2160)
      throw std::runtime_error("unsupported source monitor dimensions");
    p->codec = stream.at("profile").value("codec", std::string("mpeg2"));
    if (p->codec != "mpeg2" && p->codec != "raw-i420")
      throw std::runtime_error("unsupported video codec: " + p->codec);
  }
  const bool raw = !audio && p->codec == "raw-i420";
  p->payload = stream.value("payload", std::string("mpv"));
  if (!audio && !raw && p->payload != "mpv" && p->payload != "gst-frame")
    throw std::runtime_error("unsupported MPEG-2 RTP payload");
  const bool framed = !audio && !raw && p->payload == "gst-frame";
  p->expected = normalize_fingerprint(
      offer.at(sender ? "client_fingerprint" : "host_fingerprint"));
  p->pipeline = gst_pipeline_new(nullptr);
  if (!p->plaintext) {
  p->dec = element(p->pipeline, "dtlssrtpdec");
  p->enc = element(p->pipeline, "dtlssrtpenc");
  std::string conn = offer.at("session").get<std::string>() + "-" + p->id +
                     (sender ? "-s" : "-r");
  g_object_set(p->dec, "connection-id", conn.c_str(), "pem", pem.c_str(),
               nullptr);
  g_object_set(p->enc, "connection-id", conn.c_str(), "is-client", !sender,
               nullptr);
  }
  int local = stream.at(sender ? "host_port" : "client_port"),
      remote = stream.at(sender ? "client_port" : "host_port");
  if (local < 1024 || local > 65535 || remote < 1024 || remote > 65535)
    throw std::runtime_error("invalid media port");
  auto host = offer.at("host").get<std::string>();
  GInetAddress *address = g_inet_address_new_from_string(host.c_str());
  if (!address)
    throw std::runtime_error("media peer must be a numeric address");
  p->expected_address = canonical_address(address);
  p->peer_port = unsigned(remote);
  auto family = g_inet_address_get_family(address);
  p->peer_address = G_INET_ADDRESS(g_object_ref(address));
  g_object_unref(address);
  GError *error = nullptr;
  p->socket = g_socket_new(family, G_SOCKET_TYPE_DATAGRAM,
                           G_SOCKET_PROTOCOL_UDP, &error);
  if (!p->socket) {
    std::string why = error->message;
    g_error_free(error);
    throw std::runtime_error(why);
  }
  auto *any = g_inet_address_new_any(family);
  auto *bound = g_inet_socket_address_new(any, guint16(local));
  g_object_unref(any);
  if (!g_socket_bind(p->socket, bound, FALSE, &error)) {
    std::string why = error->message;
    g_error_free(error);
    g_object_unref(bound);
    throw std::runtime_error("media bind: " + why);
  }
  g_object_unref(bound);
  auto *udpin = p->plaintext && sender ? nullptr : element(p->pipeline, "udpsrc");
  const bool paced = sender && stream.contains("wire_limit_bps");
  p->udpout = element(p->pipeline, paced ? "appsink" : "udpsink");
  if (udpin) g_object_set(udpin, "socket", p->socket, "close-socket", FALSE, nullptr);
  if (udpin)
    // A native 1366x768 I420 frame occupies ~1.5 MiB. Permit a short bounded
    // burst without overflowing the ordinary ~200 KiB UDP receive buffer.
    // The OS may clamp this request; deployment must qualify its actual limit.
    // MPEG-2 also needs room for a short I-picture burst while the single
    // Atom core is scheduled elsewhere. This is a socket burst allowance,
    // not an additional decoded-frame queue.
    g_object_set(udpin, "buffer-size", raw ? 4 * 1024 * 1024 : 512 * 1024, nullptr);
  if (!paced) g_object_set(p->udpout, "socket", p->socket, "close-socket", FALSE, "host",
               host.c_str(), "port", remote, "sync", FALSE, "async", FALSE, nullptr);
  if (!p->plaintext) { link(udpin, p->dec); link(p->enc, p->udpout); }
  if (paced) {
    const auto limit = stream.at("wire_limit_bps").get<std::uint64_t>();
    if (limit < 64000 || limit > 1000000000) throw std::runtime_error("Invalid stream bandwidth allocation");
    p->wire_ceiling_bps = stream.value("wire_ceiling_bps", limit);
    if (p->wire_ceiling_bps < limit || p->wire_ceiling_bps > 1000000000)
      throw std::runtime_error("Invalid stream bandwidth ceiling");
    p->pacer = std::make_unique<media_detail::PacketPacer>(limit);
    g_object_set(p->udpout, "sync", FALSE, "async", FALSE, "max-buffers", 1u, "drop", FALSE, "buffer-list", TRUE, "enable-last-sample", FALSE, nullptr);
    GstAppSinkCallbacks callbacks{}; callbacks.new_sample = paced_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(p->udpout), &callbacks, p.get(), nullptr);
  }
  if (udpin) {
    auto *pad = gst_element_get_static_pad(udpin, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, nat_peer, p.get(),
                      nullptr);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, received_packet, p.get(), nullptr);
    gst_object_unref(pad);
  }
  auto *rtp = p->plaintext ? nullptr : element(p->pipeline, "rtpbin");
  if (rtp) {
  g_object_set(rtp, "latency", p->jitter_ms, "drop-on-latency", audio, "do-lost", TRUE,
               nullptr);
  g_signal_connect(rtp, "new-jitterbuffer", G_CALLBACK(remember_jitter),
                   p.get());
  padlink(p->dec, "rtcp_src", rtp, "recv_rtcp_sink_0", false, true);
  padlink(rtp, "send_rtcp_src_0", p->enc, "rtcp_sink_0", true, true);
  }
  p->gate = element(p->pipeline, "valve");
  g_object_set(p->gate, "drop", TRUE, "drop-mode", 2, nullptr);
  auto caps_profile = p->display;
  if (sender && audio) {
    p->source = audio_capture(p->pipeline, offer);
    p->queue = element(p->pipeline, "queue");
    auto *convert = element(p->pipeline, "audioconvert");
    auto *resample = element(p->pipeline, "audioresample");
    auto *filter = element(p->pipeline, "capsfilter");
    p->encoder = element(p->pipeline, "opusenc");
    auto *pay = element(p->pipeline, "rtpopuspay");
    auto *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING,
                                     "S16LE", "layout", G_TYPE_STRING,
                                     "interleaved", "rate", G_TYPE_INT, 48000,
                                     "channels", G_TYPE_INT, 2, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(p->queue, "max-size-buffers", 0u, "max-size-bytes", 0u,
                 "max-size-time", guint64(100 * GST_MSECOND), "leaky", 2,
                 nullptr);
    g_object_set(p->encoder, "bitrate", 96000, "frame-size", 20, "inband-fec",
                 TRUE, "packet-loss-percentage", 5, nullptr);
    g_object_set(pay, "pt", 96u, "mtu", 1200u, nullptr);
    if (p->plaintext) g_object_set(pay, "ssrc", p->ssrc, nullptr);
    link(p->source, p->queue);
    link(p->queue, convert);
    link(convert, resample);
    link(resample, filter);
    link(filter, p->encoder);
    link(p->encoder, pay);
    link(pay, p->gate);
    if (p->plaintext) link(p->gate, p->udpout);
    else {
    padlink(p->gate, "src", rtp, "send_rtp_sink_0", false, true);
    padlink(rtp, "send_rtp_src_0", p->enc, "rtp_sink_0", false, true);
    auto *drain = element(p->pipeline, "fakesink");
    g_object_set(drain, "sync", FALSE, "async", FALSE, nullptr);
    padlink(p->dec, "rtp_src", drain, "sink");
    }
  } else if (sender) {
    p->source = element(p->pipeline, "appsrc");
    auto *queue = element(p->pipeline, "queue");
    p->queue = queue;
    auto *convert = element(p->pipeline, "videoconvert");
    auto *scale = element(p->pipeline, "videoscale");
    auto *filter = element(p->pipeline, "capsfilter");
    p->raw_filter = filter;
    auto *pay = element(p->pipeline, raw ? "rtpvrawpay" : framed ? "rtpgstpay" : "rtpmpvpay");
    auto *caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT,
        p->source_width, "height", G_TYPE_INT, p->source_height,
        "framerate", GST_TYPE_FRACTION, caps_profile.fps, 1, nullptr);
    g_object_set(p->source, "caps", caps, "is-live", TRUE, "format",
                 GST_FORMAT_TIME, "do-timestamp", TRUE, "block", FALSE,
                 "max-buffers", guint64(1), "leaky-type", 2, nullptr);
    // Reuse host capture staging buffers. DONTWAIT acquisition below drops a
    // stale frame rather than allocating unboundedly or delaying input/audio.
    p->frame_pool = gst_buffer_pool_new();
    auto *pool_config = gst_buffer_pool_get_config(p->frame_pool);
    gst_buffer_pool_config_set_params(
        pool_config, caps, guint(p->source_width * p->source_height * 4),
        2, 8);
    if (!gst_buffer_pool_set_config(p->frame_pool, pool_config) ||
        !gst_buffer_pool_set_active(p->frame_pool, TRUE)) {
      gst_caps_unref(caps);
      throw std::runtime_error("capture staging buffer pool unavailable");
    }
    gst_caps_unref(caps);
    g_object_set(queue, "max-size-buffers", 1u, "max-size-bytes", 0u,
                 "max-size-time", guint64(0), "leaky", 2, nullptr);
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420",
                               "width", G_TYPE_INT, caps_profile.encoded_width,
                               "height", G_TYPE_INT,
                               caps_profile.encoded_height, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr);
    if (raw)
      gst_caps_set_simple(
          caps, "colorimetry", G_TYPE_STRING, "bt709", "interlace-mode",
          G_TYPE_STRING, "progressive", "pixel-aspect-ratio", GST_TYPE_FRACTION,
          1, 1, "framerate", GST_TYPE_FRACTION, caps_profile.fps, 1, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(pay, "mtu", 1200u, nullptr);
    if (p->plaintext) g_object_set(pay, "ssrc", p->ssrc, nullptr);
    if (raw || framed)
      g_object_set(pay, "pt", 96u, nullptr);
    else
      // Flush every parsed picture, including tiny unchanged-desktop P frames.
      // The default waits for an MTU and groups many pictures under one RTP
      // timestamp, causing bursty decode and dropped presentation frames.
      g_object_set(pay, "max-ptime", gint64(1), nullptr);
    auto *capture_pad = gst_element_get_static_pad(p->source, "src");
    gst_pad_add_probe(capture_pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                      source_allocation_query, nullptr, nullptr);
    gst_object_unref(capture_pad);
    link(p->source, queue);
    link(queue, convert);
    link(convert, scale);
    link(scale, filter);
    if (raw)
      link(filter, pay);
    else {
      p->encoder = element(p->pipeline, "avenc_mpeg2video");
      auto *encoded_pad = gst_element_get_static_pad(p->encoder, "src");
      gst_pad_add_probe(encoded_pad, GST_PAD_PROBE_TYPE_BUFFER, encoded_frame, p.get(), nullptr);
      gst_object_unref(encoded_pad);
      g_object_set(p->encoder, "bitrate", caps_profile.bitrate, "max-bframes",
                   0, "gop-size", std::max(1, caps_profile.fps / 2), nullptr);
      // No frame reordering on either side. Signal this in the MPEG-2
      // sequence header as well as disabling B pictures, and parallelize
      // slices within a picture rather than queueing whole pictures.
      gst_util_set_object_arg(G_OBJECT(p->encoder), "flags", "low_delay");
      g_object_set(p->encoder, "thread-type", 2, nullptr);
      g_object_set(p->encoder, "maxrate", gint64(caps_profile.bitrate),
                   "bufsize", std::max(163840, caps_profile.bitrate / 10),
                   "threads", 0, "mbd", 2, "trellis", 0, nullptr);
      link(filter, p->encoder);
      // avenc already emits one complete picture per buffer. A second
      // elementary-stream parser here waits for the next picture boundary,
      // adding a whole frame of delay before network transmission.
      // Encoding and paced UDP must run concurrently. Retain one COMPLETE
      // coded picture; dropping P pictures here would corrupt references.
      // The upstream raw queue remains leaky to bound capture latency.
      auto *coded_queue = element(p->pipeline, "queue");
      g_object_set(coded_queue, "max-size-buffers", 1u, "max-size-bytes", 0u,
                   "max-size-time", guint64(0), "leaky", 0, nullptr);
      if (framed) {
        // Preserve each complete encoder picture across RTP fragmentation.
        // MPV depayloading emits packet fragments, forcing a parser to wait
        // for the following picture boundary on the client.
        auto *pictures = element(p->pipeline, "capsfilter");
        p->pictures = pictures;
        auto *picture_caps = gst_caps_new_simple("video/mpeg", "mpegversion", G_TYPE_INT, 2,
          "systemstream", G_TYPE_BOOLEAN, FALSE, "parsed", G_TYPE_BOOLEAN, TRUE, nullptr);
        g_object_set(pictures, "caps", picture_caps, nullptr); gst_caps_unref(picture_caps);
        g_object_set(pay, "config-interval", 1u, nullptr);
        link(p->encoder, pictures); link(pictures, coded_queue);
      } else link(p->encoder, coded_queue);
      link(coded_queue, pay);
    }
    link(pay, p->gate);
    if (p->plaintext) link(p->gate, p->udpout);
    else {
    padlink(p->gate, "src", rtp, "send_rtp_sink_0", false, true);
    padlink(rtp, "send_rtp_src_0", p->enc, "rtp_sink_0", false, true);
    auto *drain = element(p->pipeline, "fakesink");
    g_object_set(drain, "sync", FALSE, "async", FALSE, nullptr);
    padlink(p->dec, "rtp_src", drain, "sink");
    }
  } else {
    auto *filter = element(p->pipeline, "capsfilter");
    auto *caps = gst_caps_new_simple(
        "application/x-rtp", "media", G_TYPE_STRING, audio ? "audio" : framed ? "application" : "video",
        "encoding-name", G_TYPE_STRING,
        audio ? "OPUS"
        : raw ? "RAW" : framed ? "X-GST"
              : "MPV",
        "clock-rate", G_TYPE_INT, audio ? 48000 : 90000, "payload", G_TYPE_INT,
        audio || raw || framed ? 96 : 32, nullptr);
    if (audio)
      gst_caps_set_simple(caps, "encoding-params", G_TYPE_STRING, "2", nullptr);
    if (raw)
      gst_caps_set_simple(
          caps, "sampling", G_TYPE_STRING, "YCbCr-4:2:0", "depth",
          G_TYPE_STRING, "8", "width", G_TYPE_STRING,
          std::to_string(caps_profile.encoded_width).c_str(), "height",
          G_TYPE_STRING, std::to_string(caps_profile.encoded_height).c_str(),
          "colorimetry", G_TYPE_STRING, "BT709-2", "a-framerate", G_TYPE_STRING,
          std::to_string(caps_profile.fps).c_str(), nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    auto *packet_pad = gst_element_get_static_pad(filter, "src");
    gst_pad_add_probe(packet_pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                      source_allocation_query, nullptr, nullptr);
    gst_object_unref(packet_pad);
    if (p->plaintext) link(udpin, p->gate);
    else padlink(p->dec, "rtp_src", p->gate, "sink");
    link(p->gate, filter);
    if (rtp) padlink(filter, "src", rtp, "recv_rtp_sink_0", false, true);
    auto *depay = element(p->pipeline, audio ? "rtpopusdepay"
                                       : raw ? "rtpvrawdepay"
                                             : framed ? "rtpgstdepay" : "rtpmpvdepay");
    if (p->plaintext) {
      auto *jitter = element(p->pipeline, "rtpjitterbuffer");
      g_object_set(jitter, "latency", p->jitter_ms, "drop-on-latency", audio,
                   "do-lost", TRUE, "faststart-min-packets", !audio || p->jitter_ms < 40 ? 1u : 0u, nullptr);
      p->jitters.push_back(GST_ELEMENT(gst_object_ref(jitter)));
      link(filter, jitter); link(jitter, depay);
    }
    p->queue = element(p->pipeline, "queue");
    // A zero window is reserved for explicit non-display integration tests.
    p->sink = element(p->pipeline, !window ? (raw ? "appsink" : "fakesink")
                                   : audio ? (desktop_output ? "pulsesink" : "alsasink")
                                           : (desktop_output ? "ximagesink" : "xvimagesink"));
    g_object_set(p->queue, "max-size-buffers", audio ? 0u : 1u,
                 "max-size-bytes", 0u, "max-size-time",
                 audio ? guint64(100 * GST_MSECOND) : guint64(0), "leaky", 2,
                 nullptr);
    // Remote interaction follows fresh network frames, rather than waiting a
    // second time on the RTP playback clock. Audio retains its own clock.
    // The jitter buffer still repairs packet order and bounds network delay;
    // the one-frame decoded queue replaces old frames during slow scanout.
    g_object_set(p->sink, "sync", audio, "async", FALSE, nullptr);
    if (!audio)
      // No playback-clock QoS may make the MPEG-2 decoder skip reference
      // pictures. Freshness is enforced before decode and by the one-frame
      // queue; sink timing feedback is unsuitable for immediate presentation.
      g_object_set(p->sink, "qos", FALSE, "max-lateness", gint64(-1),
                   "processing-deadline", guint64(0), "enable-last-sample", FALSE, nullptr);
    if (!window && raw) {
      g_object_set(p->sink, "emit-signals", TRUE, "max-buffers", 1u, "drop",
                   TRUE, "wait-on-eos", FALSE, nullptr);
      g_signal_connect(p->sink, "new-sample", G_CALLBACK(headless_sample),
                       p.get());
    }
    if (audio) {
      auto *decode = element(p->pipeline, "opusdec");
      // The pinned opusdec reserves 120 ms when FEC is enabled regardless
      // of our 20 ms packet duration. PLC keeps playback continuous without
      // that additional lookahead in this interactive profile.
      g_object_set(decode, "plc", TRUE, "use-inband-fec", FALSE, nullptr);
      auto *convert = element(p->pipeline, "audioconvert");
      auto *resample = element(p->pipeline, "audioresample");
      link(depay, decode);
      link(decode, convert);
      link(convert, resample);
      link(resample, p->queue);
      if (window)
        g_object_set(p->sink, "buffer-time", gint64(60000), "latency-time",
                     gint64(10000), nullptr);
    } else {
      if (raw) {
        // The host supplies final-size I420. The receiver only reassembles RTP
        // and presents YUV through XVideo; no decoder/converter/scaler runs.
        p->raw_coverage = std::make_unique<media_detail::RawFrameCoverage>(
            caps_profile.encoded_width, caps_profile.encoded_height);
        link(depay, p->queue);
        auto *packet_pad = gst_element_get_static_pad(depay, "sink");
        gst_pad_add_probe(packet_pad, GST_PAD_PROBE_TYPE_BUFFER, raw_packet,
                          p.get(), nullptr);
        gst_object_unref(packet_pad);
        auto *raw_pad = gst_element_get_static_pad(depay, "src");
        gst_pad_add_probe(raw_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          raw_complete_frame, p.get(), nullptr);
        gst_object_unref(raw_pad);
      } else {
        auto *decode = element(p->pipeline, "avdec_mpeg2video");
        p->decoder = decode;
        // Atom has two SMT threads, not two independent physical cores.
        // Slice workers share the current picture; frame workers also add
        // buffering, and the pinned libav wrapper accounts for that latency.
        g_object_set(decode, "max-threads", 2, "thread-type", 2, "qos", FALSE, nullptr);
        auto *input = gst_element_get_static_pad(decode, "sink");
        gst_pad_add_probe(input, GST_PAD_PROBE_TYPE_BUFFER, decoder_input, p.get(), nullptr); gst_object_unref(input);
        auto *output = gst_element_get_static_pad(decode, "src");
        gst_pad_add_probe(output, GST_PAD_PROBE_TYPE_BUFFER, decoder_output, p.get(), nullptr); gst_object_unref(output);
        // RTP reassembly must keep draining while decoding uses the Atom.
        // Discard only complete old pictures, never arbitrary RTP fragments.
        // queue marks the next buffer DISCONT after a leak, so the reference
        // gate above waits for a fresh I picture before accepting P pictures.
        p->compressed_queue = element(p->pipeline, "queue");
        g_object_set(p->compressed_queue, "max-size-buffers", 3u,
                     "max-size-bytes", 2u * 1024u * 1024u,
                     "max-size-time", guint64(0), "leaky", 2, nullptr);
        auto *compressed_pad = gst_element_get_static_pad(p->compressed_queue, "sink");
        gst_pad_add_probe(compressed_pad, GST_PAD_PROBE_TYPE_BUFFER, compressed_picture, p.get(), nullptr);
        gst_object_unref(compressed_pad);
        if (framed) link(depay, p->compressed_queue);
        else {
          auto *parse = element(p->pipeline, "mpegvideoparse");
          link(depay, parse); link(parse, p->compressed_queue);
        }
        link(p->compressed_queue, decode);
        link(decode, p->queue);
      }
      if (window) {
        g_object_set(p->sink, "handle-events", FALSE, nullptr);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(p->sink), "force-aspect-ratio"))
          g_object_set(p->sink, "force-aspect-ratio", TRUE, nullptr);
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(p->sink),
                                            guintptr(window));
      }
    }
    auto *loss_pad = gst_element_get_static_pad(depay, "sink");
    gst_pad_add_probe(loss_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      packet_lost, p.get(), nullptr);
    gst_object_unref(loss_pad);
    if (rtp) g_signal_connect(rtp, "pad-added", G_CALLBACK(dynamic_rtp), depay);
    if (desktop_output && !audio && window) {
      auto* convert = element(p->pipeline, "videoconvert");
      link(p->queue, convert); link(convert, p->sink);
    } else link(p->queue, p->sink);
    auto *pad = gst_element_get_static_pad(p->sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_frame, p.get(),
                      nullptr);
    gst_object_unref(pad);
  }
  return p;
}
struct Engine {
  std::vector<std::unique_ptr<Pipe>> pipes;
  std::thread monitor;
  std::atomic<bool> active{false}, ready{false};
  bool desktop_output = false;
  std::mutex mutex;
  std::map<std::string, std::uint64_t> previous_frames;
  std::map<std::string, std::uint64_t> previous_dropped;
  std::map<std::string, std::uint64_t> previous_decoded, previous_stream_lost, previous_stream_pushed;
  std::map<std::string, std::uint64_t> previous_captured, previous_submitted, previous_encoded, previous_bytes;
  std::map<std::string, std::uint64_t> previous_presentation_drops, previous_recovery_drops;
  std::uint64_t previous_lost = 0, previous_pushed = 0;
  std::chrono::steady_clock::time_point previous_sample{};
  std::function<void(const Json &)> handler;
  void authorize() {
    std::lock_guard<std::mutex> lock(mutex);
    ready = true;
    for (auto &p : pipes) g_object_set(p->gate, "drop", !p->verified, nullptr);
  }
  ~Engine() { stop(); }
  void stop() {
    active = false;
    if (monitor.joinable())
      monitor.join();
    std::lock_guard<std::mutex> lock(mutex);
    pipes.clear();
    previous_frames.clear();
    previous_dropped.clear();
    previous_decoded.clear(); previous_stream_lost.clear(); previous_stream_pushed.clear();
    previous_captured.clear(); previous_submitted.clear(); previous_encoded.clear(); previous_bytes.clear();
    previous_presentation_drops.clear(); previous_recovery_drops.clear();
    previous_lost = previous_pushed = 0;
    previous_sample = {};
    ready = false;
  }
  void start(const Json &offer, const std::vector<std::uintptr_t> &windows,
             const std::string &pem, bool sender,
             std::function<void(const Json &)> cb) {
    stop();
    init_gst();
    std::lock_guard<std::mutex> build_lock(mutex);
    handler = std::move(cb);
    auto streams = offer.at("streams");
    if (!streams.is_array() || streams.empty() || streams.size() > 3)
      throw std::runtime_error(
          "expected one or two video streams and optional audio");
    try {
      std::size_t video_count = 0, audio_count = 0;
      std::set<std::string> ids;
      const bool test_output =
          !windows.empty() && std::all_of(windows.begin(), windows.end(),
                                          [](auto w) { return w == 0; });
      for (const auto &stream : streams) {
        const auto kind = stream.at("kind").get<std::string>();
        if (!ids.insert(stream.at("id").get<std::string>()).second)
          throw std::runtime_error("duplicate media stream id");
        std::uintptr_t window = 0;
        if (kind == "video") {
          if (++video_count > 2)
            throw std::runtime_error("at most two video streams are supported");
          if (!sender)
            window = windows.at(video_count - 1);
        } else if (kind == "audio") {
          if (++audio_count > 1)
            throw std::runtime_error("at most one audio stream is supported");
          window = test_output ? 0 : 1;
        } else
          throw std::runtime_error("unsupported media kind");
        pipes.push_back(build_pipe(offer, stream, sender, window, pem, desktop_output));
      }
      if (!video_count)
        throw std::runtime_error("a video stream is required");
      for (auto &p : pipes)
        if (gst_element_set_state(p->pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE)
          throw std::runtime_error("media pipeline startup failed");
    } catch (...) {
      pipes.clear();
      throw;
    }
    active = true;
    monitor = std::thread([this] {
      bool notified = false;
            auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(12);
      while (active) {
        bool all = true;
        for (auto &p : pipes) {
          if (p->raw_sync_failed) {
            if (handler)
              handler({{"type", "error"},
                       {"message", "Raw reassembly lost frame synchronization; "
                                   "reconnect required"}});
            active = false;
            break;
          }
          auto *bus = gst_element_get_bus(p->pipeline);
          // Timing becomes known after the dynamic RTP pad/decoder starts.
          // Ignoring LATENCY messages leaves the sink's clock compensation
          // stale; QoS also supplies pressure before a frame queue fills.
          GstMessage *msg = nullptr;
          for (unsigned messages = 0; messages < 64 && active &&
               (msg = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(
                   GST_MESSAGE_ERROR | GST_MESSAGE_LATENCY | GST_MESSAGE_QOS))); ++messages) {
          if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_LATENCY) {
            gst_bin_recalculate_latency(GST_BIN(p->pipeline));
            gst_message_unref(msg);
          } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_QOS) {
            gint64 jitter = 0;
            gdouble proportion = 1;
            gint quality = 0;
            gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);
            p->lateness_ns = jitter;
            p->qos_proportion = proportion;
            if (p->decoder && GST_MESSAGE_SRC(msg) == GST_OBJECT(p->decoder)) {
              GstFormat format;
              guint64 processed = 0, discarded = 0;
              gst_message_parse_qos_stats(msg, &format, &processed, &discarded);
              if (format == GST_FORMAT_BUFFERS) p->decoder_dropped = discarded;
            }
            gst_message_unref(msg);
          } else if (msg) {
            GError *e = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &e, &debug);
            auto reason = std::string(e ? e->message : "media failure");
            g_clear_error(&e);
            g_free(debug);
            gst_message_unref(msg);
            if (handler)
              handler({{"type", "error"}, {"message", reason}});
            active = false;
            break;
          }
          }
          gst_object_unref(bus);
          if (!active) break;
          if (!p->plaintext && !p->verified) {
            gchar *peer = nullptr;
            g_object_get(p->dec, "peer-pem", &peer, nullptr);
            if (peer) {
              try {
                if (pem_fingerprint(peer) != p->expected)
                  throw std::runtime_error("DTLS peer fingerprint mismatch");
                p->verified = true;
              } catch (const std::exception &e) {
                if (handler)
                  handler({{"type", "error"}, {"message", e.what()}});
                active = false;
              }
              g_free(peer);
            }
          }
          all = all && p->verified.load();
          {
            std::lock_guard<std::mutex> lock(mutex);
            g_object_set(p->gate, "drop", !(p->verified && ready), nullptr);
          }
        }
        if (!active) {
          for (auto &p : pipes)
            g_object_set(p->gate, "drop", TRUE, nullptr);
          break;
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto &p : pipes) {
          if (now - p->last_recovery_request >= std::chrono::milliseconds(500) &&
              p->recovery_requested.exchange(false) && handler) {
            handler({{"type", "keyframe"}, {"id", p->id}});
            p->last_recovery_request = now;
          }
        }
        if (all && !notified) {
          notified = true;
          if (handler)
            handler({{"type", "media_verified"}});
        }
        if (!all && std::chrono::steady_clock::now() > deadline) {
          if (handler)
            handler({{"type", "error"},
                     {"message", "DTLS media handshake timed out"}});
          active = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
};
} // namespace
struct MediaReceiver::Impl {
  Engine engine;
};
struct MediaSender::Impl {
  Engine engine;
};
bool media_available() { return true; }
Json media_capabilities() {
  init_gst();
  Json missing = Json::array();
  for (auto name :
       {"dtlssrtpenc", "dtlssrtpdec", "rtpbin", "rtpmpvpay", "rtpmpvdepay",
        "mpegvideoparse", "avenc_mpeg2video", "avdec_mpeg2video", "xvimagesink",
        "opusenc", "opusdec", "rtpopuspay", "rtpopusdepay", "rtpvrawpay",
        "rtpvrawdepay", "rtpgstpay", "rtpgstdepay"}) {
    auto *f = gst_element_factory_find(name);
    if (!f)
      missing.push_back(name);
    else
      gst_object_unref(f);
  }
  auto *pipewire = gst_element_factory_find("pipewiresrc");
  const bool user_playback = pipewire != nullptr;
  if (pipewire) gst_object_unref(pipewire);
  return {{"compiled", true},  {"missing_elements", missing}, {"user_playback", user_playback},
          {"codec", "mpeg2"},  {"codecs", Json::array({"mpeg2", "raw-i420"})},
          {"raw_max_fps", 60}, {"mpeg2_max_fps", 60}, {"audio", "opus-48000-stereo"},
          {"transports", Json::array({"dtls-srtp", "rtp-udp"})},
          {"mpeg2_payloads", Json::array({"gst-frame", "mpv"})},
          {"encrypted", true}};
}
void MediaReceiver::start(const Json &o, const std::vector<std::uintptr_t> &w,
                          const std::string &p, Handler cb) {
  impl_->engine.start(o, w, p, false, std::move(cb));
}
namespace {
bool compatible_profiles(const Engine &engine, const std::vector<Display> &profiles) {
  try { (void)parse_displays(Json(profiles)); } catch (...) { return false; }
  std::size_t videos = 0;
  for (const auto &p : engine.pipes) {
    if (p->kind != "video") continue;
    ++videos;
    const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const Display &d) { return d.id == p->id; });
    if (it == profiles.end()) return false;
    const auto &a = p->display, &b = *it;
    if (a.codec != "mpeg2" || b.codec != a.codec || a.width != b.width || a.height != b.height ||
        a.encoded_width != b.encoded_width || a.encoded_height != b.encoded_height || a.x != b.x || a.y != b.y)
      return false;
  }
  return videos == profiles.size() && videos != 0 && engine.active;
}
}
bool MediaReceiver::update_profiles(const std::vector<Display> &profiles) {
  auto &engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  if (!compatible_profiles(engine, profiles)) return false;
  for (const auto &d : profiles)
    for (auto &p : engine.pipes) if (p->kind == "video" && p->id == d.id) p->display = d;
  return true;
}
bool MediaSender::reconfigure(const std::vector<Display> &profiles) {
  auto &engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  if (!compatible_profiles(engine, profiles)) return false;
  for (const auto &p : engine.pipes)
    if (p->kind == "video" && (!p->raw_filter || !p->pictures || !p->encoder)) return false;
  for (const auto &d : profiles) for (auto &p : engine.pipes) {
    if (p->kind != "video" || p->id != d.id || (p->display.fps == d.fps && p->display.bitrate == d.bitrate)) continue;
    std::string error;
    if (!media_detail::replace_mpeg2_encoder(p->pipeline, p->source, p->raw_filter,
        p->pictures, p->encoder, d, [&](GstElement *encoder) {
          auto *pad = gst_element_get_static_pad(encoder, "src");
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, encoded_frame, p.get(), nullptr);
          gst_object_unref(pad);
        }, error)) return false;
    p->display = d;
    p->next_frame = {};
    // Codec bitrate is an average content budget. Keep the negotiated packet
    // ceiling so an I picture finishes promptly after a codec/FPS reduction.
    // Reducing both extended serialization beyond the RTP repair deadline.
    if (p->pacer) p->pacer->set_rate(p->wire_ceiling_bps);
  }
  return true;
}
void MediaReceiver::stop() { impl_->engine.stop(); }
void MediaReceiver::authorize() { impl_->engine.authorize(); }
void MediaReceiver::set_desktop_output(bool enabled) {
  auto& engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  if (engine.active) throw std::runtime_error("Stop media before changing presentation mode");
  engine.desktop_output = enabled;
}
void MediaReceiver::refresh_windows() {
  auto& engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  if (!engine.ready) return;
  for (const auto& pipe : engine.pipes)
    if (pipe->kind == "video" && pipe->verified)
      media_detail::refresh_video_window(pipe->sink);
}
Json MediaReceiver::stats() const {
  auto &engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  const auto now = std::chrono::steady_clock::now();
  const double elapsed =
      engine.previous_sample.time_since_epoch().count()
          ? std::chrono::duration<double>(now - engine.previous_sample).count()
          : 0.0;
  std::uint64_t frames = 0, audio_buffers = 0, lost = 0, pushed = 0, late = 0;
  double max_queue_ms = 0, max_drop_fraction = 0, max_lateness_ms = 0;
  Json streams = Json::array();
  for (const auto &p : engine.pipes) {
    const auto decoded = p->kind == "video" && p->codec == "mpeg2" ? p->decoded_frames.load() : p->frames.load();
    guint64 count = 0, dropped = 0;
    auto *sink_stats = gst_base_sink_get_stats(GST_BASE_SINK(p->sink));
    if (sink_stats) {
      gst_structure_get_uint64(sink_stats, "rendered", &count);
      gst_structure_get_uint64(sink_stats, "dropped", &dropped);
      gst_structure_free(sink_stats);
    }
    dropped += p->decoder_dropped.load();
    guint64 queued = 0;
    if (p->queue)
      g_object_get(p->queue, "current-level-time", &queued, nullptr);
    if (p->kind == "video" && p->codec == "mpeg2") {
      guint pending = 0;
      g_object_get(p->queue, "current-level-buffers", &pending, nullptr);
      const auto passed = p->frames.load();
      // Frames removed by the leaky queue never reach the sink's QoS counts.
      // Use a monotonic lower bound, excluding pending and in-flight buffers.
      if (decoded > passed + pending + 1)
        p->queue_replaced = std::max(p->queue_replaced, decoded - passed - pending - 1);
      dropped += p->queue_replaced;
    }
    if (p->compressed_queue) {
      guint pending = 0;
      guint64 compressed_time = 0;
      g_object_get(p->compressed_queue, "current-level-buffers", &pending,
                   "current-level-time", &compressed_time, nullptr);
      const auto in = p->compressed_received.load(), out = p->compressed_passed.load();
      if (in > out + pending + 1)
        p->compressed_replaced = std::max(p->compressed_replaced, in - out - pending - 1);
      queued = std::max(queued, compressed_time);
    }
    const auto presentation_drops = dropped + p->compressed_replaced;
    const auto recovery_drops = p->recovery_discarded.load();
    dropped = presentation_drops + recovery_drops;
    const double queue_ms = double(queued) / GST_MSECOND;
    guint64 stream_lost = 0, stream_pushed = 0, stream_late = 0;
    {
      std::lock_guard<std::mutex> jitter_lock(p->jitter_mutex);
      for (auto *jitter : p->jitters) {
        GstStructure *stats = nullptr;
        g_object_get(jitter, "stats", &stats, nullptr);
        if (!stats)
          continue;
        guint64 n = 0;
        if (gst_structure_get_uint64(stats, "num-lost", &n))
          stream_lost += n;
        if (gst_structure_get_uint64(stats, "num-pushed", &n))
          stream_pushed += n;
        if (gst_structure_get_uint64(stats, "num-late", &n))
          stream_late += n;
        gst_structure_free(stats);
      }
    }
    lost += stream_lost;
    pushed += stream_pushed;
    late += stream_late;
    const auto previous = engine.previous_frames[p->id];
    const auto previous_dropped = engine.previous_dropped[p->id];
    const auto delivered_delta = count >= previous ? count - previous : 0;
    const auto dropped_delta = dropped >= previous_dropped ? dropped - previous_dropped : 0;
    const double drop_fraction = delivered_delta + dropped_delta
        ? double(dropped_delta) / double(delivered_delta + dropped_delta) : 0;
    const double lateness_ms = std::max(0.0, double(p->lateness_ns.load()) / GST_MSECOND);
    const double rate = elapsed > 0 && count >= previous
                            ? double(count - previous) / elapsed
                            : 0;
    const auto lost_delta = stream_lost >= engine.previous_stream_lost[p->id] ? stream_lost - engine.previous_stream_lost[p->id] : 0;
    const auto pushed_delta = stream_pushed >= engine.previous_stream_pushed[p->id] ? stream_pushed - engine.previous_stream_pushed[p->id] : 0;
    const auto fraction = [delivered_delta](std::uint64_t current, std::uint64_t previous) {
      const auto delta = current >= previous ? current - previous : 0;
      return delivered_delta + delta ? double(delta) / double(delivered_delta + delta) : 0.0;
    };
    const auto received = p->received_bytes.load();
    gint socket_receive_buffer = 0;
    g_socket_get_option(p->socket, SOL_SOCKET, SO_RCVBUF, &socket_receive_buffer, nullptr);
    Json detail{{"id", p->id},
                {"kind", p->kind},
                {"buffers", count},
                {"transport", p->plaintext ? "rtp-udp" : "dtls-srtp"},
                {"payload", p->payload}, {"jitter_ms", p->jitter_ms},
                {"decoded_buffers", decoded},
                {"decoded_fps", elapsed > 0 && decoded >= engine.previous_decoded[p->id] ? double(decoded - engine.previous_decoded[p->id]) / elapsed : 0.0},
                {"loss", lost_delta + pushed_delta ? double(lost_delta) / double(lost_delta + pushed_delta) : 0.0},
                {"presentation", p->kind == "audio" ? "audio-clock" : "fresh-frame"},
                {"clock_sync", bool(gst_base_sink_get_sync(GST_BASE_SINK(p->sink)))},
                {"clock_qos", bool(gst_base_sink_is_qos_enabled(GST_BASE_SINK(p->sink)))},
                {"decode_ms", p->decode_ms.load()},
                {"receiver_timeline_delay_ms", p->timeline_delay_ms.load()},
                {"recovery_discarded_frames", p->recovery_discarded.load()},
                {"decoder_dropped_frames", p->decoder_dropped.load()},
                {"dropped_frames", dropped},
                {"queue_replaced_frames", p->queue_replaced},
                {"drop_fraction", drop_fraction},
                {"presentation_drop_fraction", fraction(presentation_drops, engine.previous_presentation_drops[p->id])},
                {"recovery_drop_fraction", fraction(recovery_drops, engine.previous_recovery_drops[p->id])},
                {"compressed_queue_drops", p->compressed_replaced},
                {"receive_mbps", elapsed > 0 && received >= engine.previous_bytes[p->id] ?
                    double(received - engine.previous_bytes[p->id]) * 8 / elapsed / 1000000 : 0.0},
                {"socket_receive_buffer_bytes", socket_receive_buffer},
                {"lateness_ms", lateness_ms},
                {"qos_proportion", p->qos_proportion.load()},
                {"playout_latency_ms", double(gst_base_sink_get_latency(GST_BASE_SINK(p->sink))) / GST_MSECOND},
                {"queue_ms", queue_ms},
                {"packets_lost", stream_lost},
                {"packets_late", stream_late}};
    if (p->kind == "video") {
      detail["encoded_width"] = p->display.encoded_width;
      detail["encoded_height"] = p->display.encoded_height;
      detail["target_fps"] = p->display.fps;
      auto *factory = gst_element_get_factory(p->sink);
      detail["sink"] = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "unknown";
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(p->sink), "device-name")) {
        gchar *adapter = nullptr;
        g_object_get(p->sink, "device-name", &adapter, nullptr);
        detail["output_adapter"] = adapter ? adapter : "not negotiated";
        g_free(adapter);
      }
      // Audio's own bounded playback queue must never throttle video.
      max_queue_ms = std::max(max_queue_ms, queue_ms);
      max_drop_fraction = std::max(max_drop_fraction, drop_fraction);
      max_lateness_ms = std::max(max_lateness_ms, lateness_ms);
      frames += count;
      detail["fps"] = rate;
      detail["codec"] = p->codec;
      if (p->codec == "raw-i420")
        detail["incomplete_frames"] = p->incomplete_frames.load();
      {
        std::lock_guard<std::mutex> digest_lock(p->digest_mutex);
        if (p->checked_frames) {
          detail["headless_sha256"] = p->headless_digest;
          detail["headless_checked_frames"] = p->checked_frames;
          detail["headless_changed_frames"] = p->changed_frames;
        }
      }
    } else {
      audio_buffers += count;
      detail["buffers_per_second"] = rate;
    }
    streams.push_back(std::move(detail));
    engine.previous_presentation_drops[p->id] = presentation_drops;
    engine.previous_recovery_drops[p->id] = recovery_drops;
    engine.previous_bytes[p->id] = received;
    engine.previous_frames[p->id] = count;
    engine.previous_dropped[p->id] = dropped;
    engine.previous_decoded[p->id] = decoded;
    engine.previous_stream_lost[p->id] = stream_lost;
    engine.previous_stream_pushed[p->id] = stream_pushed;
  }
  const auto lost_delta =
      lost >= engine.previous_lost ? lost - engine.previous_lost : 0;
  const auto pushed_delta =
      pushed >= engine.previous_pushed ? pushed - engine.previous_pushed : 0;
  const double loss =
      (lost_delta + pushed_delta)
          ? double(lost_delta) / double(lost_delta + pushed_delta)
          : 0;
  engine.previous_lost = lost;
  engine.previous_pushed = pushed;
  engine.previous_sample = now;
  return {{"frames", frames},     {"audio_buffers", audio_buffers},
          {"loss", loss},         {"queue_ms", max_queue_ms},
          {"packets_lost", lost}, {"packets_late", late},
          {"drop_fraction", max_drop_fraction}, {"lateness_ms", max_lateness_ms},
          {"streams", streams},   {"running", engine.active.load()}};
}
Json MediaSender::stats() const {
  auto &engine = impl_->engine;
  std::lock_guard<std::mutex> lock(engine.mutex);
  const auto now = std::chrono::steady_clock::now();
  const double seconds = engine.previous_sample.time_since_epoch().count() ? std::chrono::duration<double>(now - engine.previous_sample).count() : 0;
  Json streams = Json::array();
  for (const auto &p : engine.pipes) {
    if (p->kind != "video") continue;
    const auto captured = p->captured_frames.load(), submitted = p->submitted_frames.load(), encoded = p->encoded_frames.load(), bytes = p->wire_bytes.load();
    const auto rate = [seconds](std::uint64_t current, std::uint64_t previous) { return seconds > 0 && current >= previous ? double(current - previous) / seconds : 0.0; };
    streams.push_back({{"id", p->id}, {"capture_fps", rate(captured, engine.previous_captured[p->id])},
                      {"submitted_fps", rate(submitted, engine.previous_submitted[p->id])},
                      {"encoded_fps", rate(encoded, engine.previous_encoded[p->id])},
                      {"rtp_mbps", rate(bytes, engine.previous_bytes[p->id]) * 8 / 1000000}});
    engine.previous_captured[p->id] = captured; engine.previous_submitted[p->id] = submitted;
    engine.previous_encoded[p->id] = encoded; engine.previous_bytes[p->id] = bytes;
  }
  engine.previous_sample = now;
  return {{"streams", streams}, {"running", engine.active.load()}};
}
void MediaSender::start(const Json &o, const std::string &p,
                        std::function<void(const Json &)> cb) {
  impl_->engine.start(o, {}, p, true, std::move(cb));
}
void MediaSender::stop() { impl_->engine.stop(); }
void MediaSender::authorize() { impl_->engine.authorize(); }
void MediaSender::frame(const std::string &id, int width, int height,
                        int stride, const std::uint8_t *data,
                        std::size_t size) {
  std::lock_guard<std::mutex> lock(impl_->engine.mutex);
  if (!impl_->engine.active || !impl_->engine.ready)
    return;
  if (width <= 0 || height <= 0 || stride < width * 4 ||
      size < std::size_t(stride) * height)
    return;
  for (auto &p : impl_->engine.pipes)
    if (p->kind == "video" && p->id == id && p->source &&
        p->source_width == width && p->source_height == height) {
      ++p->captured_frames;
      if (!p->verified)
        return;
      auto now = std::chrono::steady_clock::now();
      const auto period = std::chrono::microseconds(1000000 / p->display.fps);
      // Capture has its own cadence. Accept a slightly early fresh capture
      // rather than discarding every other picture when those two schedules
      // differ by copying/poll jitter. The phase still limits average rate.
      if (now + period / 4 < p->next_frame)
        return;
      // Advance the schedule instead of timing each frame from its arrival;
      // small capture jitter must not repeatedly turn 60 fps into 30 fps.
      if (now - p->next_frame > period)
        p->next_frame = now + period;
      else
        p->next_frame += period;
      GstBuffer *b = nullptr;
      GstBufferPoolAcquireParams acquire{};
      acquire.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
      if (gst_buffer_pool_acquire_buffer(p->frame_pool, &b, &acquire) !=
          GST_FLOW_OK)
        return;
      GstMapInfo map;
      if (gst_buffer_map(b, &map, GST_MAP_WRITE)) {
        for (int y = 0; y < height; ++y)
          std::memcpy(map.data + std::size_t(y) * width * 4,
                      data + std::size_t(y) * stride, std::size_t(width) * 4);
        gst_buffer_unmap(b, &map);
        GST_BUFFER_DURATION(b) = GST_SECOND / unsigned(p->display.fps);
        if (gst_app_src_push_buffer(GST_APP_SRC(p->source), b) == GST_FLOW_OK) ++p->submitted_frames;
      } else
        gst_buffer_unref(b);
      return;
    }
}
void MediaSender::request_keyframe(const std::string &id) {
  std::lock_guard<std::mutex> lock(impl_->engine.mutex);
  for (auto &p : impl_->engine.pipes)
    if (p->kind == "video" && p->encoder && (id.empty() || p->id == id))
      gst_element_send_event(p->encoder,
                             gst_video_event_new_downstream_force_key_unit(
                                 GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE,
                                 GST_CLOCK_TIME_NONE, TRUE, 0));
}
#else
struct MediaReceiver::Impl {};
struct MediaSender::Impl {};
bool media_available() { return false; }
Json media_capabilities() {
  return {{"compiled", false}, {"reason", "build with REMOTEOS_WITH_MEDIA=ON"}};
}
void MediaReceiver::start(const Json &, const std::vector<std::uintptr_t> &,
                          const std::string &, Handler) {
  throw std::runtime_error("media support was not compiled");
}
void MediaReceiver::stop() {}
bool MediaReceiver::update_profiles(const std::vector<Display> &) { return false; }
bool MediaSender::reconfigure(const std::vector<Display> &) { return false; }
void MediaReceiver::authorize() {}
void MediaReceiver::refresh_windows() {}
void MediaReceiver::set_desktop_output(bool) {}
Json MediaReceiver::stats() const { return {{"running", false}}; }
Json MediaSender::stats() const { return {{"running", false}}; }
void MediaSender::start(const Json &, const std::string &,
                        std::function<void(const Json &)>) {
  throw std::runtime_error("media support was not compiled");
}
void MediaSender::frame(const std::string &, int, int, int,
                        const std::uint8_t *, std::size_t) {}
void MediaSender::authorize() {}
void MediaSender::request_keyframe(const std::string &id) {}
void MediaSender::stop() {}
#endif
MediaReceiver::MediaReceiver() : impl_(std::make_unique<Impl>()) {}
MediaReceiver::~MediaReceiver() = default;
MediaSender::MediaSender() : impl_(std::make_unique<Impl>()) {}
MediaSender::~MediaSender() = default;
} // namespace remoteos
