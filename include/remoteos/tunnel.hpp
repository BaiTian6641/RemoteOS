#pragma once
#include <asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace remoteos::devices {
using TunnelAuthorize=std::function<bool(const std::string& fingerprint,const std::string& session,
    const std::string& bus_id,const std::string& token)>;
using TunnelStatus=std::function<void(bool connected,const std::string& detail)>;
using TunnelClosed=std::function<void(const std::string& session,const std::string& bus_id,
    const std::string& token,const std::string& reason)>;

// All methods must run on the supplied io_context's single executor thread,
// or before/after that executor runs. No private worker threads are created.
// Loopback endpoints are trusted local importer/exporter processes; network
// peers always authenticate using the stored exact certificate fingerprint.
class TunnelServer {
public:
    TunnelServer(asio::io_context&,const std::string& identity_dir,const std::string& bind,
                 std::uint16_t port,TunnelAuthorize authorize,TunnelClosed closed={});
    ~TunnelServer();
    TunnelServer(const TunnelServer&)=delete;
    TunnelServer& operator=(const TunnelServer&)=delete;
    // At most eight prepared/active devices. Tokens must be unique, random and
    // >=32 characters. Preparation expires after 30s unless the relay starts.
    std::uint16_t prepare(const std::string& session,const std::string& bus_id,
                         const std::string& token,const std::string& peer_fingerprint, bool reusable=false);
    std::uint16_t port()const;
    // Every removed grant posts one closed callback after its sockets close,
    // including expiry/revocation. It never calls back inline. Match all three
    // identity fields before cleaning application state: a new grant for the
    // same bus may exist by the time this notification runs. The io_context
    // must keep running to dispatch callbacks; shutdown must also clean state.
    void revoke_device(const std::string& session,const std::string& bus_id);
    void revoke_session(const std::string& session);
    void stop()noexcept;
private:struct Impl;std::shared_ptr<Impl> impl_;
};

class TunnelClient {
public:
    TunnelClient(asio::io_context&,const std::string& identity_dir,const std::string& host,
                 std::uint16_t server_port,const std::string& pinned_host_fingerprint,
                 const std::string& session,const std::string& bus_id,const std::string& token,
                 std::uint16_t local_export_port);
    ~TunnelClient();
    TunnelClient(const TunnelClient&)=delete;
    TunnelClient& operator=(const TunnelClient&)=delete;
    void start(TunnelStatus status);
    void stop()noexcept;
private:struct Impl;std::shared_ptr<Impl> impl_;
};
} // namespace remoteos::devices
