#include "audio/audio_engine.hpp"

#include <algorithm>
#include <cstring>

namespace fzgx {

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(std::string* error_out) {
    shutdown();
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (error_out) {
                *error_out = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
            }
            device_name_ = "Unavailable";
            return false;
        }
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = static_cast<int>(kOutputSampleRate);

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_callback, this);
    if (!stream_) {
        if (error_out) {
            *error_out = std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError();
        }
        device_name_ = "Unavailable";
        return false;
    }

    SDL_AudioDeviceID dev_id = SDL_GetAudioStreamDevice(stream_);
    const char* name = SDL_GetAudioDeviceName(dev_id);
    device_name_ = name ? name : "SDL3 Playback Device";
    device_open_ = true;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioEngine::shutdown() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    device_open_ = false;
    device_name_ = "Stopped";
}

void AudioEngine::apply_config(const AppConfig& config) {
    set_volumes(config.master_volume, config.music_volume, config.sfx_volume, config.mute_audio);
}

void AudioEngine::set_volumes(float master, float music, float sfx, bool muted) {
    master_vol_.store(std::clamp(master, 0.0f, 1.0f));
    music_vol_.store(std::clamp(music, 0.0f, 1.0f));
    sfx_vol_.store(std::clamp(sfx, 0.0f, 1.0f));
    muted_.store(muted);
}

bool AudioEngine::play_adx(std::vector<uint8_t> adx_bytes,
                           const std::string& label,
                           bool loop,
                           std::string* error_out) {
    std::lock_guard lock(mutex_);
    if (!adx_.open(std::move(adx_bytes), error_out)) {
        adx_playing_ = false;
        return false;
    }
    adx_label_ = label;
    adx_loop_ = loop;
    adx_paused_ = false;
    adx_playing_ = true;
    return true;
}

void AudioEngine::stop_adx() {
    std::lock_guard lock(mutex_);
    adx_playing_ = false;
    adx_paused_ = false;
    adx_label_.clear();
}

void AudioEngine::set_adx_paused(bool paused) {
    std::lock_guard lock(mutex_);
    adx_paused_ = paused;
}

void AudioEngine::set_adx_loop(bool loop) {
    std::lock_guard lock(mutex_);
    adx_loop_ = loop;
}

void AudioEngine::push_fmv_pcm(const float* stereo_frames, size_t frame_count) {
    if (!stereo_frames || frame_count == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    // Cap buffered FMV audio to ~1 second (48000 stereo frames = 96000 floats)
    constexpr size_t kMaxFloats = kOutputSampleRate * 2;
    if (fmv_pcm_queue_.size() > kMaxFloats) {
        const size_t drop = fmv_pcm_queue_.size() - (kMaxFloats / 2);
        fmv_pcm_queue_.erase(fmv_pcm_queue_.begin(), fmv_pcm_queue_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    fmv_pcm_queue_.insert(fmv_pcm_queue_.end(), stereo_frames, stereo_frames + frame_count * 2);
}

void AudioEngine::clear_fmv_pcm() {
    std::lock_guard lock(mutex_);
    fmv_pcm_queue_.clear();
}

AudioStatus AudioEngine::status() const {
    std::lock_guard lock(mutex_);
    AudioStatus s{};
    s.device_open = device_open_;
    s.device_name = device_name_;
    s.adx_playing = adx_playing_;
    s.adx_paused = adx_paused_;
    s.adx_loop = adx_loop_;
    s.adx_label = adx_label_;
    s.adx_info = adx_.info();
    s.adx_current_sample = adx_.current_sample();
    s.fmv_buffered_frames = fmv_pcm_queue_.size() / 2;
    return s;
}

void SDLCALL AudioEngine::sdl_audio_callback(void* userdata,
                                             SDL_AudioStream* stream,
                                             int additional_amount,
                                             int) {
    if (!userdata || !stream || additional_amount <= 0) {
        return;
    }
    auto* self = static_cast<AudioEngine*>(userdata);
    const size_t total_frames = static_cast<size_t>(additional_amount) / (sizeof(float) * 2);
    if (total_frames == 0) {
        return;
    }

    constexpr size_t kChunkFrames = 1024;
    float mix_buf[kChunkFrames * 2];
    size_t remaining = total_frames;
    while (remaining > 0) {
        const size_t n = std::min(remaining, kChunkFrames);
        self->mix_frames(mix_buf, n);
        SDL_PutAudioStreamData(stream, mix_buf, static_cast<int>(n * 2 * sizeof(float)));
        remaining -= n;
    }
}

void AudioEngine::mix_frames(float* out_stereo, size_t frame_count) {
    std::memset(out_stereo, 0, frame_count * 2 * sizeof(float));

    const bool is_muted = muted_.load();
    const float master = master_vol_.load();
    const float music_gain = is_muted ? 0.0f : (master * music_vol_.load());

    std::lock_guard lock(mutex_);

    if (adx_playing_ && !adx_paused_) {
        scratch_adx_.resize(frame_count * 2);
        const size_t decoded = adx_.read_frames_stereo_f32(
            scratch_adx_.data(), frame_count, kOutputSampleRate, adx_loop_);
        for (size_t i = 0; i < decoded * 2; ++i) {
            out_stereo[i] += scratch_adx_[i] * music_gain;
        }
        if (adx_.is_finished()) {
            adx_playing_ = false;
        }
    }

    if (!fmv_pcm_queue_.empty()) {
        const size_t avail_frames = fmv_pcm_queue_.size() / 2;
        const size_t take_frames = std::min(avail_frames, frame_count);
        for (size_t i = 0; i < take_frames * 2; ++i) {
            out_stereo[i] += fmv_pcm_queue_.front() * music_gain;
            fmv_pcm_queue_.pop_front();
        }
    }

    for (size_t i = 0; i < frame_count * 2; ++i) {
        out_stereo[i] = std::clamp(out_stereo[i], -1.0f, 1.0f);
    }
}

} // namespace fzgx
