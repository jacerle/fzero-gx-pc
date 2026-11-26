#pragma once

#include "audio/audio_engine.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fzgx {

class SofdecPlayer {
public:
    SofdecPlayer() = default;
    ~SofdecPlayer();

    SofdecPlayer(const SofdecPlayer&) = delete;
    SofdecPlayer& operator=(const SofdecPlayer&) = delete;

    static bool is_ffmpeg_supported();

    bool open_from_memory(std::vector<uint8_t> sfd_bytes,
                          const std::string& label,
                          std::string* error_out = nullptr);
    void close();

    void set_paused(bool paused) { paused_ = paused; }
    bool is_paused() const { return paused_; }
    bool is_playing() const { return playing_; }

    // Advances playback by dt_seconds and emits any decoded audio to AudioEngine.
    // Returns true if a new video frame was decoded this tick.
    bool update(double dt_seconds, AudioEngine& audio);

    const std::vector<uint8_t>& latest_rgba() const { return rgba_frame_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t frame_serial() const { return frame_serial_; }
    double current_time_sec() const { return clock_sec_; }
    double duration_sec() const { return duration_sec_; }
    const std::string& label() const { return label_; }
    const std::string& status_message() const { return status_message_; }

private:
    struct Impl;
    Impl* impl_{nullptr};
    bool playing_{false};
    bool paused_{false};
    uint32_t width_{0};
    uint32_t height_{0};
    uint64_t frame_serial_{0};
    double clock_sec_{0.0};
    double duration_sec_{0.0};
    std::string label_;
    std::string status_message_{"Idle"};
    std::vector<uint8_t> rgba_frame_;
};

} // namespace fzgx
