#pragma once

#include "codec/mp3_encoder.h"
#include "codec/resampler.h"
#include "pipeline/ring_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace icecast_bridge {

class IcecastSession;  // fwd

// Per-active-call state. Created in call_start, destroyed shortly after call_end.
struct CallState {
    long call_num = 0;
    uint16_t input_sample_rate = 16000;   // recorder native rate
    std::string mountpoint;               // icecast mount this call feeds

    // Cached at call_start so the asio thread can log with the same context
    // trunk-recorder's bundled plugins use (short_name / TG / freq).
    std::string short_name;
    long talkgroup = 0;
    std::string talkgroup_display;
    double freq = 0.0;

    // Linear amplitude multiplier copied from the mount's Config at encoder-
    // init time. Applied to the resampled PCM just before LAME encoding.
    // 1.0 = unity. Values > 1.0 boost (will clip if source is near full scale).
    // Values < 1.0 attenuate. Samples are clamped to int16 range after scaling.
    float gain = 1.0f;

    PcmRingBuffer pcm;
    std::unique_ptr<Resampler> resampler;       // input_rate -> mount output_rate
    std::unique_ptr<Mp3FrameEncoder> encoder;   // per-call so its bit reservoir
                                                // dies cleanly on call_end
    std::vector<int16_t> out_pcm;               // resampled samples not yet
                                                // encoded; flushed in 1152-sample
                                                // chunks. Asio thread only.
    std::shared_ptr<IcecastSession> session;

    std::atomic<bool> ending{false};
    uint64_t mp3_frame_id = 0;             // mutated only on asio thread
    std::chrono::steady_clock::time_point first_sent{};
    // Last pacer tick that pulled real PCM. Drives the stale-call watchdog
    // (finalize a call whose call_end trunk-recorder never delivered).
    // Asio thread only.
    std::chrono::steady_clock::time_point last_input{};
};

// Thread-safe registry keyed by call_num. Reads (audio_stream callback) take the
// shared lock; writes (call_start, call_end) take exclusive.
class CallStateRegistry {
public:
    void insert(long call_num, std::shared_ptr<CallState> state) {
        std::unique_lock lk(mu_);
        states_[call_num] = std::move(state);
    }

    std::shared_ptr<CallState> find(long call_num) const {
        std::shared_lock lk(mu_);
        auto it = states_.find(call_num);
        return it == states_.end() ? nullptr : it->second;
    }

    std::shared_ptr<CallState> remove(long call_num) {
        std::unique_lock lk(mu_);
        auto it = states_.find(call_num);
        if (it == states_.end()) return nullptr;
        auto out = std::move(it->second);
        states_.erase(it);
        return out;
    }

    template <typename F>
    void for_each(F&& f) const {
        std::shared_lock lk(mu_);
        for (auto& [k, v] : states_) f(k, v);
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<long, std::shared_ptr<CallState>> states_;
};

}  // namespace icecast_bridge
