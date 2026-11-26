#pragma once

#include "audio/adx_decoder.hpp"
#include "core/config.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fzgx {

struct AudioStatus {
    bool device_open{false};
    std::string device_name{"Uninitialized"};
    bool adx_playing{false};
    bool adx_paused{false};
    bool adx_loop{true};
    std::string adx_label;
    AdxInfo adx_info{};
    uint32_t adx_current_sample{0};
    size_t fmv_buffered_frames{0};
};

class AudioEngine {
public:
    static constexpr uint32_t kOutputSampleRate = 48000;

    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool initialize(std::string* error_out = nullptr);
    void shutdown();

    void apply_config(const AppConfig& config);
    void set_volumes(float master, float music, float sfx, bool muted);

    bool play_adx(std::vector<uint8_t> adx_bytes, const std::string& label, bool loop = true, std::string* error_out = nullptr);
    void stop_adx();
    void set_adx_paused(bool paused);
    void set_adx_loop(bool loop);

    // Interleaved 48 kHz stereo float32 frames pushed from SofdecPlayer
    void push_fmv_pcm(const float* stereo_frames, size_t frame_count);
    void clear_fmv_pcm();

    AudioStatus status() const;

private:
    static void SDLCALL sdl_audio_callback(void* userdata,
                                           SDL_AudioStream* stream,
                                           int additional_amount,
                                           int total_amount);
    void mix_frames(float* out_stereo, size_t frame_count);

    SDL_AudioStream* stream_{nullptr};
    bool device_open_{false};
    std::string device_name_{"Uninitialized"};

    std::atomic<float> master_vol_{1.0f};
    std::atomic<float> music_vol_{0.85f};
    std::atomic<float> sfx_vol_{0.90f};
    std::atomic<bool> muted_{false};

    mutable std::mutex mutex_;
    AdxDecoder adx_;
    bool adx_playing_{false};
    bool adx_paused_{false};
    bool adx_loop_{true};
    std::string adx_label_;

    std::deque<float> fmv_pcm_queue_;
    std::vector<float> scratch_adx_;
};

} // namespace fzgx
