#include "codec/mp3_encoder.h"
#include "codec/resampler.h"
#include "pipeline/call_state.h"
#include "plugin/config.h"
#include "transport/icecast_pool.h"
#include "transport/icecast_session.h"

#include "trunk-recorder/plugin_manager/plugin_api.h"
#include "trunk-recorder/recorders/recorder.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/dll/alias.hpp>
#include <boost/log/trivial.hpp>
#include <boost/shared_ptr.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace icecast_bridge {

namespace {
constexpr const char* kPluginTag = "\t[Icecast Bridge]\t";

// If an active call goes this long with no PCM and no call_end, the pacer
// treats it as concluded. Without this, a call for which trunk-recorder skips
// call_end (short / encrypted / sub-minDuration calls) would hold the front of
// its mountpoint queue forever and block every talkgroup behind it.
constexpr int kStaleAfterMs = 5000;

// Ghost-cleanup threshold. The stale watchdog above only runs once a call has
// been enqueued (i.e. produced audio). For squelch-flutter ghosts (transmission
// length ~ms) trunk-recorder skips BOTH audio_stream and call_end, so the call
// is never enqueued and its registry entry would leak for the life of the
// process. If a call hasn't been enqueued this many seconds after call_start,
// it's presumed a ghost and dropped. Real calls produce audio within ~100ms.
constexpr int kGhostCleanupSeconds = 10;

inline std::string call_hdr(const CallState& cs) {
    return log_header(cs.short_name, cs.call_num, cs.talkgroup_display, cs.freq) +
           "Icecast Bridge - ";
}

// Render a mount's title_template against a call's cached fields. Substitutes
// ${TOKEN} (case-insensitive); unknown tokens become empty. Collapses the runs
// of whitespace that empty tokens leave behind so titles stay tidy.
inline std::string render_title(const IcecastSession::Config& mc,
                                const CallState& cs) {
    char time_buf[16] = {0};
    if (cs.start_time) {
        std::tm tm{};
        time_t st = cs.start_time;
        localtime_r(&st, &tm);
        std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);
    }
    char freq_buf[32];
    std::snprintf(freq_buf, sizeof(freq_buf), "%.4f", cs.freq / 1e6);

    const std::unordered_map<std::string, std::string> vals = {
        {"TALKGROUP", std::to_string(cs.talkgroup)},
        {"TALKGROUP_TAG", cs.talkgroup_display},
        {"TAG", cs.talkgroup_tag},
        {"SYSTEM", cs.short_name},
        {"FREQ", freq_buf},
        {"TIME", time_buf},
    };

    const std::string& s = mc.title_template;
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            size_t end = s.find('}', i + 2);
            if (end != std::string::npos) {
                std::string key = s.substr(i + 2, end - (i + 2));
                for (char& c : key) c = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c)));
                auto it = vals.find(key);
                if (it != vals.end()) out += it->second;  // unknown -> empty
                i = end + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }

    // Collapse whitespace runs and trim, so an empty ${TAG}/${TIME} doesn't
    // leave "(123)  14:00" style gaps.
    std::string tidy;
    tidy.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        bool is_space = std::isspace(static_cast<unsigned char>(c));
        if (is_space) {
            if (!prev_space && !tidy.empty()) tidy.push_back(' ');
            prev_space = true;
        } else {
            tidy.push_back(c);
            prev_space = false;
        }
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}
}  // namespace

class IcecastBridgePlugin : public Plugin_Api {
public:
    static boost::shared_ptr<IcecastBridgePlugin> create() {
        return boost::shared_ptr<IcecastBridgePlugin>(new IcecastBridgePlugin());
    }

    int parse_config(json config_data) override {
        if (!parse_plugin_config(config_data, cfg_)) return -1;
        BOOST_LOG_TRIVIAL(info) << kPluginTag
            << "Config OK - server: " << cfg_.host << ":" << cfg_.port
            << ", mounts: " << cfg_.mounts_by_name.size()
            << ", systems: " << cfg_.systems.size();
        return 0;
    }

