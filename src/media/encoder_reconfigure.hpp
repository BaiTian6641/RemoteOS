#pragma once
#include "remoteos/protocol.hpp"
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

namespace remoteos::media_detail {

// appsrc supplies its own buffers. Decline pool negotiation before the raw
// queue so serialized allocation queries cannot race encoder replacement or
// wait behind a blocked picture. Caps and scheduling queries still pass.
inline GstPadProbeReturn owned_buffer_allocation_query(GstPad *, GstPadProbeInfo *info,
                                                      gpointer) {
  return GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) == GST_QUERY_ALLOCATION
      ? GST_PAD_PROBE_HANDLED : GST_PAD_PROBE_OK;
}

// An upstream queue can still contain pictures negotiated at the old rate.
// Allow their CAPS events to finish, but do not encode those pictures in the
// newly installed encoder. This drops only unencoded input, never P pictures.
// One pad-owned guard is reused across changes, including another change made
// before capture resumes; guards for different rates must not stack.
inline void await_raw_framerate(GstPad *pad, int fps) {
  constexpr auto key = "remoteos-reconfigure-framerate-guard";
  auto *wanted = static_cast<std::atomic<int> *>(g_object_get_data(G_OBJECT(pad), key));
  if (!wanted) {
    wanted = new std::atomic<int>(0);
    g_object_set_data(G_OBJECT(pad), key, wanted);
    gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
        [](GstPad *pad, GstPadProbeInfo *, gpointer data) {
          auto &wanted = *static_cast<std::atomic<int> *>(data);
          const auto expected = wanted.load();
          if (!expected) return GST_PAD_PROBE_OK;
          auto *caps = gst_pad_get_current_caps(pad);
          int numerator = 0, denominator = 0;
          const bool matches = caps && gst_structure_get_fraction(gst_caps_get_structure(caps, 0),
              "framerate", &numerator, &denominator) && denominator > 0 &&
              numerator == expected * denominator;
          if (caps) gst_caps_unref(caps);
          if (!matches) return GST_PAD_PROBE_DROP;
          wanted = 0;
          return GST_PAD_PROBE_OK;
        }, wanted, [](gpointer data) { delete static_cast<std::atomic<int> *>(data); });
  }
  *wanted = fps;
}

