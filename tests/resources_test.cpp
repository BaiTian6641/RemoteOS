#include "remoteos/resources.hpp"
#include "remoteos/tunnel.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/protocol.hpp"
#include "resource_tunnels.hpp"
#include <cpp-httplib/httplib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace remoteos;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){
  namespace fs=std::filesystem;const auto root=fs::temp_directory_path()/("remoteos-resources-"+random_token(8));fs::create_directories(root/"sd");
  try {
    const auto cap=random_token(32);std::ofstream(root/"sd/sample.txt")<<"shared SD contents";std::ofstream(root/"private.txt")<<"private";fs::create_symlink(root/"private.txt",root/"sd/link");
    ensure_identity((root/"host").string(),"test host");ensure_identity((root/"client").string(),"test client");
    const auto hp=certificate_fingerprint(root/"host/certificate.pem"),cp=certificate_fingerprint(root/"client/certificate.pem");
    asio::io_context io;auto guard=asio::make_work_guard(io);const auto token=random_token(32);
    devices::FileShare files((root/"sd").string(),cap,true);
    devices::TunnelServer server(io,(root/"host").string(),"127.0.0.1",0,[&](const auto& fp,const auto& session,const auto& id,const auto& secret){return fp==cp&&session=="session"&&(id=="files"||id=="camera")&&secret==token;});
    const auto imported=server.prepare("session","files",token,cp,true);std::thread executor([&]{io.run();});
    try {
      client::ResourceTunnels relays;relays.open((root/"client").string(),"127.0.0.1",server.port(),hp,"session","files",token,files.port(),[](bool,const auto&){});
      auto request=[&](const std::string& method,const std::string& path,const std::string& body=""){httplib::Client http("127.0.0.1",imported);http.set_read_timeout(5);http.set_write_timeout(5);httplib::Request r;r.method=method;r.path="/"+cap+"/"+path;r.body=body;return http.send(r);};
      for(int i=0;i<3;++i){auto r=request("GET","sample.txt");require(r&&r->status==200&&r->body=="shared SD contents","Sequential HTTP requests did not reconnect through pinned TLS");}
      auto put=request("PUT","new.txt","uploaded");require(put&&put->status==201,"WebDAV upload failed");
      auto get=request("GET","new.txt");require(get&&get->body=="uploaded","Upload contents changed");
      auto props=request("PROPFIND","");require(props&&props->status==207&&props->body.find("new.txt")!=std::string::npos,"WebDAV listing failed");
      for(const auto* path:{"link","%2e%2e/private.txt","..%2fprivate.txt"}){auto r=request("GET",path);require(r&&r->status!=200,"Shared files escaped selected SD root");}
      auto del=request("DELETE","new.txt");require(del&&del->status==204,"WebDAV deletion failed");
      {devices::FileShare ro((root/"sd").string(),cap);httplib::Client http("127.0.0.1",ro.port());auto r=http.Put("/"+cap+"/denied.txt","x","text/plain");require(r&&r->status==403&&!fs::exists(root/"sd/denied.txt"),"Read-only share accepted write");}
      {
        asio::io_context local_io;asio::ip::tcp::socket socket(local_io);socket.connect({asio::ip::address_v4::loopback(),static_cast<unsigned short>(files.port())});
        const auto request="PUT /"+cap+"/sample.txt HTTP/1.1\r\nHost: localhost\r\nContent-Length: 32\r\nConnection: close\r\n\r\nshort";
        asio::write(socket,asio::buffer(request));socket.close();std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::ifstream previous(root/"sd/sample.txt");std::string bytes((std::istreambuf_iterator<char>(previous)),{});require(bytes=="shared SD contents","Interrupted upload destroyed previous file");
      }
      const auto camera_token=random_token(32);
      // Use a separate server authorization closure for the camera fixture.
      devices::TunnelServer cs(io,(root/"host").string(),"127.0.0.1",0,[&](const auto& fp,const auto& session,const auto& id,const auto& secret){return fp==cp&&session=="session"&&id=="camera"&&secret==camera_token;});
      devices::CameraExport camera(cap,true);const auto local=cs.prepare("session","camera",camera_token,cp,true);
      relays.open((root/"client").string(),"127.0.0.1",cs.port(),hp,"session","camera",camera_token,camera.port(),[](bool,const auto&){});
      {devices::CameraBridge bridge(local,cap);auto url=bridge.url();const auto colon=url.find(':',7),slash=url.find('/',colon);httplib::Client http("127.0.0.1",std::stoi(url.substr(colon+1,slash-colon-1)));http.set_read_timeout(5);bool jpeg=false;std::string bytes;
        http.Get(url.substr(slash)+"camera.mjpg",[&](const char* data,size_t size){bytes.append(data,size);if(bytes.find(std::string("\xff\xd8",2))!=std::string::npos){jpeg=true;return false;}return bytes.size()<512*1024;});require(jpeg,"Host did not convert forwarded raw frames to MJPEG");}
      relays.stop();auto done=std::make_shared<std::promise<void>>();auto f=done->get_future();asio::post(io,[&]{cs.stop();server.stop();done->set_value();});f.wait();
    }catch(...){io.stop();executor.join();throw;}guard.reset();io.stop();executor.join();fs::remove_all(root);std::cout<<"Shared files, traversal refusal, sequential TLS reconnects and host camera conversion passed\n";
  }catch(const std::exception& e){fs::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}return 0;
}