    int init(Config* tr_config, std::vector<Source*>, std::vector<System*>) override {
        if (!tr_config || !tr_config->enable_audio_streaming) {
            BOOST_LOG_TRIVIAL(error) << kPluginTag
                << "Requires \"audioStreaming\": true in trunk-recorder's main config";
            return -1;
        }
        pool_ = std::make_unique<IcecastPool>(ioc_);
        return 0;
    }

    int start() override {
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioc_));
        worker_ = std::thread([this] {
            try {
                ioc_.run();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << kPluginTag << "io_context died: " << e.what();
            }
        });
        // Bring up every configured mount eagerly — listeners can connect
        // immediately and hear silence until calls arrive.
        std::vector<IcecastSession::Config> mounts;
        mounts.reserve(cfg_.mounts_by_name.size());
        for (auto& [_, m] : cfg_.mounts_by_name) mounts.push_back(m);
        boost::asio::post(ioc_, [this, mounts] { pool_->start_all(mounts); });
        BOOST_LOG_TRIVIAL(info) << kPluginTag << "Started";
        return 0;
    }

    int stop() override {
        boost::asio::post(ioc_, [this] {
            if (pool_) pool_->shutdown();
            // Cancel any pending ghost timers so the io_context can drain.
            // A pending steady_timer counts as work; without this, worker_
            // wouldn't exit until every armed timer fired (up to
            // kGhostCleanupSeconds), stalling shutdown on a busy system.
            for (auto& [_, t] : ghost_timers_) {
                boost::system::error_code ignore;
                t->cancel(ignore);
            }
            ghost_timers_.clear();
        });
        if (work_guard_) work_guard_.reset();
        if (worker_.joinable()) worker_.join();
        BOOST_LOG_TRIVIAL(info) << kPluginTag << "Stopped";
        return 0;
    }

    int call_start(Call* call) override {
        if (!call) return 0;
        auto state = register_call(call);
        if (!state) return 0;          // talkgroup not mapped to a mount
        const long call_num = state->call_num;

        // Arm the ghost-cleanup timer. trunk-recorder swallows BOTH call_end
        // and audio_stream for sub-minDuration squelch flutter, so neither
        // normal cleanup path runs. If this call hasn't been enqueued (never
        // produced audio) by the time the timer fires, drop the registry
        // entry. Calls that did produce audio are in mount_queue_ and the
        // stale watchdog in produce_frame_for owns them from there.
        std::weak_ptr<CallState> weak_state = state;
        boost::asio::post(ioc_, [this, weak_state, call_num] {
            auto timer = std::make_shared<boost::asio::steady_timer>(ioc_);
            timer->expires_after(std::chrono::seconds(kGhostCleanupSeconds));
            ghost_timers_[call_num] = timer;
            timer->async_wait([this, weak_state, call_num, timer]
                              (const boost::system::error_code& ec) {
                ghost_timers_.erase(call_num);  // `timer` capture keeps it alive
                if (ec) return;                 // cancelled by stop()
                auto cs = weak_state.lock();
                if (!cs) return;                // already cleaned up normally
                auto qit = mount_queue_.find(cs->mountpoint);
                if (qit != mount_queue_.end()) {
                    for (auto& s : qit->second)
                        if (s->call_num == call_num) return;  // watchdog owns it
                }
                BOOST_LOG_TRIVIAL(info) << call_hdr(*cs)
                    << "Discarded (no audio, no call_end - sub-minDuration ghost)";
                registry_.remove(call_num);
            });
        });
        return 0;
    }

    int audio_stream(Call* call, Recorder* recorder, int16_t* samples,
                     int sampleCount) override {
        if (!call || sampleCount <= 0) return 0;
        auto state = registry_.find(call->get_call_num());
        if (!state) {
            // No registry entry. For a conventional channel call_start fires
            // at channel setup (or the previous transmission's conclude), so
            // the gap to this audio is a normal inter-transmission gap —
            // routinely minutes — and the ghost-cleanup timer has long since
            // dropped the entry. Register the call now from the live Call* so
            // its audio is streamed instead of discarded.
            state = register_call(call);
            if (!state) return 0;       // talkgroup not mapped to a mount
        }

        if (!state->encoder) {
            uint16_t in_rate = recorder ? static_cast<uint16_t>(recorder->get_wav_hz())
                                        : uint16_t{16000};
            state->input_sample_rate = in_rate;
            const auto* mount_cfg = cfg_.find_mount(state->mountpoint);
            if (!mount_cfg) return 0;  // shouldn't happen — config validates this
            try {
                state->resampler = std::make_unique<Resampler>(
                    in_rate, mount_cfg->output_sample_rate, mount_cfg->channels);
                state->encoder = std::make_unique<Mp3FrameEncoder>(
                    mount_cfg->output_sample_rate, mount_cfg->bitrate_kbps,
                    mount_cfg->channels);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << call_hdr(*state)
                    << "Encoder init failed: " << e.what();
                return 0;
            }
            auto self_state = state;
            boost::asio::post(ioc_, [this, self_state] {
                enqueue_for_streaming(self_state);
            });
        }
        state->pcm.push(samples, sampleCount);
        return 0;
    }

    int call_end(Call_Data_t call_info) override {
        auto state = registry_.find(call_info.call_num);
        if (!state) return 0;
        state->ending.store(true);
        BOOST_LOG_TRIVIAL(info) << call_hdr(*state) << "Call ended";
        auto self_state = state;
        boost::asio::post(ioc_, [this, self_state] {
            cleanup_inactive(self_state);
        });
        return 0;
    }

