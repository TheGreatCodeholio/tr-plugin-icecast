#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace icecast_bridge {

// Single-producer / single-consumer PCM buffer. Producer is the recorder's
// flowgraph thread (audio_stream callback); consumer is the plugin's asio
// worker thread. push and the pop variants contend on a single mutex; under
// the expected load (~once per MP3 frame each) contention is negligible.
class PcmRingBuffer {
public:
    void push(const int16_t* samples, int count) {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.insert(buf_.end(), samples, samples + count);
    }

    // Pops exactly `frame_samples` into `out`. Returns false if buffer is short.
    bool try_pop_frame(int frame_samples, std::vector<int16_t>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (static_cast<int>(buf_.size()) < frame_samples) return false;
        out.assign(buf_.begin(), buf_.begin() + frame_samples);
        buf_.erase(buf_.begin(), buf_.begin() + frame_samples);
        return true;
    }

    // Pops up to `frame_samples` samples, zero-padding only the final short
    // frame. Returns false only when the buffer is empty. Unlike try_pop_frame,
    // this tolerates underfill — call it once per tick to drain the tail of an
    // ending call. (It must NOT take the whole buffer at once: a call queued
    // behind another can have seconds of audio buffered, and one MP3 frame is
    // all we may emit per tick.)
    bool drain_padded_frame(int frame_samples, std::vector<int16_t>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.empty()) return false;
        const int n = std::min<int>(static_cast<int>(buf_.size()), frame_samples);
        out.assign(buf_.begin(), buf_.begin() + n);
        buf_.erase(buf_.begin(), buf_.begin() + n);
        out.resize(frame_samples, 0);   // pad only when n < frame_samples
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return buf_.size();
    }

private:
    mutable std::mutex mu_;
    std::deque<int16_t> buf_;
};

}  // namespace icecast_bridge