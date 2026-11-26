#include "audio/adx_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace fzgx {

namespace {

inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline int16_t clamp_s16(int32_t v) {
    return static_cast<int16_t>(std::clamp<int32_t>(v, -32768, 32767));
}

} // namespace

bool AdxDecoder::open(std::vector<uint8_t> adx_bytes, std::string* error_out) {
    open_ = false;
    data_ = std::move(adx_bytes);
    if (data_.size() < 24) {
        if (error_out) *error_out = "File too small for CRI ADX header.";
        return false;
    }

    const uint16_t magic = read_be16(data_.data() + 0x00);
    if (magic != 0x8000u) {
        if (error_out) *error_out = "Invalid CRI ADX magic (expected 0x8000).";
        return false;
    }

    const uint16_t data_offset = static_cast<uint16_t>(read_be16(data_.data() + 0x02) + 4);
    const uint8_t encoding = data_[0x04];
    const uint8_t block_size = data_[0x05];
    const uint8_t sample_bits = data_[0x06];
    const uint8_t channels = data_[0x07];
    const uint32_t sample_rate = read_be32(data_.data() + 0x08);
    const uint32_t total_samples = read_be32(data_.data() + 0x0C);
    const uint16_t highpass_freq = read_be16(data_.data() + 0x10);
    const uint8_t version = data_[0x12];

    if (encoding != 3 || block_size != 18 || sample_bits != 4 || (channels != 1 && channels != 2) ||
        sample_rate == 0 || data_offset < 20 || data_offset > data_.size()) {
        if (error_out) *error_out = "Unsupported CRI ADX encoding/parameters.";
        return false;
    }

    if (data_offset >= 6 && std::memcmp(data_.data() + data_offset - 6, "(c)CRI", 6) != 0) {
        if (error_out) *error_out = "Missing '(c)CRI' signature in ADX header.";
        return false;
    }

    info_ = {};
    info_.sample_rate = sample_rate;
    info_.channels = channels;
    info_.total_samples = total_samples;
    info_.data_offset = data_offset;
    info_.block_size = block_size;
    info_.version = version;
    info_.highpass_freq = highpass_freq > 0 ? highpass_freq : 500;

    // Parse loop table if header is large enough (v3 at 0x18..0x24, v4 at 0x24..0x30)
    if (version == 3 && data_offset >= 0x2C) {
        const uint32_t loop_flag = read_be32(data_.data() + 0x18);
        if (loop_flag != 0) {
            info_.has_loop = true;
            info_.loop_start_sample = read_be32(data_.data() + 0x1C);
            info_.loop_end_sample = read_be32(data_.data() + 0x24);
        }
    } else if (version == 4 && data_offset >= 0x38) {
        const uint32_t loop_flag = read_be32(data_.data() + 0x24);
        if (loop_flag != 0) {
            info_.has_loop = true;
            info_.loop_start_sample = read_be32(data_.data() + 0x28);
            info_.loop_end_sample = read_be32(data_.data() + 0x30);
        }
    }

    if (info_.loop_end_sample > info_.total_samples || info_.loop_end_sample <= info_.loop_start_sample) {
        info_.has_loop = false;
    }

    const double a = std::sqrt(2.0) -
                     std::cos(2.0 * std::numbers::pi * static_cast<double>(info_.highpass_freq) /
                              static_cast<double>(info_.sample_rate));
    const double b = std::sqrt(2.0) - 1.0;
    const double c = (a - std::sqrt(std::max(0.0, (a + b) * (a - b)))) / b;
    coef1_ = static_cast<int32_t>(std::floor(c * 2.0 * 4096.0));
    coef2_ = static_cast<int32_t>(std::floor(-(c * c) * 4096.0));

    open_ = true;
    reset();
    return true;
}

void AdxDecoder::reset() {
    hist1_[0] = hist1_[1] = 0;
    hist2_[0] = hist2_[1] = 0;
    current_sample_ = 0;
    block_pos_sample_ = 0;
    block_valid_samples_ = 0;
    block_cursor_ = 0;
    finished_ = false;
    resample_pos_ = 0.0;
    prev_l_ = prev_r_ = cur_l_ = cur_r_ = 0.0f;
    has_resample_pair_ = false;
}

