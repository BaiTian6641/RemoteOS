#include "../src/media/encoder_reconfigure.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace remoteos;
using Clock = std::chrono::steady_clock;
void require(bool condition, const std::string &why) { if (!condition) throw std::runtime_error(why); }
struct Observed {
  std::mutex mutex;
  std::uint64_t bytes = 0, frames = 0, audio = 0, gaps = 0;
  unsigned sequence_headers = 0, intra = 0, fps_code = 0;
  guint32 ssrc = 0;
  guint16 last_seq = 0;
  bool seen = false;
  unsigned expected_refresh_fps = 0, bad_refresh = 0, checked_refresh = 0;
  std::vector<unsigned char> picture;
  void packet(GstBuffer *buffer) {
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) return;
    std::lock_guard<std::mutex> lock(mutex);
    const auto seq = gst_rtp_buffer_get_seq(&rtp);
    const auto incoming_ssrc = gst_rtp_buffer_get_ssrc(&rtp);
    if (seen && (guint16(last_seq + 1) != seq || ssrc != incoming_ssrc)) ++gaps;
    seen = true; last_seq = seq; ssrc = incoming_ssrc;
    bytes += gst_buffer_get_size(buffer);
    const auto *data = static_cast<const unsigned char *>(gst_rtp_buffer_get_payload(&rtp));
    const auto size = gst_rtp_buffer_get_payload_len(&rtp);
    if (size >= 8) {
      const auto offset = (unsigned(data[4]) << 24) | (unsigned(data[5]) << 16) | (unsigned(data[6]) << 8) | data[7];
      if (!offset) picture.clear();
      if (offset == picture.size()) picture.insert(picture.end(), data + 8, data + size);
      if (gst_rtp_buffer_get_marker(&rtp)) {
        bool picture_header = false;
        for (std::size_t i = 0; i + 8 <= picture.size(); ++i) {
          if (picture[i] || picture[i + 1] || picture[i + 2] != 1) continue;
          if (picture[i + 3] == 0xb3) { ++sequence_headers; fps_code = picture[i + 7] & 15; picture_header = true; }
          if (picture[i + 3] == 0) {
            ++frames;
            const bool is_intra = ((picture[i + 5] >> 3) & 7) == 1;
            if (is_intra) ++intra;
            if (expected_refresh_fps) {
              if (!picture_header || !is_intra || fps_code != expected_refresh_fps) ++bad_refresh;
              expected_refresh_fps = 0; ++checked_refresh;
            }
          }
        }
        picture.clear();
      }
    }
    gst_rtp_buffer_unmap(&rtp);
  }
};
struct Stall { std::atomic<bool> entered{false}; unsigned milliseconds = 75; };
GstFlowReturn sample(GstAppSink *sink, gpointer data) {
  auto *observed = static_cast<Observed *>(data);
  auto *value = gst_app_sink_pull_sample(sink);
  if (!value) return GST_FLOW_FLUSHING;
  if (auto *list = gst_sample_get_buffer_list(value))
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) observed->packet(gst_buffer_list_get(list, i));
  else if (auto *buffer = gst_sample_get_buffer(value)) observed->packet(buffer);
  gst_sample_unref(value);
  return GST_FLOW_OK;
}
void check_errors(GstElement *pipeline) {
  auto *bus = gst_element_get_bus(pipeline);
  auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (!message) return;
  GError *error = nullptr; gchar *debug = nullptr;
  gst_message_parse_error(message, &error, &debug);
  const std::string why = std::string(error ? error->message : "pipeline error") + " " + (debug ? debug : "");
  g_clear_error(&error); g_free(debug); gst_message_unref(message);
  throw std::runtime_error(why);
}
int main() {
  gst_init(nullptr, nullptr);
  GstElement *pipeline = nullptr;
  Observed observed;
  Stall stall;
  try {
    GError *error = nullptr;
    pipeline = gst_parse_launch(
      "appsrc name=capture is-live=true format=time do-timestamp=true block=false max-buffers=1 leaky-type=downstream "
      "caps=video/x-raw,format=BGRA,width=640,height=360,framerate=60/1 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
      "videoconvert ! capsfilter name=raw caps=video/x-raw,format=I420,width=640,height=360,framerate=60/1 ! "
      "avenc_mpeg2video name=encoder bitrate=3000000 maxrate=3000000 bufsize=750000 max-bframes=0 gop-size=30 flags=low_delay thread-type=slice threads=0 mbd=2 trellis=1 ! "
      "capsfilter name=pictures caps=video/mpeg,mpegversion=2,systemstream=false,parsed=true ! "
      "rtpgstpay name=pay mtu=1200 pt=96 ssrc=12345678 config-interval=1 ! "
      "appsink name=packets sync=false async=false max-buffers=1 drop=false buffer-list=true "
      "audiotestsrc is-live=true samplesperbuffer=960 ! fakesink name=audio sync=false async=false signal-handoffs=true", &error);
    require(pipeline && !error, error ? error->message : "pipeline construction failed");
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "capture");
    auto *source_pad = gst_element_get_static_pad(source, "src");
    gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                     media_detail::owned_buffer_allocation_query, nullptr, nullptr);
    gst_object_unref(source_pad);
    auto *raw = gst_bin_get_by_name(GST_BIN(pipeline), "raw");
    auto *pictures = gst_bin_get_by_name(GST_BIN(pipeline), "pictures");
    auto *encoder_owned = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
    auto *encoder = encoder_owned;
    auto *packets = gst_bin_get_by_name(GST_BIN(pipeline), "packets");
    auto *audio = gst_bin_get_by_name(GST_BIN(pipeline), "audio");
    GstAppSinkCallbacks callbacks{}; callbacks.new_sample = sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(packets), &callbacks, &observed, nullptr);
    g_signal_connect(audio, "handoff", G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer data) {
      auto &observed = *static_cast<Observed *>(data);
      std::lock_guard<std::mutex> lock(observed.mutex); ++observed.audio;
    }), &observed);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "pipeline startup failed");
    auto push = [&](unsigned frame, int fps) {
      auto *buffer = gst_buffer_new_allocate(nullptr, 640 * 360 * 4, nullptr);
      GstMapInfo map{}; require(gst_buffer_map(buffer, &map, GST_MAP_WRITE), "frame mapping failed");
      for (unsigned y = 0; y < 360; ++y) for (unsigned x = 0; x < 640; ++x) {
        // Whole-picture changing, textured content exercises rate control.
        const unsigned value = ((x / 4) * 13871u + (y / 4) * 31513u + frame * 1277u) ^
                               ((x / 4 + frame * 7) * (y / 4 + frame * 3) * 17u);
        auto *pixel = map.data + (y * 640 + x) * 4;
        pixel[0] = 112 + (value & 31); pixel[1] = pixel[0]; pixel[2] = pixel[0]; pixel[3] = 255;
      }
      gst_buffer_unmap(buffer, &map);
      GST_BUFFER_DURATION(buffer) = GST_SECOND / fps;
      require(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK, "capture stopped flowing");
    };
    unsigned frame = 0;
    auto run = [&](int fps, unsigned milliseconds) {
      std::uint64_t before_bytes, before_frames;
      { std::lock_guard<std::mutex> lock(observed.mutex); before_bytes = observed.bytes; before_frames = observed.frames; }
      const auto begin = Clock::now(), end = begin + std::chrono::milliseconds(milliseconds);
      auto next = begin;
      while (Clock::now() < end) {
        push(frame++, fps);
        next += std::chrono::nanoseconds(1000000000 / fps);
        std::this_thread::sleep_until(next);
        check_errors(pipeline);
      }
      const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
      std::lock_guard<std::mutex> lock(observed.mutex);
      return Json{{"mbps", (observed.bytes - before_bytes) * 8.0 / seconds / 1000000},
                  {"fps", (observed.frames - before_frames) / seconds}, {"fps_code", observed.fps_code},
                  {"headers", observed.sequence_headers}, {"intra", observed.intra}, {"audio", observed.audio}};
    };
    run(60, 700);
    const auto before = run(60, 1800);
    Display profile{"internal", 640, 360, 0, 0, 640, 360, 30, 12000000};
    std::string reason;
    auto refresh = [&](unsigned fps_code) {
      return [&, fps_code](GstElement *) {
        std::lock_guard<std::mutex> lock(observed.mutex); observed.expected_refresh_fps = fps_code;
      };
    };
    const auto changed = Clock::now();
    require(media_detail::replace_mpeg2_encoder(pipeline, source, raw, pictures, encoder, profile, refresh(5), reason), reason);
    const auto change_ms = std::chrono::duration<double, std::milli>(Clock::now() - changed).count();
    run(30, 700);
    const auto after = run(30, 1800);
    check_errors(pipeline);
    require(before["fps_code"] == 8 && after["fps_code"] == 5, "MPEG-2 sequence header did not change from 60 to 30 fps");
    require(before["fps"].get<double>() >= 50 && after["fps"].get<double>() >= 25, "encoder replacement interrupted picture cadence");
    require(after["mbps"].get<double>() > before["mbps"].get<double>() * 1.5, "actual high-motion bitrate did not rise after reconfigure");
    require(after["headers"] > before["headers"] && after["intra"] > before["intra"], "replacement did not emit fresh sequence headers and I pictures");
    require(after["audio"].get<unsigned>() >= before["audio"].get<unsigned>() + 100, "unrelated audio stopped during encoder replacement");
    profile.bitrate = 3000000;
    require(media_detail::replace_mpeg2_encoder(pipeline, source, raw, pictures, encoder, profile, refresh(5), reason), reason);
    run(30, 700);
    const auto bitrate_only = run(30, 1800);
    require(bitrate_only["fps_code"] == 5 && bitrate_only["fps"].get<double>() >= 25,
            "bitrate-only replacement changed or interrupted frame rate");
    require(bitrate_only["mbps"].get<double>() < after["mbps"].get<double>() * .65,
            "same-caps bitrate reduction did not change encoded bytes");
    auto *encoded = gst_element_get_static_pad(encoder, "src");
    gst_pad_add_probe(encoded, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer data) {
          auto &stall = *static_cast<Stall *>(data);
          stall.entered = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(stall.milliseconds));
          return GST_PAD_PROBE_REMOVE;
        }, &stall, nullptr);
    gst_object_unref(encoded);
    push(frame++, 30);
    const auto stall_deadline = Clock::now() + std::chrono::seconds(1);
    while (!stall.entered && Clock::now() < stall_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(stall.entered, "test encoder never reached the in-flight picture");
    push(frame++, 30);
    profile.fps = 60;
    require(media_detail::replace_mpeg2_encoder(pipeline, source, raw, pictures, encoder, profile, refresh(8), reason), reason);
    // Let the already queued old-rate picture reach the new encoder boundary
    // before submitting fresh capture. It must not cause negotiation failure
    // or become the replacement's first picture with the obsolete frame rate.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    run(60, 700);
    const auto queued_change = run(60, 1000);
    require(queued_change["fps_code"] == 8 && queued_change["fps"].get<double>() >= 50,
            "an in-flight picture prevented frame-rate reconfiguration: " + queued_change.dump());
    auto *before_rollback = encoder;
    profile.bitrate = 6000000;
    require(!media_detail::replace_mpeg2_encoder(pipeline, source, raw, pictures, encoder, profile,
                [](GstElement *) { throw std::runtime_error("test installation failure"); }, reason) &&
            encoder == before_rollback && reason.find("test installation failure") != std::string::npos,
            "encoder installation failure did not restore the original element");
    const auto rollback = run(60, 700);
    require(rollback["fps"].get<double>() >= 50, "rollback did not restore a flowing encoder");
    stall.entered = false;
    stall.milliseconds = 2200;
    encoded = gst_element_get_static_pad(encoder, "src");
    gst_pad_add_probe(encoded, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer data) {
          auto &stall = *static_cast<Stall *>(data);
          stall.entered = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(stall.milliseconds));
          return GST_PAD_PROBE_REMOVE;
        }, &stall, nullptr);
    gst_object_unref(encoded);
    push(frame++, 60);
    const auto blocked_deadline = Clock::now() + std::chrono::seconds(1);
    while (!stall.entered && Clock::now() < blocked_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(stall.entered, "test encoder never reached the blocked picture");
    const auto timeout_started = Clock::now();
    require(!media_detail::replace_mpeg2_encoder(pipeline, source, raw, pictures, encoder, profile, {}, reason) &&
            reason.find("within two seconds") != std::string::npos && encoder == before_rollback,
            "blocked encoder did not time out without changing the live element");
    const auto timeout_ms = std::chrono::duration<double, std::milli>(Clock::now() - timeout_started).count();
    require(timeout_ms >= 1900 && timeout_ms < 2500, "encoder boundary timeout exceeded its bound");
    run(60, 500);
    const auto after_timeout = run(60, 700);
    require(after_timeout["fps"].get<double>() >= 50, "removing a timed-out probe left capture blocked");
    check_errors(pipeline);
    {
      std::lock_guard<std::mutex> lock(observed.mutex);
      require(observed.gaps == 0 && observed.ssrc == 12345678, "RTP sequence or SSRC restarted");
      require(observed.checked_refresh == 3 && observed.bad_refresh == 0,
              "first replacement picture lacked an I-picture, headers or the new encoded frame rate");
    }
    std::cout << Json{{"before", before}, {"after", after}, {"bitrate_only", bitrate_only},
                     {"queued_change", queued_change}, {"rollback", rollback},
                     {"boundary_timeout_ms", timeout_ms}, {"after_timeout", after_timeout},
                     {"encoder_change_ms", change_ms}, {"rtp_continuity", true}, {"fresh_first_pictures", true}}.dump() << '\n';
    gst_element_set_state(pipeline, GST_STATE_NULL);
    for (auto *element : {source, raw, pictures, encoder_owned, packets, audio}) gst_object_unref(element);
    gst_object_unref(pipeline);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    if (pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); }
    return 1;
  }
}
