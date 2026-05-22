#include "codec/mp3_encoder.h"

#include <lame/lame.h>

#include <stdexcept>
#include <string>

namespace icecast_bridge {

namespace {
// lame's recommended output buffer size is 1.25 * num_samples + 7200.
constexpr size_t kEncodeBufBytes =
    static_cast<size_t>(1.25 * Mp3FrameEncoder::kFrameSamples) + 7200;

inline lame_global_flags* L(void* p) {
    return static_cast<lame_global_flags*>(p);
}
}  // namespace

Mp3FrameEncoder::Mp3FrameEncoder(uint32_t sample_rate,
                                 uint32_t bitrate_kbps,
                                 int channels)
    : sample_rate_(sample_rate),
      bitrate_kbps_(bitrate_kbps),
      channels_(channels) {
    if (channels_ != 1 && channels_ != 2) {
        throw std::runtime_error("Mp3FrameEncoder: channels must be 1 or 2");
    }
    lame_global_flags* gfp = lame_init();
    if (!gfp) throw std::runtime_error("lame_init failed");
    lame_ = gfp;

    lame_set_in_samplerate(gfp, static_cast<int>(sample_rate_));
    lame_set_out_samplerate(gfp, static_cast<int>(sample_rate_));
    lame_set_num_channels(gfp, channels_);
    lame_set_mode(gfp, channels_ == 1 ? MONO : JOINT_STEREO);
    lame_set_brate(gfp, static_cast<int>(bitrate_kbps_));
    lame_set_quality(gfp, 5);          // 0=best/slow, 9=worst/fast
    lame_set_VBR(gfp, vbr_off);
    lame_set_bWriteVbrTag(gfp, 0);     // no Xing/Info header for streaming
    // The wire stream is a concatenation of frames from several encoder
    // instances (one per call, plus the session's silence encoder). The
    // MPEG-1 bit reservoir lets a frame's data spill into earlier frames'
    // bytes; across an encoder-instance splice those bytes belong to a
    // different encoder and decode as an audible click. Disabling the
    // reservoir makes every frame self-contained and splice-safe.
    lame_set_disable_reservoir(gfp, 1);

    if (lame_init_params(gfp) < 0) {
        lame_close(gfp);
        lame_ = nullptr;
        throw std::runtime_error("lame_init_params failed");
    }
}

Mp3FrameEncoder::~Mp3FrameEncoder() {
    if (lame_) lame_close(L(lame_));
}

std::vector<uint8_t> Mp3FrameEncoder::encode(const int16_t* pcm, int frame_samples) {
    std::vector<uint8_t> out(kEncodeBufBytes);
    int bytes;
    if (channels_ == 1) {
        bytes = lame_encode_buffer(L(lame_),
                                   const_cast<short*>(pcm), nullptr,
                                   frame_samples,
                                   out.data(), static_cast<int>(out.size()));
    } else {
        // Stereo callers must supply interleaved L/R; frame_samples is per-channel.
        bytes = lame_encode_buffer_interleaved(L(lame_),
                                               const_cast<short*>(pcm),
                                               frame_samples,
                                               out.data(),
                                               static_cast<int>(out.size()));
    }
    if (bytes < 0) {
        throw std::runtime_error("lame_encode_buffer failed: " + std::to_string(bytes));
    }
    out.resize(static_cast<size_t>(bytes));
    return out;
}

std::vector<uint8_t> Mp3FrameEncoder::flush() {
    std::vector<uint8_t> out(7200);
    int bytes = lame_encode_flush(L(lame_), out.data(), static_cast<int>(out.size()));
    if (bytes < 0) {
        throw std::runtime_error("lame_encode_flush failed: " + std::to_string(bytes));
    }
    out.resize(static_cast<size_t>(bytes));
    return out;
}

}  // namespace icecast_bridge