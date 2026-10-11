#include "remoteos/tunnel.hpp"
#include "remoteos/devices.hpp"
#include "remoteos/protocol.hpp"
#include "../core/transport.hpp"
#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace remoteos::devices {
namespace {
using tcp=asio::ip::tcp;
using Error=std::error_code;
constexpr std::size_t maximum_header=2048;
constexpr std::size_t maximum_devices=8;
constexpr unsigned rate_limit=4*1024*1024;
void cancel_timer(asio::steady_timer& timer)noexcept{try{timer.cancel();}catch(...){}}

std::string pin(const std::string& value) {
    auto result=normalize_fingerprint(value);
    if(result.size()!=64)throw std::invalid_argument("SHA-256 certificate pin is required");
    return result;
}
bool resource_id(const std::string& bus) { return bus=="camera" || bus=="files"; }
unsigned resource_rate(const std::string& bus) { return bus=="files" ? 512*1024 : bus=="camera" ? 1024*1024 : rate_limit; }
void validate(const std::string& session,const std::string& bus,const std::string& token) {
    if(session.empty()||session.size()>128||(!valid_bus_id(bus)&&!resource_id(bus))||token.size()<32||token.size()>128)
        throw std::invalid_argument("Invalid device-tunnel grant");
}

struct Pipe : std::enable_shared_from_this<Pipe> {
    asio::ssl::stream<tcp::socket> tls;
    tcp::socket local;
    asio::steady_timer deadline,up_pace,down_pace;
    std::array<unsigned char,4> header{};
    std::string body;
    std::array<char,16384> incoming{},outgoing{};
    bool closed=false,started=false;
    unsigned rate=rate_limit;
    std::function<void(std::string)> on_close;
    explicit Pipe(asio::io_context& io,asio::ssl::context& ctx):tls(io,ctx),local(io),deadline(io),up_pace(io),down_pace(io){}
    void timeout(unsigned seconds) {
        deadline.expires_after(std::chrono::seconds(seconds));auto self=shared_from_this();
        deadline.async_wait([self](Error error){if(!error)self->close("Device tunnel setup timed out");});
    }
    std::string peer() {
        X509* certificate=SSL_get1_peer_certificate(tls.native_handle());
        if(!certificate)throw std::runtime_error("Device tunnel peer certificate is missing");
        std::string value;
        try{value=net::fingerprint(certificate);}catch(...){X509_free(certificate);throw;}
        X509_free(certificate);return value;
    }
    void close(const std::string& why)noexcept {
        if(closed)return;
        closed=true;Error ignored;
        cancel_timer(deadline);cancel_timer(up_pace);cancel_timer(down_pace);
        tls.next_layer().cancel(ignored);tls.next_layer().close(ignored);
        local.cancel(ignored);local.close(ignored);
        if(on_close){try{on_close(why);}catch(...){}}
    }
    void read_header(std::function<void(Json)> complete) {
        auto self=shared_from_this();
        asio::async_read(tls,asio::buffer(header),[self,complete](Error error,std::size_t){
            if(error){self->close(error.message());return;}
            std::size_t count=0;for(auto byte:self->header)count=(count<<8)|byte;
            if(!count||count>maximum_header){self->close("Device tunnel header is too large");return;}
            self->body.resize(count);
            asio::async_read(self->tls,asio::buffer(self->body),[self,complete](Error error,std::size_t){
                if(error){self->close(error.message());return;}
                try {
                    auto json=Json::parse(self->body,[](int depth,Json::parse_event_t,Json&){
                        if(depth>4)throw std::invalid_argument("Device tunnel header nesting limit");
                        return true;
                    });
                    if(!json.is_object()||json.value("version",0)!=1)throw std::invalid_argument("Device tunnel protocol version mismatch");
                    complete(std::move(json));
                }catch(const std::exception& e){self->close(e.what());}
            });
        });
    }
    void write_header(Json message,std::function<void()> complete) {
        const auto payload=message.dump();auto bytes=std::make_shared<std::string>(4,'\0');
        std::size_t count=payload.size();
        for(int i=3;i>=0;--i){(*bytes)[i]=static_cast<char>(count&255);count>>=8;}
        *bytes+=payload;auto self=shared_from_this();
        asio::async_write(tls,asio::buffer(*bytes),[self,bytes,complete](Error error,std::size_t){
            if(error){self->close(error.message());return;}complete();
        });
    }
    void start() {
        if(closed||started)return;
        started=true;Error ignored;cancel_timer(deadline);
        tls.next_layer().set_option(tcp::no_delay(true),ignored);local.set_option(tcp::no_delay(true),ignored);
        from_remote();from_local();
    }
    void from_remote() {
        if(closed)return;
        auto self=shared_from_this();
        tls.async_read_some(asio::buffer(incoming),[self](Error error,std::size_t count){
            if(error){self->close(error.message());return;}
            asio::async_write(self->local,asio::buffer(self->incoming.data(),count),[self,count](Error error,std::size_t){
                if(error){self->close(error.message());return;}
                self->down_pace.expires_after(std::chrono::microseconds(count*1000000ULL/self->rate));
                self->down_pace.async_wait([self](Error error){if(!error)self->from_remote();});
            });
        });
    }
    void from_local() {
        if(closed)return;
        auto self=shared_from_this();
        local.async_read_some(asio::buffer(outgoing),[self](Error error,std::size_t count){
            if(error){self->close(error.message());return;}
            asio::async_write(self->tls,asio::buffer(self->outgoing.data(),count),[self,count](Error error,std::size_t){
                if(error){self->close(error.message());return;}
                self->up_pace.expires_after(std::chrono::microseconds(count*1000000ULL/self->rate));
                self->up_pace.async_wait([self](Error error){if(!error)self->from_local();});
            });
        });
    }
};

struct Grant {
    std::string session,bus,token,peer_pin;
    tcp::acceptor importer;
    tcp::socket socket;
    asio::steady_timer expiry;
    std::shared_ptr<Pipe> pipe;
    bool local_ready=false,authenticated=false,started=false,reusable=false;
    explicit Grant(asio::io_context& io):importer(io),socket(io),expiry(io){}
};
}

