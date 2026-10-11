#pragma once
#include "remoteos/tunnel.hpp"
#include <asio.hpp>
#include <future>
#include <map>
#include <thread>
namespace remoteos::client {
// A resource keeps its TLS grant for the control-session lifetime. Each HTTP
// request reconnects when the previous local connection closes; no LAN listener.
class ResourceTunnels {
  struct Entry {
    std::shared_ptr<devices::TunnelClient> tunnel;
    asio::steady_timer retry;
    std::function<void()> connect;
    bool stopped=false,ready=false;unsigned failures=0;
    explicit Entry(asio::io_context& io):retry(io){}
  };
  asio::io_context io_;
  asio::executor_work_guard<asio::io_context::executor_type> work_{asio::make_work_guard(io_)};
  std::thread worker_{[this]{io_.run();}};
  std::map<std::string,std::shared_ptr<Entry>> entries_;
public:
  ~ResourceTunnels(){stop();work_.reset();io_.stop();worker_.join();}
  void open(std::string identity,std::string host,unsigned port,std::string fingerprint,std::string session,
            std::string resource,std::string token,unsigned local_port,devices::TunnelStatus status) {
    asio::post(io_,[=]{
      auto old=entries_.find(resource);if(old!=entries_.end()){old->second->stopped=true;old->second->retry.cancel();old->second->tunnel->stop();}
      auto e=std::make_shared<Entry>(io_);entries_[resource]=e;
      e->connect=[=,weak=std::weak_ptr<Entry>(e)] {
        auto item=weak.lock();if(!item||item->stopped)return;
        try {
          item->tunnel=std::make_shared<devices::TunnelClient>(io_,identity,host,port,fingerprint,session,resource,token,local_port);
          item->tunnel->start([=](bool connected,const std::string& detail){
            if(item->stopped)return;
            if(connected){item->failures=0;if(!item->ready)status(true,detail);item->ready=true;return;}
            if(++item->failures==3)status(false,"Resource TLS connection failed; check the host device port and firewall");
            item->retry.expires_after(std::chrono::milliseconds(80));
            item->retry.async_wait([item](std::error_code error){if(!error&&!item->stopped)item->connect();});
          });
        }catch(const std::exception& error){status(false,error.what());}
      };e->connect();
    });
  }
  void close(const std::string& resource){asio::post(io_,[this,resource]{auto it=entries_.find(resource);if(it==entries_.end())return;auto e=it->second;e->stopped=true;e->retry.cancel();if(e->tunnel)e->tunnel->stop();e->tunnel.reset();e->connect={};entries_.erase(it);});}
  void stop(){auto done=std::make_shared<std::promise<void>>();auto f=done->get_future();asio::post(io_,[this,done]{for(auto& [id,e]:entries_){e->stopped=true;e->retry.cancel();if(e->tunnel)e->tunnel->stop();e->tunnel.reset();e->connect={};}entries_.clear();done->set_value();});f.wait();}
};
}
