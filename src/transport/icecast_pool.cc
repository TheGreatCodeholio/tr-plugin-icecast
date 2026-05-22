#include "transport/icecast_pool.h"

namespace icecast_bridge {

IcecastPool::IcecastPool(asio::io_context& ioc) : ioc_(ioc) {}

void IcecastPool::start_all(const std::vector<IcecastSession::Config>& mounts) {
    for (const auto& cfg : mounts) {
        if (sessions_.count(cfg.mountpoint)) continue;
        auto sess = std::make_shared<IcecastSession>(ioc_, cfg);
        sessions_[cfg.mountpoint] = sess;
        sess->start();
    }
}

std::shared_ptr<IcecastSession> IcecastPool::get(const std::string& mountpoint) const {
    auto it = sessions_.find(mountpoint);
    return it == sessions_.end() ? nullptr : it->second;
}

void IcecastPool::shutdown() {
    for (auto& [_, sess] : sessions_) sess->close();
    sessions_.clear();
}

}  // namespace icecast_bridge