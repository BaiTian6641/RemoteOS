#include "remoteos/tunnel.hpp"
#include "remoteos/identity.hpp"
#include "remoteos/protocol.hpp"
#include <asio.hpp>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <map>
#include <stdexcept>

using asio::ip::tcp;
using remoteos::devices::TunnelClient;
using remoteos::devices::TunnelServer;
namespace fs=std::filesystem;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Echo {
    tcp::acceptor acceptor;
    tcp::socket socket;
    std::array<char,16384> buffer{};
    unsigned connections=0;
    explicit Echo(asio::io_context& io):acceptor(io,tcp::endpoint(asio::ip::address_v4::loopback(),0)),socket(io){
        acceptor.async_accept(socket,[this](std::error_code error){if(!error){++connections;read();}});
    }
    void read(){socket.async_read_some(asio::buffer(buffer),[this](std::error_code error,std::size_t count){
        if(!error)asio::async_write(socket,asio::buffer(buffer.data(),count),[this](std::error_code error,std::size_t){if(!error)read();});
    });}
    std::uint16_t port()const{return acceptor.local_endpoint().port();}
};
struct Identities {
    fs::path root;
    std::string server,client,other,server_pin,client_pin;
    Identities(){
        root=fs::temp_directory_path()/ ("remoteos-tunnel-test-"+remoteos::random_token(8));
        server=(root/"server").string();client=(root/"client").string();other=(root/"other").string();
        remoteos::ensure_identity(server,"tunnel test server");
        remoteos::ensure_identity(client,"tunnel test client");
        remoteos::ensure_identity(other,"tunnel test stranger");
        server_pin=remoteos::certificate_fingerprint(fs::path(server)/"certificate.pem");
        client_pin=remoteos::certificate_fingerprint(fs::path(client)/"certificate.pem");
    }
    ~Identities(){std::error_code error;fs::remove_all(root,error);}
};
void roundtrip_and_revoke(const Identities& ids,bool abrupt=false){
    asio::io_context io;Echo echo(io);std::string failure;bool echoed=false,revoked=false,connected=false;unsigned notices=0;
    const auto token=remoteos::random_token();
    TunnelServer server(io,ids.server,"127.0.0.1",0,[&](const auto& fp,const auto& session,const auto& bus,const auto& secret){
        return fp==ids.client_pin&&session=="test-session"&&bus=="1-2"&&secret==token;
    },[&](const auto& session,const auto& bus,const auto& secret,const auto& reason){
        require(session=="test-session"&&bus=="1-2"&&secret==token&&!reason.empty(),"incorrect grant-close identity");
        ++notices;
        server.revoke_device(session,bus); // Removed already: no recursive notification.
        if(revoked)io.stop();
    });
    const auto local=server.prepare("test-session","1-2",token,ids.client_pin);
    bool duplicate_rejected=false;
    try{server.prepare("test-session","1-2",token,ids.client_pin);}catch(...){duplicate_rejected=true;}
    require(duplicate_rejected,"duplicate grant accepted");
    TunnelClient client(io,ids.client,"127.0.0.1",server.port(),ids.server_pin,"test-session","1-2",token,echo.port());
    tcp::socket importer(io);
    std::string payload(192*1024,'\0');for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<char>(i%251);
    std::string received(payload.size(),'\0');
    importer.async_connect(tcp::endpoint(asio::ip::address_v4::loopback(),local),[&](std::error_code error){
        if(error){failure=error.message();io.stop();return;}
        asio::async_write(importer,asio::buffer(payload),[&](std::error_code error,std::size_t){
            if(error){failure=error.message();io.stop();}
        });
        asio::async_read(importer,asio::buffer(received),[&](std::error_code error,std::size_t){
            if(error){failure=error.message();io.stop();return;}
            echoed=received==payload;
            if(abrupt){std::error_code ignored;importer.close(ignored);}
            else server.revoke_session("test-session");
        });
    });
    client.start([&](bool up,const std::string& detail){
        if(up)connected=true;
        else if(echoed){revoked=true;if(notices)io.stop();}
        else {failure=detail;io.stop();}
    });
    io.run_for(std::chrono::seconds(5));
    server.stop();client.stop();
    if(!failure.empty())throw std::runtime_error(failure);
    require(connected&&echoed&&revoked,"authenticated relay/teardown did not finish");
    require(echo.connections==1,"unexpected exporter connection count");
    require(notices==1,"grant close was not notified exactly once");
}
void reject(const Identities& ids,int kind){
    asio::io_context io;Echo echo(io);bool denied=false,reported=false;std::string detail;
    const auto token=remoteos::random_token();
    TunnelServer server(io,ids.server,"127.0.0.1",0,[&](const auto&,const auto&,const auto&,const auto&){return kind!=3;});
    server.prepare("test-session","1-2",token,ids.client_pin);
    TunnelClient client(io,kind==1?ids.other:ids.client,"127.0.0.1",server.port(),kind==0?std::string(64,'0'):ids.server_pin,
                        "test-session","1-2",kind==2?remoteos::random_token():token,echo.port());
    client.start([&](bool up,const std::string& why){if(!reported){reported=true;denied=!up;detail=why;io.stop();}});
    io.run_for(std::chrono::seconds(5));
    server.stop();client.stop();
    require(denied,"unauthorized tunnel accepted");
    require(echo.connections==0,"exporter contacted before authorization");
    if(kind==0)require(detail.find("certificate changed")!=std::string::npos,"host pin failure not reported");
}
void revoke_before_connect(const Identities& ids){
    asio::io_context io;Echo echo(io);bool denied=false;const auto token=remoteos::random_token();
    TunnelServer server(io,ids.server,"127.0.0.1",0,[](const auto&,const auto&,const auto&,const auto&){return true;});
    server.prepare("test-session","1-2",token,ids.client_pin);server.revoke_session("test-session");
    TunnelClient client(io,ids.client,"127.0.0.1",server.port(),ids.server_pin,"test-session","1-2",token,echo.port());
    client.start([&](bool up,const std::string&){denied=!up;io.stop();});io.run_for(std::chrono::seconds(5));
    server.stop();client.stop();require(denied&&echo.connections==0,"revoked grant accepted");
}
void limits(const Identities& ids){
    asio::io_context io;
    TunnelServer server(io,ids.server,"127.0.0.1",0,[](const auto&,const auto&,const auto&,const auto&){return true;});
    for(unsigned i=0;i<8;++i)server.prepare("test-session","1-2",remoteos::random_token(),ids.client_pin);
    bool rejected=false;try{server.prepare("test-session","1-2",remoteos::random_token(),ids.client_pin);}catch(...){rejected=true;}
    require(rejected,"pending device limit missing");
    server.revoke_session("test-session");
    require(server.prepare("another-session","1-2",remoteos::random_token(),ids.client_pin)>0,"revocation did not free grants");
    server.revoke_device("another-session","1-2");
    for(unsigned i=0;i<8;++i)server.prepare("replacement-session","1-2",remoteos::random_token(),ids.client_pin);
    server.stop();
}
void close_notification_order(const Identities& ids){
    asio::io_context io;std::map<std::string,unsigned> notices;
    const auto old_token=remoteos::random_token(),new_token=remoteos::random_token();
    TunnelServer server(io,ids.server,"127.0.0.1",0,[](const auto&,const auto&,const auto&,const auto&){return true;},
        [&](const auto& session,const auto& bus,const auto& token,const auto& reason){
            require(session=="test-session"&&bus=="1-2"&&!reason.empty(),"incorrect queued grant identity");
            ++notices[token];
        });
    server.prepare("test-session","1-2",old_token,ids.client_pin);
    server.revoke_device("test-session","1-2");
    require(notices.empty(),"grant notification ran inline during revocation");
    server.prepare("test-session","1-2",new_token,ids.client_pin);
    io.poll();
    require(notices[old_token]==1&&notices[new_token]==0,"replacement grant confused with old close");
    server.stop();
    io.restart();io.poll();
    require(notices[old_token]==1&&notices[new_token]==1,"stop callback missing or repeated");
}
void expiration_notification(const Identities& ids){
    asio::io_context io;unsigned notices=0;
    const auto token=remoteos::random_token();
    TunnelServer server(io,ids.server,"127.0.0.1",0,[](const auto&,const auto&,const auto&,const auto&){return true;},
        [&](const auto& session,const auto& bus,const auto& secret,const auto& reason){
            require(session=="test-session"&&bus=="1-2"&&secret==token,"expired grant identity changed");
            require(reason.find("expired")!=std::string::npos,"grant expiration reason missing");
            ++notices;io.stop();
        });
    server.prepare("test-session","1-2",token,ids.client_pin);
    io.run_for(std::chrono::seconds(32));
    require(notices==1,"prepared grant expiration did not notify cleanup");
    server.stop();io.restart();io.poll();
    require(notices==1,"expired grant notified twice during shutdown");
}
}
int main(){
    try {
        const Identities identities;
        roundtrip_and_revoke(identities);
        roundtrip_and_revoke(identities,true);
        for(int kind=0;kind<4;++kind)reject(identities,kind);
        revoke_before_connect(identities);limits(identities);
        close_notification_order(identities);expiration_notification(identities);
        std::cout<<"Device tunnel tests passed: TLS echo, pinned identities, tokens, authorization, revoke, limits and per-grant cleanup notifications\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
