#pragma once

#include <cstdint>
#include <vector>

typedef struct SRC_STATE_tag SRC_STATE;  // libsamplerate opaque handle

namespace icecast_bridge {

// Wraps libsamplerate (SRC) for one call's PCM stream. Trunk-recorder hands us
// 8 kHz or 16 kHz int16 PCM; icecast/MP3 listeners expect 22050/32000/44100.
// One Resampler per call so internal SRC state doesn't bleed between calls.
class Resampler {
public:
    Resampler(uint32_t input_rate, uint32_t output_rate, int channels);
    ~Resampler();

    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Push input PCM, get output PCM at output_rate. Output length is roughly
    // in_count * (output_rate / input_rate); SRC may hold a few samples back.
    // Returns whatever it produced this call (may be empty).
    std::vector<int16_t> process(const int16_t* in, int in_count);

    // Final flush at end-of-call (drain SRC's internal buffer with end_of_input=1).
    std::vector<int16_t> flush();

    uint32_t input_rate() const { return input_rate_; }
    uint32_t output_rate() const { return output_rate_; }

private:
    uint32_t input_rate_;
    uint32_t output_rate_;
    int channels_;
    SRC_STATE* state_ = nullptr;
};

}  // namespace icecast_bridge