#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fzgx {

struct AdxInfo {
    uint32_t sample_rate{0};
    uint32_t channels{0};
    uint32_t total_samples{0};
    uint16_t data_offset{0};
    uint8_t block_size{18};
    uint8_t version{3};
    uint16_t highpass_freq{500};
    bool has_loop{false};
    uint32_t loop_start_sample{0};
    uint32_t loop_end_sample{0};
};

class AdxDecoder {
public:
    bool open(std::vector<uint8_t> adx_bytes, std::string* error_out = nullptr);
    void reset();

    bool is_open() const { return open_; }
    const AdxInfo& info() const { return info_; }
    uint32_t current_sample() const { return current_sample_; }
    bool is_finished() const { return finished_; }

    // Decodes and resamples into interleaved stereo float32 frames at target_rate (e.g. 48000 Hz).
    // Returns number of stereo frames written.
    size_t read_frames_stereo_f32(float* out_stereo,
                                  size_t num_frames,
                                  uint32_t target_rate,
                                  bool loop_enabled);

private:
    bool decode_next_block();
    bool seek_to_sample(uint32_t target_sample);

    std::vector<uint8_t> data_;
    AdxInfo info_{};
    bool open_{false};
    bool finished_{false};

    int32_t coef1_{0};
    int32_t coef2_{0};
    int32_t hist1_[2]{0, 0};
    int32_t hist2_[2]{0, 0};

    uint32_t current_sample_{0};
    uint32_t block_pos_sample_{0};
    // Current decoded block (up to 32 samples per channel)
    int16_t block_pcm_[2][32]{};
    uint32_t block_valid_samples_{0};
    uint32_t block_cursor_{0};

    // Linear resampling accumulator in source sample units
    double resample_pos_{0.0};
    float prev_l_{0.0f};
    float prev_r_{0.0f};
    float cur_l_{0.0f};
    float cur_r_{0.0f};
    bool has_resample_pair_{false};
};

} // namespace fzgx
