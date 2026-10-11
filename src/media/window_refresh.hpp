#pragma once
#include <gst/video/videooverlay.h>

namespace remoteos::media_detail {
inline bool refresh_video_window(GstElement* sink) {
  if (!sink || !GST_IS_VIDEO_OVERLAY(sink)) return false;
  // xvimagesink refreshes its cached X11 geometry through expose. Keep its
  // navigation events disabled: the client broker owns keyboard and pointer.
  gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  return true;
}
}
