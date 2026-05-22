#pragma once

#include "transport/icecast_session.h"

#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace icecast_bridge {

// Owns IcecastSessions keyed by mountpoint. Unlike Zello's ChannelPool, there
// is no idle-close: an icecast source connection stays up for the life of the
// plugin (silence keeps it warm). Listeners drop instantly if we disconnect.
class IcecastPool {
public:
    IcecastPool(asio::io_context& ioc);

    // Create + start one session per provided config. Idempotent: if a session
    // already exists for the mountpoint, the existing one is kept.
    void start_all(const std::vector<IcecastSession::Config>& mounts);

    std::shared_ptr<IcecastSession> get(const std::string& mountpoint) const;

    // Close all sessions.
    void shutdown();

private:
    asio::io_context& ioc_;
    std::unordered_map<std::string, std::shared_ptr<IcecastSession>> sessions_;
};

}  // namespace icecast_bridge