struct TunnelServer::Impl:std::enable_shared_from_this<TunnelServer::Impl> {
    asio::io_context& io;
    asio::ssl::context context{asio::ssl::context::tls_server};
    tcp::acceptor acceptor;
    TunnelAuthorize authorize;
    TunnelClosed closed;
    std::map<std::string,std::shared_ptr<Grant>> grants;
    std::set<std::shared_ptr<Pipe>> pending;
    bool stopped=false;
    Impl(asio::io_context& loop,const std::string& identity,const std::string& bind,std::uint16_t port,TunnelAuthorize authorization,TunnelClosed notification)
        :io(loop),acceptor(loop),authorize(std::move(authorization)),closed(std::move(notification)) {
        if(!authorize)throw std::invalid_argument("Device tunnel authorization callback is required");
        net::configure(context,identity);
        tcp::endpoint endpoint(asio::ip::make_address(bind),port);
        acceptor.open(endpoint.protocol());acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);acceptor.listen(8);
    }
    void accept() {
        if(stopped)return;
        auto stream=std::make_shared<Pipe>(io,context);auto self=shared_from_this();
        acceptor.async_accept(stream->tls.next_layer(),[self,stream](Error error){
            if(self->stopped)return;
            if(!error) {
                if(self->pending.size()>=maximum_devices)stream->close("Too many pending tunnel handshakes");
                else {
                    self->pending.insert(stream);
                    stream->on_close=[weak=std::weak_ptr<Impl>(self),weak_pipe=std::weak_ptr<Pipe>(stream)](const std::string&){
                        if(auto owner=weak.lock())if(auto pipe=weak_pipe.lock())owner->pending.erase(pipe);
                    };
                    stream->timeout(5);
                    stream->tls.async_handshake(asio::ssl::stream_base::server,[self,stream](Error error){
                        if(error){stream->close("Device TLS handshake failed");return;}
                        stream->read_header([self,stream](Json message){self->authenticate(stream,message);});
                    });
                }
            }
            self->accept();
        });
    }
    void authenticate(const std::shared_ptr<Pipe>& stream,const Json& message) {
        if(stopped||stream->closed)return;
        if(message.value("type","")!="open_tunnel")throw std::invalid_argument("Expected open_tunnel request");
        const auto session=message.at("session_id").get<std::string>();
        const auto bus=message.at("bus_id").get<std::string>();
        const auto token=message.at("token").get<std::string>();
        validate(session,bus,token);const auto peer=stream->peer();
        const auto found=grants.find(token);
        if(found==grants.end())throw std::runtime_error("Unknown, revoked or expired tunnel token");
        const auto& grant=found->second;
        if(grant->pipe||grant->session!=session||grant->bus!=bus||grant->peer_pin!=peer||!authorize(peer,session,bus,token))
            throw std::runtime_error("Device tunnel authorization denied");
        grant->pipe=stream;pending.erase(stream);
        stream->rate=resource_rate(bus);
        if (!grant->reusable) stream->timeout(30); else cancel_timer(stream->deadline);
        stream->on_close=[weak=std::weak_ptr<Impl>(shared_from_this()),token](const std::string& reason){if(auto owner=weak.lock())owner->pipe_closed(token,reason);};
        auto self=shared_from_this();
        stream->write_header(Json{{"version",1},{"type","tunnel_ready"}},[self,grant]{
            grant->authenticated=true;self->bridge(grant);
        });
    }
    void bridge(const std::shared_ptr<Grant>& grant) {
        if(stopped||grant->started||!grant->local_ready||!grant->authenticated||grant->pipe->closed)return;
        grant->started=true;cancel_timer(grant->expiry);
        grant->pipe->local=std::move(grant->socket);grant->pipe->start();
    }
    void accept_importer(const std::shared_ptr<Grant>& grant) {
        auto self=shared_from_this();
        grant->importer.async_accept(grant->socket,[self,grant](Error error){
            if(error) { if(self->grants.count(grant->token)) self->remove(grant->token,"Importer connection failed"); return; }
            if(!grant->reusable){Error ignored;grant->importer.close(ignored);}
            grant->local_ready=true;self->bridge(grant);
        });
    }
    void pipe_closed(const std::string& token,const std::string& reason) {
        const auto found=grants.find(token);if(found==grants.end())return;
        auto grant=found->second;
        if(!grant->reusable || !grant->started || stopped){remove(token,reason);return;}
        // HTTP resources use sequential connections under a session-scoped grant.
        // Reset on the executor after the closing Pipe callback has returned.
        asio::post(io,[self=shared_from_this(),grant]{
            if(!self->grants.count(grant->token)||self->stopped)return;
            grant->pipe.reset();grant->started=grant->authenticated=grant->local_ready=false;
            grant->socket=tcp::socket(self->io);self->accept_importer(grant);
        });
    }
    void remove(const std::string& token,const std::string& reason="Device tunnel grant revoked")noexcept {
        const auto found=grants.find(token);if(found==grants.end())return;
        auto grant=found->second;grants.erase(found);Error ignored;
        cancel_timer(grant->expiry);grant->importer.close(ignored);grant->socket.close(ignored);
        if(grant->pipe)grant->pipe->close(reason);
        if(closed) {
            // Erase and close before notifying; an application may revoke its
            // other grants or create a replacement from inside the callback.
            try {
                asio::post(io,[callback=closed,session=grant->session,bus=grant->bus,token=grant->token,reason]{
                    callback(session,bus,token,reason);
                });
            }catch(...){}
        }
    }
    std::uint16_t prepare(const std::string& session,const std::string& bus,const std::string& token,const std::string& peer,bool reusable) {
        validate(session,bus,token);
        if(stopped||grants.size()>=maximum_devices||grants.count(token))throw std::runtime_error("Device tunnel grant unavailable");
        auto grant=std::make_shared<Grant>(io);
        grant->reusable=reusable;
        if(reusable&&!resource_id(bus))throw std::invalid_argument("Only userspace resources can reconnect");
        grant->session=session;grant->bus=bus;grant->token=token;grant->peer_pin=pin(peer);
        tcp::endpoint endpoint(asio::ip::address_v4::loopback(),0);
        grant->importer.open(tcp::v4());grant->importer.bind(endpoint);grant->importer.listen(reusable?16:1);
        const auto port=grant->importer.local_endpoint().port();
        grants[token]=grant;auto self=shared_from_this();
        grant->expiry.expires_after(std::chrono::seconds(30));
        grant->expiry.async_wait([self,token](Error error){if(!error)self->remove(token,"Device tunnel preparation expired");});
        accept_importer(grant);
        return port;
    }
    void stop()noexcept {
        if(stopped)return;
        stopped=true;Error ignored;acceptor.close(ignored);
        while(!grants.empty())remove(grants.begin()->first,"Device tunnel server stopped");
        auto peers=pending;for(auto& peer:peers)peer->close("Device tunnel server stopped");pending.clear();
    }
};

