#pragma once

#include "transport/icecast_session.h"

#include <json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace icecast_bridge {

struct SystemMap {
    std::string short_name;
    // talkgroup id -> mountpoint string. Multiple talkgroups MAY map to the
    // same mountpoint — that's the whole point of this plugin (FIFO-merge
    // several talkgroups into one listener-facing stream).
    std::unordered_map<long, std::string> tg_to_mount;
};

struct PluginConfig {
    // Icecast server connection
    std::string host = "localhost";
    uint16_t port = 8000;
    std::string source_user = "source";
    std::string source_password;

    // Per-mountpoint stream definitions (advertised to icecast on connect).
    // The key in mounts_by_name is the mountpoint string (e.g. "/dispatch.mp3").
    std::unordered_map<std::string, IcecastSession::Config> mounts_by_name;

    std::vector<SystemMap> systems;

    // Returns nullptr if (short_name, tg) is not mapped.
    const std::string* lookup_mount(const std::string& short_name, long tg) const;

    // Returns nullptr if `mount` is not declared in mounts_by_name.
    const IcecastSession::Config* find_mount(const std::string& mount) const;
};

// Returns true on success. On failure logs the reason via Boost.Log and
// leaves `out` in an indeterminate state.
bool parse_plugin_config(const nlohmann::json& cfg, PluginConfig& out);

}  // namespace icecast_bridge