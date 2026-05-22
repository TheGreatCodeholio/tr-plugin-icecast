#pragma once

#include <cstdint>
#include <vector>

namespace icecast_bridge {

// Wraps libmp3lame for one logical stream (one mountpoint or one call's chunk
// of a mountpoint stream). MP3 is a self-framed bitstream — concatenation of
// the bytes returned by encode() / flush() is a valid icecast payload, with
// the caveat that lame's bit-reservoir means the *first* frame may emit zero
// bytes and tail bytes appear in flush().
//
// Pacing note: an MP3 frame at MPEG-1 Layer III is 1152 PCM samples, so frame
// duration = 1152 / sample_rate seconds. The plugin's drain timer should use
// this, NOT a fixed 60 ms (which is the Zello/Opus convention).
class Mp3FrameEncoder {
public:
    Mp3FrameEncoder(uint32_t sample_rate, uint32_t bitrate_kbps, int channels);
    ~Mp3FrameEncoder();

    Mp3FrameEncoder(const Mp3FrameEncoder&) = delete;
    Mp3FrameEncoder& operator=(const Mp3FrameEncoder&) = delete;

    // Encode one MP3 frame worth of PCM (frame_samples == 1152 for MPEG-1 L3).
    // May return an empty vector while lame buffers the bit reservoir; that's
    // expected — caller should still pace by frame, not by bytes returned.
    std::vector<uint8_t> encode(const int16_t* pcm, int frame_samples);

    // Flush any residual bytes from the bit reservoir; call once per encoder
    // lifetime, not between frames.
    std::vector<uint8_t> flush();

    uint32_t sample_rate() const { return sample_rate_; }
    static constexpr int kFrameSamples = 1152;  // MPEG-1 Layer III

private:
    uint32_t sample_rate_;
    uint32_t bitrate_kbps_;
    int channels_;
    void* lame_ = nullptr;   // lame_global_flags*; void* avoids leaking lame.h

};

}  // namespace icecast_bridge