// gst-libav copies encoder properties into AVCodecContext only at set_format.
// Replace that one element while its upstream pad is idle and blocked; RTP,
// sockets, the pipeline clock and any other audio/video pipelines stay alive.
// The caller must serialize this with capture/profile changes and destruction.
inline bool replace_mpeg2_encoder(GstElement *pipeline, GstElement *source,
                                 GstElement *raw_filter, GstElement *pictures,
                                 GstElement *&encoder, const Display &profile,
                                 const std::function<void(GstElement *)> &installed,
                                 std::string &error) {
  error.clear();
  if (!pipeline || !source || !raw_filter || !pictures || !encoder || profile.codec != "mpeg2" ||
      profile.fps < 5 || profile.fps > 60 || profile.bitrate < 250000 || profile.bitrate > 30000000) {
    error = "Live encoder update requires an existing MPEG-2 video pipeline";
    return false;
  }
  GstCaps *source_caps = nullptr, *raw_caps = nullptr;
  g_object_get(source, "caps", &source_caps, nullptr);
  g_object_get(raw_filter, "caps", &raw_caps, nullptr);
  if (!source_caps || !raw_caps || !gst_caps_is_fixed(source_caps) || !gst_caps_is_fixed(raw_caps)) {
    if (source_caps) gst_caps_unref(source_caps);
    if (raw_caps) gst_caps_unref(raw_caps);
    error = "Live encoder update has no fixed raw-video caps";
    return false;
  }
  auto *next_source_caps = gst_caps_copy(source_caps);
  auto *next_raw_caps = gst_caps_copy(raw_caps);
  gst_caps_set_simple(next_source_caps, "framerate", GST_TYPE_FRACTION, profile.fps, 1, nullptr);
  gst_caps_set_simple(next_raw_caps, "framerate", GST_TYPE_FRACTION, profile.fps, 1, nullptr);
  auto *replacement = gst_element_factory_make("avenc_mpeg2video", nullptr);
  if (!replacement) {
    gst_caps_unref(source_caps); gst_caps_unref(raw_caps);
    gst_caps_unref(next_source_caps); gst_caps_unref(next_raw_caps);
    error = "MPEG-2 encoder factory is unavailable";
    return false;
  }
  g_object_set(replacement, "bitrate", profile.bitrate, "max-bframes", 0,
               "gop-size", std::max(1, profile.fps / 2), "thread-type", 2,
               "mbd", 2, "trellis", 0,
               "maxrate", gint64(profile.bitrate),
               "bufsize", std::max(163840, profile.bitrate / 10), "threads", 0, nullptr);
  gst_util_set_object_arg(G_OBJECT(replacement), "flags", "low_delay");
  auto *boundary = gst_element_get_static_pad(raw_filter, "src");
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool idle = false;
  };
  // The pad owns callback data until any in-flight callback has finished,
  // including the timeout/remove race. A stack Gate could outlive its scope.
  auto *gate = new Gate;
  const auto probe = gst_pad_add_probe(boundary, GST_PAD_PROBE_TYPE_IDLE,
      [](GstPad *, GstPadProbeInfo *, gpointer data) {
        auto &gate = *static_cast<Gate *>(data);
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.idle = true;
        gate.changed.notify_one();
        return GST_PAD_PROBE_OK;
      }, gate, [](gpointer data) { delete static_cast<Gate *>(data); });
  bool ready;
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    ready = gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->idle; });
  }
  bool success = false;
  if (ready) {
    auto *previous = GST_ELEMENT(gst_object_ref(encoder));
    gst_element_set_locked_state(previous, TRUE);
    // Unlink before deactivation. NULL while still linked can propagate
    // FLUSHING into an upstream queue returning from its last picture and
    // leave that queue's task stopped even after the new encoder is linked.
    gst_element_unlink(raw_filter, previous);
    gst_element_unlink(previous, pictures);
    gst_element_set_state(previous, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(pipeline), previous);
    if (gst_bin_add(GST_BIN(pipeline), replacement) &&
        gst_element_link(raw_filter, replacement) && gst_element_link(replacement, pictures)) {
      g_object_set(source, "caps", next_source_caps, nullptr);
      g_object_set(raw_filter, "caps-change-mode", 1, "caps", next_raw_caps, nullptr);
      try {
        if (installed) installed(replacement);
        success = gst_element_sync_state_with_parent(replacement);
      } catch (const std::exception &failure) {
        error = std::string("MPEG-2 encoder installation failed: ") + failure.what();
      } catch (...) {
        error = "MPEG-2 encoder installation failed";
      }
    }
    if (success) {
      encoder = replacement;
      await_raw_framerate(boundary, profile.fps);
    } else {
      // Restore a usable encoder if relinking or state negotiation failed.
      gst_element_unlink(raw_filter, replacement);
      gst_element_unlink(replacement, pictures);
      gst_element_set_state(replacement, GST_STATE_NULL);
      if (GST_OBJECT_PARENT(replacement) == GST_OBJECT(pipeline))
        gst_bin_remove(GST_BIN(pipeline), replacement);
      else gst_object_unref(replacement);
      gst_bin_add(GST_BIN(pipeline), previous);
      gst_element_link(raw_filter, previous); gst_element_link(previous, pictures);
      g_object_set(source, "caps", source_caps, nullptr);
      g_object_set(raw_filter, "caps", raw_caps, nullptr);
      gst_element_set_locked_state(previous, FALSE);
      gst_element_sync_state_with_parent(previous);
      int numerator = 0, denominator = 0;
      if (gst_structure_get_fraction(gst_caps_get_structure(source_caps, 0), "framerate",
                                     &numerator, &denominator) && denominator == 1)
        await_raw_framerate(boundary, numerator);
      if (error.empty()) error = "Could not replace the MPEG-2 encoder; previous profile restored";
    }
    gst_object_unref(previous);
  } else {
    gst_object_unref(replacement);
    error = "MPEG-2 encoder did not reach a complete-picture boundary within two seconds";
  }
  if (probe) gst_pad_remove_probe(boundary, probe);
  gst_object_unref(boundary);
  gst_caps_unref(source_caps); gst_caps_unref(raw_caps);
  gst_caps_unref(next_source_caps); gst_caps_unref(next_raw_caps);
  return success;
}

} // namespace remoteos::media_detail
