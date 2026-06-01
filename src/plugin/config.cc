#include "plugin/config.h"

#include <boost/log/trivial.hpp>

namespace icecast_bridge {

namespace {
constexpr const char* kTag = "\t[Icecast Bridge]\t";
}  // namespace

const std::string* PluginConfig::lookup_mount(const std::string& short_name, long tg) const {
    for (const auto& sys : systems) {
        if (sys.short_name == short_name) {
            auto it = sys.tg_to_mount.find(tg);
            if (it != sys.tg_to_mount.end()) return &it->second;
            return nullptr;
        }
    }
    return nullptr;
}

const IcecastSession::Config* PluginConfig::find_mount(const std::string& mount) const {
    auto it = mounts_by_name.find(mount);
    return it == mounts_by_name.end() ? nullptr : &it->second;
}

bool parse_plugin_config(const nlohmann::json& cfg, PluginConfig& out) {
    out.host = cfg.value("host", std::string{"localhost"});
    out.port = static_cast<uint16_t>(cfg.value("port", 8000));
    out.source_user = cfg.value("source_user", std::string{"source"});
    out.source_password = cfg.value("source_password", std::string{});

    if (out.source_password.empty()) {
        BOOST_LOG_TRIVIAL(error) << kTag << "source_password is required";
        return false;
    }

    if (!cfg.contains("mounts") || !cfg["mounts"].is_array() || cfg["mounts"].empty()) {
        BOOST_LOG_TRIVIAL(error) << kTag << "config must include a non-empty 'mounts' array";
        return false;
    }
    for (const auto& m : cfg["mounts"]) {
        IcecastSession::Config mc;
        mc.host = out.host;
        mc.port = out.port;
        mc.username = out.source_user;
        mc.password = out.source_password;
        mc.mountpoint = m.value("mount", std::string{});
        mc.display_name = m.value("name", mc.mountpoint);
        mc.description = m.value("description", std::string{});
        mc.genre = m.value("genre", std::string{"Public Safety"});
        mc.is_public = m.value("public", false);
        mc.output_sample_rate = m.value("sample_rate", 22050u);
        mc.bitrate_kbps = m.value("bitrate", 64u);
        mc.channels = m.value("channels", 1);
        mc.gain = m.value("gain", 1.0f);
        mc.legacy_source = m.value("legacy_source", false);
        mc.admin_user = m.value("admin_user", std::string{"admin"});
        mc.admin_password = m.value("admin_password", std::string{});
        mc.metadata_format  = m.value("metadata_format",
            std::string{"TG: {talkgroup_tag} ({talkgroup}) {talker_alias} {time}"});
        mc.metadata_standby = m.value("metadata_standby", std::string{"Standby"});

        if (mc.mountpoint.empty() || mc.mountpoint.front() != '/') {
            BOOST_LOG_TRIVIAL(error) << kTag
                << "every mount needs a 'mount' starting with '/' (e.g. \"/dispatch.mp3\")";
            return false;
        }
        if (out.mounts_by_name.count(mc.mountpoint)) {
            BOOST_LOG_TRIVIAL(error) << kTag
                << "duplicate mount '" << mc.mountpoint << "' in 'mounts'";
            return false;
        }

        if (mc.gain <= 0.0f) {
            BOOST_LOG_TRIVIAL(error) << kTag
                << "mount '" << mc.mountpoint << "': gain must be > 0.0";
            return false;
        }
        if (mc.gain != 1.0f) {
            BOOST_LOG_TRIVIAL(info) << kTag
                << "mount '" << mc.mountpoint << "': gain=" << mc.gain;
        }

        out.mounts_by_name[mc.mountpoint] = std::move(mc);
    }

    if (!cfg.contains("systems") || !cfg["systems"].is_array() ||
        cfg["systems"].empty()) {
        BOOST_LOG_TRIVIAL(error) << kTag
            << "config must include a non-empty 'systems' array";
        return false;
    }
    for (const auto& sys_cfg : cfg["systems"]) {
        SystemMap sys;
        sys.short_name = sys_cfg.value("shortName", "");
        if (sys.short_name.empty()) {
            BOOST_LOG_TRIVIAL(error) << kTag << "every system entry needs 'shortName'";
            return false;
        }
        if (!sys_cfg.contains("talkgroups") || !sys_cfg["talkgroups"].is_object()) {
            BOOST_LOG_TRIVIAL(error) << kTag << "system '" << sys.short_name
                << "' has no 'talkgroups' object";
            return false;
        }
        for (auto it = sys_cfg["talkgroups"].begin();
             it != sys_cfg["talkgroups"].end(); ++it) {
            long tg = 0;
            try {
                tg = std::stol(it.key());
            } catch (const std::exception&) {
                BOOST_LOG_TRIVIAL(error) << kTag << "system '" << sys.short_name
                    << "': talkgroup key '" << it.key() << "' is not a number";
                return false;
            }
            if (!it.value().is_string()) {
                BOOST_LOG_TRIVIAL(error) << kTag << "system '" << sys.short_name
                    << "': mount value for tg " << tg << " must be a string";
                return false;
            }
            std::string mount = it.value().get<std::string>();
            if (!out.mounts_by_name.count(mount)) {
                BOOST_LOG_TRIVIAL(error) << kTag << "system '" << sys.short_name
                    << "': tg " << tg << " maps to mount '" << mount
                    << "' but no such mount is declared in top-level 'mounts'";
                return false;
            }
            sys.tg_to_mount[tg] = std::move(mount);
        }
        out.systems.push_back(std::move(sys));
    }
    return true;
}

}  // namespace icecast_bridge
