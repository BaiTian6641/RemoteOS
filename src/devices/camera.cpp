#include "remoteos/resources.hpp"
#include <cpp-httplib/httplib.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#ifdef REMOTEOS_WITH_MEDIA
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
namespace remoteos::devices {
namespace {
constexpr size_t frame_bytes=320*240*2;
GstElement* pipeline(const std::string& spec){gst_init(nullptr,nullptr);GError* error=nullptr;auto* p=gst_parse_launch(spec.c_str(),&error);if(error||!p){std::string why=error?error->message:"Camera pipeline failed";if(error)g_error_free(error);if(p)gst_object_unref(p);throw std::runtime_error(why);}if(gst_element_set_state(p,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE){gst_object_unref(p);throw std::runtime_error("Camera could not start");}return p;}
void destroy(GstElement*& p){if(p){gst_element_set_state(p,GST_STATE_NULL);gst_object_unref(p);p=nullptr;}}
void configure(httplib::Server& s){s.new_task_queue=[] {return new httplib::ThreadPool(2,2,8);};s.set_keep_alive_max_count(1);s.set_write_timeout(3);s.set_read_timeout(3);}
void cap_check(const std::string& cap){if(cap.size()<32||cap.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::invalid_argument("Invalid camera capability");}
}
struct CameraExport::Impl {
  httplib::Server server;std::thread worker;GstElement* pipe=nullptr;GstElement* sink=nullptr;std::atomic<bool> running{true};unsigned port=0;
  Impl(const std::string& cap,bool test){
    cap_check(cap);
    pipe=pipeline(std::string(test?"videotestsrc is-live=true":"v4l2src device=/dev/video0 do-timestamp=true")+" ! video/x-raw,format=YUY2,width=320,height=240,framerate=5/1 ! appsink name=frames sync=false max-buffers=1 drop=true");
    sink=gst_bin_get_by_name(GST_BIN(pipe),"frames");
    // A real camera has to deliver the negotiated mode before advertising it.
    auto* first=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),2*GST_SECOND);
    if(!first){gst_object_unref(sink);destroy(pipe);throw std::runtime_error("Webcam cannot deliver 320x240 YUYV at 5 fps");}gst_sample_unref(first);
    configure(server);
    server.Get("/"+cap+"/camera.raw",[this](const auto&,httplib::Response& response){
      response.set_chunked_content_provider("application/x-remoteos-yuy2",[this](size_t,httplib::DataSink& out){
        if(!running)return false;auto* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),GST_SECOND);if(!sample)return running.load();
        GstMapInfo map{};auto* buffer=gst_sample_get_buffer(sample);bool ok=false;
        if(gst_buffer_map(buffer,&map,GST_MAP_READ)){ok=map.size==frame_bytes&&out.write(reinterpret_cast<char*>(map.data),map.size);gst_buffer_unmap(buffer,&map);}gst_sample_unref(sample);return ok;
      });
    });
    int p=server.bind_to_any_port("127.0.0.1");if(p<1){gst_object_unref(sink);destroy(pipe);throw std::runtime_error("Camera export bind failed");}port=p;
    worker=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
  }
  ~Impl(){running=false;server.stop();if(worker.joinable())worker.join();gst_object_unref(sink);destroy(pipe);}
};
struct CameraBridge::Impl {
  httplib::Server server;httplib::Client importer;std::thread server_worker,reader;GstElement* pipe=nullptr,*source=nullptr,*sink=nullptr;
  mutable std::mutex mutex;std::condition_variable changed;std::atomic<bool> running{true};std::string jpeg,detail,cap;size_t generation=0;unsigned port;
  Impl(unsigned input,const std::string& capability):importer("127.0.0.1",input),cap(capability){
    cap_check(cap);configure(server);importer.set_read_timeout(5);importer.set_connection_timeout(5);
    pipe=pipeline("appsrc name=input is-live=true format=time do-timestamp=true block=false max-buffers=1 leaky-type=downstream caps=video/x-raw,format=YUY2,width=320,height=240,framerate=5/1 ! videoconvert ! jpegenc quality=85 ! appsink name=jpeg sync=false max-buffers=1 drop=true");
    source=gst_bin_get_by_name(GST_BIN(pipe),"input");sink=gst_bin_get_by_name(GST_BIN(pipe),"jpeg");
    server.Get("/"+cap+"/",[this](const auto&,httplib::Response& res){res.set_header("Cache-Control","no-store");res.set_content("<!doctype html><meta charset=utf-8><style>body{margin:0;background:#000;overflow:hidden}img{width:100vw;height:100vh;object-fit:contain}</style><img src='camera.mjpg'>","text/html");});
    server.Get("/"+cap+"/camera.mjpg",[this](const auto&,httplib::Response& res){
      auto cursor=std::make_shared<size_t>(0);res.set_header("Cache-Control","no-store");
      res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=remoteos-frame",[this,cursor](size_t,httplib::DataSink& out){
        std::unique_lock lock(mutex);changed.wait_for(lock,std::chrono::seconds(1),[&]{return !running||generation!=*cursor;});if(!running)return false;if(generation==*cursor)return true;
        *cursor=generation;auto bytes=jpeg;lock.unlock();auto header="--remoteos-frame\r\nContent-Type: image/jpeg\r\nContent-Length: "+std::to_string(bytes.size())+"\r\n\r\n";return out.write(header.data(),header.size())&&out.write(bytes.data(),bytes.size())&&out.write("\r\n",2);
      });
    });
    int p=server.bind_to_any_port("127.0.0.1");if(p<1){gst_object_unref(source);gst_object_unref(sink);destroy(pipe);throw std::runtime_error("Camera bridge bind failed");}port=p;
    server_worker=std::thread([this]{server.listen_after_bind();});server.wait_until_ready();
    reader=std::thread([this]{
      std::string frame;frame.reserve(frame_bytes);
      auto result=importer.Get("/"+cap+"/camera.raw",[&](const char* data,size_t n){
        while(n&&running){const auto count=std::min(n,frame_bytes-frame.size());frame.append(data,count);data+=count;n-=count;if(frame.size()!=frame_bytes)continue;
          auto* b=gst_buffer_new_allocate(nullptr,frame.size(),nullptr);gst_buffer_fill(b,0,frame.data(),frame.size());frame.clear();
          if(gst_app_src_push_buffer(GST_APP_SRC(source),b)!=GST_FLOW_OK)return false;
          auto* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),GST_SECOND);if(!sample)continue;GstMapInfo map{};auto* buffer=gst_sample_get_buffer(sample);
          if(gst_buffer_map(buffer,&map,GST_MAP_READ)){std::lock_guard lock(mutex);jpeg.assign(reinterpret_cast<char*>(map.data),map.size);++generation;gst_buffer_unmap(buffer,&map);changed.notify_all();}gst_sample_unref(sample);
        }return running.load();
      });
      std::lock_guard lock(mutex);if(running)detail="Camera stream ended; stop and restart webcam forwarding";
    });
  }
  ~Impl(){running=false;changed.notify_all();importer.stop();server.stop();if(reader.joinable())reader.join();if(server_worker.joinable())server_worker.join();gst_object_unref(source);gst_object_unref(sink);destroy(pipe);}
};
CameraExport::CameraExport(const std::string& cap,bool test):impl_(std::make_unique<Impl>(cap,test)){}CameraExport::~CameraExport()=default;unsigned CameraExport::port()const{return impl_->port;}
CameraBridge::CameraBridge(unsigned port,const std::string& cap):impl_(std::make_unique<Impl>(port,cap)){}CameraBridge::~CameraBridge()=default;
std::string CameraBridge::url()const{return "http://127.0.0.1:"+std::to_string(impl_->port)+"/"+impl_->cap+"/";}
std::string CameraBridge::error()const{std::lock_guard lock(impl_->mutex);return impl_->detail;}
}
#else
namespace remoteos::devices {
struct CameraExport::Impl{};struct CameraBridge::Impl{};
CameraExport::CameraExport(const std::string&,bool){throw std::runtime_error("Camera forwarding needs GStreamer");}CameraExport::~CameraExport()=default;unsigned CameraExport::port()const{return 0;}
CameraBridge::CameraBridge(unsigned,const std::string&){throw std::runtime_error("Camera forwarding needs GStreamer");}CameraBridge::~CameraBridge()=default;std::string CameraBridge::url()const{return {};};std::string CameraBridge::error()const{return "Media unavailable";}
}
#endif
