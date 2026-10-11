#include "remoteos/media.hpp"
#include "window_refresh.hpp"
#include <gst/gst.h>
#include <iostream>
#include <stdexcept>

// Exercise the VideoOverlay interface without an X server, GPU or sink plugin.
// The fake is private to this test; production uses the negotiated video sink.
struct TestOverlay {
  GstElement parent;
  unsigned exposes;
};
struct TestOverlayClass { GstElementClass parent; };

static void overlay_interface_init(GstVideoOverlayInterface *iface) {
  iface->expose = +[](GstVideoOverlay *overlay) {
    ++reinterpret_cast<TestOverlay *>(overlay)->exposes;
  };
}
G_DEFINE_TYPE_WITH_CODE(TestOverlay, test_overlay, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY, overlay_interface_init))
static void test_overlay_class_init(TestOverlayClass *) {}
static void test_overlay_init(TestOverlay *overlay) { overlay->exposes = 0; }

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  try {
    require(!remoteos::media_detail::refresh_video_window(nullptr),
            "Null sink must not be refreshed");
    auto *ordinary = GST_ELEMENT(gst_bin_new(nullptr));
    require(!remoteos::media_detail::refresh_video_window(ordinary),
            "A sink without VideoOverlay must be ignored");
    gst_object_unref(ordinary);
    auto *overlay = static_cast<TestOverlay *>(g_object_new(test_overlay_get_type(), nullptr));
    require(overlay->exposes == 0, "No refresh should occur before an explicit request");
    require(remoteos::media_detail::refresh_video_window(GST_ELEMENT(overlay)),
            "VideoOverlay sink should be refreshed");
    require(overlay->exposes == 1, "One request must issue exactly one expose");
    remoteos::media_detail::refresh_video_window(GST_ELEMENT(overlay));
    require(overlay->exposes == 2, "A later layout change must issue another expose");
    gst_object_unref(overlay);
    remoteos::MediaReceiver receiver;
    receiver.refresh_windows(); // No stream or authorized drawable yet.
    receiver.stop();
    receiver.refresh_windows(); // Teardown must leave no stale window reference.
    std::cout << "VideoOverlay explicit refresh, type guards and receiver lifecycle passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