struct TunnelClient::Impl:std::enable_shared_from_this<TunnelClient::Impl> {
    asio::ssl::context context{asio::ssl::context::tls_client};
    tcp::resolver resolver;
    std::shared_ptr<Pipe> pipe;
    std::string host,host_pin,session,bus,token;
    std::uint16_t port,export_port;
    TunnelStatus status;
    bool started=false;
    Impl(asio::io_context& io,const std::string& identity,const std::string& hostname,std::uint16_t server_port,
         const std::string& fingerprint,const std::string& session_id,const std::string& bus_id,const std::string& secret,std::uint16_t local_port)
        :resolver(io),host(hostname),host_pin(pin(fingerprint)),session(session_id),bus(bus_id),token(secret),port(server_port),export_port(local_port) {
        validate(session,bus,token);if(!port||!export_port||host.empty())throw std::invalid_argument("Device tunnel ports and hostname are required");
        net::configure(context,identity);pipe=std::make_shared<Pipe>(io,context);pipe->rate=resource_rate(bus);
    }
    void start(TunnelStatus callback) {
        if(started||pipe->closed)throw std::runtime_error("Device tunnel client already started or stopped");
        started=true;status=std::move(callback);
        auto self=shared_from_this();
        pipe->on_close=[weak=std::weak_ptr<Impl>(self)](const std::string& why){
            if(auto owner=weak.lock()){owner->resolver.cancel();if(owner->status)owner->status(false,why);}
        };
        pipe->timeout(10);
        resolver.async_resolve(host,std::to_string(port),[self](Error error,tcp::resolver::results_type endpoints){
            if(self->pipe->closed)return;
            if(error){self->pipe->close(error.message());return;}
            asio::async_connect(self->pipe->tls.next_layer(),endpoints,[self](Error error,const tcp::endpoint&){
                if(self->pipe->closed)return;
                if(error){self->pipe->close(error.message());return;}
                self->pipe->tls.async_handshake(asio::ssl::stream_base::client,[self](Error error){
                    if(self->pipe->closed)return;
                    if(error){self->pipe->close("Device TLS handshake failed");return;}
                    try{if(self->pipe->peer()!=self->host_pin)throw std::runtime_error("Device tunnel host certificate changed");}
                    catch(const std::exception& e){self->pipe->close(e.what());return;}
                    self->pipe->write_header(Json{{"version",1},{"type","open_tunnel"},{"session_id",self->session},{"bus_id",self->bus},{"token",self->token}},[self]{
                        if(self->pipe->closed)return;
                        self->pipe->read_header([self](Json response){
                            if(self->pipe->closed)return;
                            if(response.value("type","")!="tunnel_ready")throw std::runtime_error("Device tunnel was not authorized");
                            self->pipe->local.async_connect(tcp::endpoint(asio::ip::address_v4::loopback(),self->export_port),[self](Error error){
                                if(self->pipe->closed)return;
                                if(error){self->pipe->close("Local USB exporter unavailable: "+error.message());return;}
                                self->pipe->start();
                                try{if(self->status)self->status(true,"Device tunnel connected");}
                                catch(const std::exception& e){self->pipe->close(e.what());}
                            });
                        });
                    });
                });
            });
        });
    }
    void stop()noexcept{resolver.cancel();pipe->close("Device tunnel client stopped");}
};

