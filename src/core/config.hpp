#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fzgx {

enum class AspectRatioMode {
    Auto = 0,
    Ratio4_3 = 1,
    Ratio16_9 = 2,
    Ratio16_10 = 3,
    Ratio21_9 = 4,
};

enum class HudLayoutMode {
    SafeCentered = 0,
    EdgeAnchored = 1,
};

enum class VsyncMode {
    Off = 0,
    Fifo = 1,
    Mailbox = 2,
};

enum class ShaderBackendMode {
    None = 0,
    BuiltinCrt = 1,
    Librashader = 2,
};

struct BuiltinCrtSettings {
    float scanline_strength{0.35f};
    float mask_strength{0.20f};
    float curvature{0.08f};
    float vignette{0.15f};
    float brightness{1.12f};
};

struct ControllerPortConfig {
    bool enabled{true};
    std::string device_name{"Auto"};
    bool prefer_gc_adapter{false};
    float stick_deadzone{0.12f};
    float stick_outer_gate{0.95f};
    float cstick_deadzone{0.15f};
    float trigger_analog_deadzone{0.05f};
    float trigger_digital_threshold{0.85f};
    bool rumble_enabled{true};
    // Maps logical GC button index (0..11) to SDL_GamepadButton integer value (-1 = default)
    std::vector<int> button_map{};
};

struct AppConfig {
    // ROM & Paths
    std::string rom_path{};
    std::string mod_override_dir{};
    std::string card_slot_a_dir{};
    std::string card_slot_b_dir{};
    bool first_run_completed{false};

    // Video & Aspect
    int internal_resolution_scale{2}; // 1x..8x (1x = 640x480 base)
    AspectRatioMode aspect_ratio{AspectRatioMode::Auto};
    HudLayoutMode hud_mode{HudLayoutMode::SafeCentered};
    int target_fps{60}; // 60, 120, 144, 240, 0 = Uncapped
    bool frame_interpolation{true};
    VsyncMode vsync{VsyncMode::Fifo};
    bool fullscreen{false};
    int window_width{1280};
    int window_height{720};

    // Shaders & Post-FX
    ShaderBackendMode shader_backend{ShaderBackendMode::BuiltinCrt};
    std::string librashader_dll_path{};
    std::string librashader_preset_path{};
    bool librashader_feed_480p{true};
    std::map<std::string, float> librashader_params{};
    BuiltinCrtSettings builtin_crt{};
    bool fsr_enabled{false};
    float fsr_sharpness{0.75f}; // 0.0 (soft) .. 1.0 (crisp RCAS sharpening)

    // Audio & FMV
    float master_volume{1.0f};
    float music_volume{0.85f};
    float sfx_volume{0.90f};
    bool mute_audio{false};

    // Overlay & Misc
    std::string overlay_hotkey{"F1"};
    bool pause_when_overlay_open{true};
    bool show_fps_hud{true};

    // Controllers (Ports 1..4)
    std::vector<ControllerPortConfig> controllers{};
};

AppConfig make_default_config();
bool load_config(const std::filesystem::path& file_path, AppConfig& out_config, std::string* error_out = nullptr);
bool save_config(const std::filesystem::path& file_path, const AppConfig& config, std::string* error_out = nullptr);

const char* to_string(AspectRatioMode mode);
const char* to_string(HudLayoutMode mode);
const char* to_string(VsyncMode mode);
const char* to_string(ShaderBackendMode mode);

float resolve_aspect_ratio(AspectRatioMode mode, uint32_t viewport_width, uint32_t viewport_height);

} // namespace fzgx
