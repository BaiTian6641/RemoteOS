#include "remoteos/protocol.hpp"
#include "portable_runtime.hpp"
#include <gst/gst.h>
#include <openssl/ssl.h>
#include <iostream>
#include <stdexcept>

// Runs generated media only: no desktop, camera, input, listener or permissions.
// Distributed with portable packages so CI can test the relocated runtime.
int main() {
  remoteos::app::portable_runtime();
  gst_init(nullptr,nullptr);
  try {
    remoteos::Json factories=remoteos::Json::array();
    for(const auto* name:{"avenc_mpeg2video","avdec_mpeg2video","mpegvideoparse","videoconvert",
      "appsrc","appsink","rtpbin","rtpgstpay","rtpgstdepay","rtpmpvpay","rtpmpvdepay",
      "udpsrc","udpsink","opusenc","opusdec","rtpopuspay","rtpopusdepay","dtlssrtpenc","dtlssrtpdec","jpegenc"}) {
      auto* factory=gst_element_factory_find(name);
      if(!factory)throw std::runtime_error(std::string("Missing bundled media element: ")+name);
      gst_object_unref(factory);factories.push_back(name);
    }
    GError* error=nullptr;
    auto* pipeline=gst_parse_launch("videotestsrc num-buffers=12 ! video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! avenc_mpeg2video max-bframes=0 ! mpegvideoparse ! avdec_mpeg2video ! fakesink sync=false",&error);
    if(error||!pipeline){std::string why=error?error->message:"Cannot create MPEG-2 test";if(error)g_error_free(error);if(pipeline)gst_object_unref(pipeline);throw std::runtime_error(why);}
    auto* bus=gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline,GST_STATE_PLAYING);
    auto* result=gst_bus_timed_pop_filtered(bus,15*GST_SECOND,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
    std::string failure;
    if(!result)failure="Bundled MPEG-2 runtime test timed out";
    else if(GST_MESSAGE_TYPE(result)==GST_MESSAGE_ERROR){gchar* debug=nullptr;gst_message_parse_error(result,&error,&debug);failure=error->message;g_error_free(error);g_free(debug);}
    if(result)gst_message_unref(result);gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(bus);gst_object_unref(pipeline);
    if(!failure.empty())throw std::runtime_error(failure);
    auto* tls=SSL_CTX_new(TLS_method());if(!tls)throw std::runtime_error("Bundled OpenSSL initialization failed");
    const bool tls13=SSL_CTX_set_min_proto_version(tls,TLS1_3_VERSION)==1;SSL_CTX_free(tls);
    if(!tls13)throw std::runtime_error("Bundled OpenSSL cannot configure TLS 1.3");
    std::cout<<remoteos::Json{{"status","passed"},{"gstreamer",gst_version_string()},{"openssl",OpenSSL_version(OPENSSL_VERSION)},
      {"mpeg2_generated_frames",12},{"factories",factories},{"physical_devices_used",false}}.dump(2)<<'\n';return 0;
  }catch(const std::exception& e){std::cerr<<"RemoteOS runtime check: "<<e.what()<<'\n';return 1;}
}