TunnelServer::TunnelServer(asio::io_context& io,const std::string& identity,const std::string& bind,std::uint16_t port,TunnelAuthorize authorize,TunnelClosed closed)
    :impl_(std::make_shared<Impl>(io,identity,bind,port,std::move(authorize),std::move(closed))){impl_->accept();}
TunnelServer::~TunnelServer(){stop();}
std::uint16_t TunnelServer::prepare(const std::string& session,const std::string& bus,const std::string& token,const std::string& peer,bool reusable){return impl_->prepare(session,bus,token,peer,reusable);}
std::uint16_t TunnelServer::port()const{return impl_->acceptor.local_endpoint().port();}
void TunnelServer::revoke_device(const std::string& session,const std::string& bus){std::vector<std::string> tokens;for(auto& entry:impl_->grants)if(entry.second->session==session&&entry.second->bus==bus)tokens.push_back(entry.first);for(auto& token:tokens)impl_->remove(token);}
void TunnelServer::revoke_session(const std::string& session){std::vector<std::string> tokens;for(auto& entry:impl_->grants)if(entry.second->session==session)tokens.push_back(entry.first);for(auto& token:tokens)impl_->remove(token);}
void TunnelServer::stop()noexcept{if(impl_)impl_->stop();}
TunnelClient::TunnelClient(asio::io_context& io,const std::string& identity,const std::string& host,std::uint16_t port,const std::string& pin,const std::string& session,const std::string& bus,const std::string& token,std::uint16_t local)
    :impl_(std::make_shared<Impl>(io,identity,host,port,pin,session,bus,token,local)){}
TunnelClient::~TunnelClient(){stop();}
void TunnelClient::start(TunnelStatus status){impl_->start(std::move(status));}
void TunnelClient::stop()noexcept{if(impl_)impl_->stop();}
} // namespace remoteos::devices