private:
    // Build and register the CallState for a call: mount lookup + registry
    // insert + the "Routing to mount" log. Shared by call_start and by
    // audio_stream — the latter calls it when audio arrives for a call whose
    // call_start registry entry is gone (conventional channels: call_start
    // fires at setup / the prior transmission's conclude, an inter-transmission
    // gap before the real audio). Returns nullptr when the talkgroup isn't
    // mapped to a mount. Safe to call from the monitor thread (call_start) or
    // the GR flowgraph thread (audio_stream) — registry_ is internally locked.
    std::shared_ptr<CallState> register_call(Call* call) {
        const std::string short_name = call->get_short_name();
        const long tg = call->get_talkgroup();
        const long call_num = call->get_call_num();
        const std::string* mount = cfg_.lookup_mount(short_name, tg);
        if (!mount) {
            BOOST_LOG_TRIVIAL(debug)
                << log_header(short_name, call_num,
                              call->get_talkgroup_display(), call->get_freq())
                << "Icecast Bridge - Talkgroup not mapped, ignoring";
            return nullptr;
        }
        auto state = std::make_shared<CallState>();
        state->call_num = call_num;
        state->mountpoint = *mount;
        state->short_name = short_name;
        state->talkgroup = tg;
        state->talkgroup_display = call->get_talkgroup_display();
        state->talkgroup_tag = call->get_talkgroup_tag();
        state->freq = call->get_freq();
        state->start_time = call->get_start_time();
        registry_.insert(call_num, state);
        BOOST_LOG_TRIVIAL(info) << call_hdr(*state)
            << "Routing to mount '" << state->mountpoint << "'";
        return state;
    }

    // Asio-thread only.  Mountpoint queue is the FIFO of calls landing on the
    // same mount. front() is the active call whose MP3 frames the session's
    // pacer pulls; everything else waits its turn.
    void enqueue_for_streaming(std::shared_ptr<CallState> state) {
        auto& q = mount_queue_[state->mountpoint];
        bool was_empty = q.empty();
        q.push_back(state);
        if (was_empty) {
            promote_active(state);
        } else {
            BOOST_LOG_TRIVIAL(info) << call_hdr(*state)
                << "Queued behind " << (q.size() - 1)
                << " on mount '" << state->mountpoint << "'";
        }
    }

    void cleanup_inactive(std::shared_ptr<CallState> state) {
        auto qit = mount_queue_.find(state->mountpoint);
        if (qit == mount_queue_.end()) {
            registry_.remove(state->call_num);
            BOOST_LOG_TRIVIAL(info) << call_hdr(*state)
                << "Ended without audio (nothing to send)";
            return;
        }
        auto& q = qit->second;
        if (!q.empty() && q.front()->call_num == state->call_num) {
            // Active — drained-empty check in the producer will finalize it.
            return;
        }
        for (auto it = q.begin(); it != q.end(); ++it) {
            if ((*it)->call_num == state->call_num) {
                q.erase(it);
                BOOST_LOG_TRIVIAL(info) << call_hdr(*state)
                    << "Dropped from queue (ended before reaching mount '"
                    << state->mountpoint << "')";
                registry_.remove(state->call_num);
                return;
            }
        }
        registry_.remove(state->call_num);
    }

    // Wire `state`'s frame producer into the session for its mountpoint.
    // The session's pacer will pull one MP3 frame per tick; when this call's
    // PCM is empty AND ending, we finalize and promote the next queued call.
    void promote_active(std::shared_ptr<CallState> state) {
        auto session = pool_ ? pool_->get(state->mountpoint) : nullptr;
        if (!session) {
            BOOST_LOG_TRIVIAL(warning) << call_hdr(*state)
                << "No session for mount '" << state->mountpoint << "', dropping";
            finalize_call(state);
            return;
        }
        state->session = session;
        state->first_sent = std::chrono::steady_clock::now();
        // Start the stale-call watchdog clock now: produce_frame_for only runs
        // for the active (promoted) call, so a queued call's clock starts when
        // it reaches the front, not when it was enqueued.
        state->last_input = state->first_sent;

        std::weak_ptr<CallState> weak_cs = state;
        auto* self = this;

        session->set_frame_producer([self, weak_cs]() -> std::vector<uint8_t> {
            auto cs = weak_cs.lock();
            if (!cs) return {};
            return self->produce_frame_for(cs);
        });

        BOOST_LOG_TRIVIAL(info) << call_hdr(*state)
            << "Now active on mount '" << state->mountpoint << "'";

        // Push "now playing" metadata for the freshly-promoted call.
        if (const auto* mc = cfg_.find_mount(state->mountpoint);
            mc && mc->metadata_enabled && !mc->title_template.empty()) {
            session->update_metadata(render_title(*mc, *state));
        }
    }

    // Called by the session's pacer (asio thread) once per MP3 frame interval.
    // Returns the encoded MP3 frame to write, or empty to fall back to silence.
    std::vector<uint8_t> produce_frame_for(std::shared_ptr<CallState> cs) {
        if (!cs || !cs->encoder) return {};
        constexpr int kOut = Mp3FrameEncoder::kFrameSamples;
        const int frame_in =
            static_cast<int>(static_cast<uint64_t>(kOut) *
                             cs->input_sample_rate / cs->encoder->sample_rate());

        const auto now = std::chrono::steady_clock::now();
        bool ending = cs->ending.load();
        if (!ending &&
            now - cs->last_input > std::chrono::milliseconds(kStaleAfterMs)) {
            BOOST_LOG_TRIVIAL(info) << call_hdr(*cs) << "No audio for "
                << (kStaleAfterMs / 1000) << "s without call_end - finalizing";
            cs->ending.store(true);   // sticky: stays ended on later ticks
            ending = true;
        }

        // Try to top up cs->out_pcm to >= kOut samples so we can emit one frame.
        std::vector<int16_t> in_pcm;
        bool got_in = ending
            ? cs->pcm.drain_padded_frame(frame_in, in_pcm)
            : cs->pcm.try_pop_frame(frame_in, in_pcm);
        if (got_in) {
            if (!ending) cs->last_input = now;   // freeze the clock once ending
            try {
                auto resampled = cs->resampler->process(
                    in_pcm.data(), static_cast<int>(in_pcm.size()));
                cs->out_pcm.insert(cs->out_pcm.end(),
                                   resampled.begin(), resampled.end());
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << call_hdr(*cs)
                    << "Resample failed: " << e.what();
            }
        }

        // End-of-call: drain resampler's filter tail into out_pcm, then pad.
        if (ending && !got_in) {
            try {
                auto tail = cs->resampler->flush();
                cs->out_pcm.insert(cs->out_pcm.end(), tail.begin(), tail.end());
            } catch (...) { /* tolerate flush errors during shutdown */ }
            if (cs->out_pcm.empty()) {
                // Nothing left to encode. Flush encoder bytes and finalize.
                std::vector<uint8_t> tail;
                try { tail = cs->encoder->flush(); }
                catch (...) {}
                auto* self = this;
                auto cs_copy = cs;
                boost::asio::post(ioc_, [self, cs_copy] {
                    self->finalize_call(cs_copy);
                });
                return tail;   // last bytes from encoder bit reservoir
            }
            // Pad to a full frame so we can emit it.
            if (cs->out_pcm.size() < static_cast<size_t>(kOut)) {
                cs->out_pcm.resize(kOut, 0);
            }
        }

        if (cs->out_pcm.size() < static_cast<size_t>(kOut)) {
            // Mid-call gap — silence this tick keeps the stream alive.
            return {};
        }

        try {
            auto bytes = cs->encoder->encode(cs->out_pcm.data(), kOut);
            cs->out_pcm.erase(cs->out_pcm.begin(), cs->out_pcm.begin() + kOut);
            cs->mp3_frame_id++;
            return bytes;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << call_hdr(*cs)
                << "MP3 encode failed: " << e.what();
            return {};
        }
    }

    void finalize_call(std::shared_ptr<CallState> cs) {
        BOOST_LOG_TRIVIAL(info) << call_hdr(*cs)
            << "Finalized (" << cs->mp3_frame_id << " MP3 frames sent)";
        registry_.remove(cs->call_num);

        auto qit = mount_queue_.find(cs->mountpoint);
        if (qit == mount_queue_.end()) return;
        auto& q = qit->second;
        if (!q.empty() && q.front()->call_num == cs->call_num) q.pop_front();
        if (q.empty()) {
            mount_queue_.erase(qit);
            // Detach producer; session reverts to silence-only.
            if (cs->session) {
                cs->session->set_frame_producer(nullptr);
                // Mount is idle — reset the "now playing" title if configured.
                if (const auto* mc = cfg_.find_mount(cs->mountpoint);
                    mc && mc->metadata_enabled && !mc->idle_title.empty()) {
                    cs->session->update_metadata(mc->idle_title);
                }
            }
            return;
        }
        auto next = q.front();
        BOOST_LOG_TRIVIAL(info) << call_hdr(*next)
            << "Promoted from queue on '" << cs->mountpoint
            << "' (remaining=" << (q.size() - 1) << ")";
        promote_active(next);
    }

    PluginConfig cfg_;
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_guard_;
    std::thread worker_;
    std::unique_ptr<IcecastPool> pool_;
    CallStateRegistry registry_;
    // Per-mountpoint FIFO of pending calls; front() is the active call whose
    // frames the session's pacer pulls. Mutated only on the asio thread.
    std::unordered_map<std::string, std::deque<std::shared_ptr<CallState>>> mount_queue_;
    // Per-call ghost-cleanup timers, keyed by call_num. A timer self-erases
    // when it fires; stop() cancels any still pending. Asio-thread only.
    std::unordered_map<long, std::shared_ptr<boost::asio::steady_timer>> ghost_timers_;
};

}  // namespace icecast_bridge

BOOST_DLL_ALIAS(icecast_bridge::IcecastBridgePlugin::create, create_plugin)