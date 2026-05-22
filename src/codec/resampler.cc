#include "codec/resampler.h"

#include <samplerate.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace icecast_bridge {

namespace {
// Generous headroom: at the highest realistic ratio (8k -> 48k = 6x) and
// trunk-recorder chunk sizes, this is plenty without per-call tuning.
constexpr int kOutputHeadroom = 64;

inline int compute_out_capacity(int in_count, double ratio) {
    return static_cast<int>(in_count * ratio) + kOutputHeadroom;
}
}  // namespace

Resampler::Resampler(uint32_t input_rate, uint32_t output_rate, int channels)
    : input_rate_(input_rate),
      output_rate_(output_rate),
      channels_(channels) {
    int err = 0;
    state_ = src_new(SRC_SINC_FASTEST, channels_, &err);
    if (!state_) {
        throw std::runtime_error(std::string("src_new failed: ") + src_strerror(err));
    }
}

Resampler::~Resampler() {
    if (state_) src_delete(state_);
}

std::vector<int16_t> Resampler::process(const int16_t* in, int in_count) {
    if (in_count <= 0) return {};
    const double ratio = static_cast<double>(output_rate_) / input_rate_;

    // int16 -> float input
    std::vector<float> in_f(static_cast<size_t>(in_count));
    src_short_to_float_array(in, in_f.data(), in_count);

    // SRC consumes whole frames (channels samples per frame). Our callers feed
    // mono int16 today, so frames == samples.
    const int in_frames = in_count / channels_;
    std::vector<float> out_f(static_cast<size_t>(
        compute_out_capacity(in_count, ratio)));

    SRC_DATA data{};
    data.data_in = in_f.data();
    data.data_out = out_f.data();
    data.input_frames = in_frames;
    data.output_frames = static_cast<long>(out_f.size() / channels_);
    data.src_ratio = ratio;
    data.end_of_input = 0;

    int err = src_process(state_, &data);
    if (err != 0) {
        throw std::runtime_error(std::string("src_process: ") + src_strerror(err));
    }

    const int out_count = static_cast<int>(data.output_frames_gen) * channels_;
    std::vector<int16_t> out(static_cast<size_t>(out_count));
    if (out_count > 0) {
        src_float_to_short_array(out_f.data(), out.data(), out_count);
    }
    return out;
}

std::vector<int16_t> Resampler::flush() {
    const double ratio = static_cast<double>(output_rate_) / input_rate_;
    // Drain SRC's internal buffer with end_of_input set. Provide a small
    // synthetic input so SRC has a frame count to anchor; the actual PCM is
    // ignored when end_of_input flushes filter state.
    float dummy_in[1] = {0.0f};
    std::vector<float> out_f(static_cast<size_t>(
        compute_out_capacity(64, ratio)));

    SRC_DATA data{};
    data.data_in = dummy_in;
    data.data_out = out_f.data();
    data.input_frames = 0;
    data.output_frames = static_cast<long>(out_f.size() / channels_);
    data.src_ratio = ratio;
    data.end_of_input = 1;

    int err = src_process(state_, &data);
    if (err != 0) {
        throw std::runtime_error(std::string("src_process(flush): ") + src_strerror(err));
    }
    const int out_count = static_cast<int>(data.output_frames_gen) * channels_;
    std::vector<int16_t> out(static_cast<size_t>(out_count));
    if (out_count > 0) {
        src_float_to_short_array(out_f.data(), out.data(), out_count);
    }
    return out;
}

}  // namespace icecast_bridge