bool AdxDecoder::decode_next_block() {
    if (!open_ || block_pos_sample_ >= info_.total_samples) {
        return false;
    }

    const uint32_t block_index = block_pos_sample_ / 32;
    const size_t frame_bytes = static_cast<size_t>(info_.channels) * info_.block_size;
    const size_t file_off = static_cast<size_t>(info_.data_offset) + static_cast<size_t>(block_index) * frame_bytes;
    if (file_off + frame_bytes > data_.size()) {
        return false;
    }

    const uint32_t remaining = info_.total_samples - block_pos_sample_;
    block_valid_samples_ = std::min<uint32_t>(32, remaining);

    for (uint32_t ch = 0; ch < info_.channels; ++ch) {
        const uint8_t* blk = data_.data() + file_off + static_cast<size_t>(ch) * info_.block_size;
        const int32_t scale = static_cast<int32_t>(read_be16(blk)) + 1;
        for (uint32_t i = 0; i < 32; ++i) {
            const uint8_t packed = blk[2 + (i / 2)];
            int32_t nibble = (i & 1u) == 0 ? ((packed >> 4) & 0xF) : (packed & 0xF);
            if (nibble >= 8) {
                nibble -= 16;
            }
            const int32_t pred = (coef1_ * hist1_[ch] + coef2_ * hist2_[ch]) >> 12;
            const int16_t sample = clamp_s16(nibble * scale + pred);
            hist2_[ch] = hist1_[ch];
            hist1_[ch] = sample;
            block_pcm_[ch][i] = sample;
        }
    }

    block_pos_sample_ += 32;
    block_cursor_ = 0;
    return true;
}

bool AdxDecoder::seek_to_sample(uint32_t target_sample) {
    reset();
    while (block_pos_sample_ + 32 <= target_sample) {
        if (!decode_next_block()) {
            return false;
        }
    }
    if (target_sample > block_pos_sample_) {
        if (!decode_next_block()) {
            return false;
        }
        block_cursor_ = target_sample - (block_pos_sample_ - 32);
    }
    current_sample_ = target_sample;
    return true;
}

size_t AdxDecoder::read_frames_stereo_f32(float* out_stereo,
                                          size_t num_frames,
                                          uint32_t target_rate,
                                          bool loop_enabled) {
    if (!open_ || !out_stereo || num_frames == 0 || target_rate == 0) {
        return 0;
    }

    auto pull_source_sample = [&](float& out_l, float& out_r) -> bool {
        if (finished_) {
            return false;
        }
        const uint32_t end_limit = (loop_enabled && info_.has_loop) ? info_.loop_end_sample : info_.total_samples;
        if (current_sample_ >= end_limit) {
            if (loop_enabled) {
                const uint32_t target = info_.has_loop ? info_.loop_start_sample : 0;
                if (!seek_to_sample(target)) {
                    finished_ = true;
                    return false;
                }
            } else {
                finished_ = true;
                return false;
            }
        }
        if (block_cursor_ >= block_valid_samples_) {
            if (!decode_next_block()) {
                finished_ = true;
                return false;
            }
        }
        const int16_t s_l = block_pcm_[0][block_cursor_];
        const int16_t s_r = info_.channels > 1 ? block_pcm_[1][block_cursor_] : s_l;
        ++block_cursor_;
        ++current_sample_;
        out_l = static_cast<float>(s_l) / 32768.0f;
        out_r = static_cast<float>(s_r) / 32768.0f;
        return true;
    };

    if (!has_resample_pair_) {
        if (!pull_source_sample(prev_l_, prev_r_)) {
            return 0;
        }
        if (!pull_source_sample(cur_l_, cur_r_)) {
            cur_l_ = prev_l_;
            cur_r_ = prev_r_;
        }
        has_resample_pair_ = true;
        resample_pos_ = 0.0;
    }

    const double step = static_cast<double>(info_.sample_rate) / static_cast<double>(target_rate);
    size_t written = 0;
    for (size_t i = 0; i < num_frames; ++i) {
        while (resample_pos_ >= 1.0) {
            prev_l_ = cur_l_;
            prev_r_ = cur_r_;
            if (!pull_source_sample(cur_l_, cur_r_)) {
                break;
            }
            resample_pos_ -= 1.0;
        }
        if (finished_) {
            break;
        }
        const float frac = static_cast<float>(std::clamp(resample_pos_, 0.0, 1.0));
        out_stereo[written * 2 + 0] = prev_l_ + (cur_l_ - prev_l_) * frac;
        out_stereo[written * 2 + 1] = prev_r_ + (cur_r_ - prev_r_) * frac;
        ++written;
        resample_pos_ += step;
    }

    return written;
}

} // namespace fzgx
