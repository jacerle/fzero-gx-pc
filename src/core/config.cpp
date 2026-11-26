#include "core/config.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fzgx {

using json = nlohmann::json;

namespace {

template <typename Enum>
Enum enum_from_int(int v, Enum fallback, int min_v, int max_v) {
    if (v < min_v || v > max_v) {
        return fallback;
    }
    return static_cast<Enum>(v);
}

} // namespace

AppConfig make_default_config() {
    AppConfig cfg;
    cfg.controllers.resize(4);
    for (size_t i = 0; i < 4; ++i) {
        cfg.controllers[i].enabled = (i == 0);
        cfg.controllers[i].button_map.assign(12, -1);
    }
    return cfg;
}

bool load_config(const std::filesystem::path& file_path, AppConfig& out_config, std::string* error_out) {
    out_config = make_default_config();
    if (!std::filesystem::exists(file_path)) {
        return true; // Missing config is valid on first run.
    }

    std::ifstream in(file_path);
    if (!in) {
        if (error_out) {
            *error_out = "Unable to open config file: " + file_path.string();
        }
        return false;
    }

    try {
        json j;
        in >> j;

        out_config.rom_path = j.value("rom_path", out_config.rom_path);
        out_config.mod_override_dir = j.value("mod_override_dir", out_config.mod_override_dir);
        out_config.card_slot_a_dir = j.value("card_slot_a_dir", out_config.card_slot_a_dir);
        out_config.card_slot_b_dir = j.value("card_slot_b_dir", out_config.card_slot_b_dir);
        out_config.first_run_completed = j.value("first_run_completed", out_config.first_run_completed);

        out_config.internal_resolution_scale =
            std::clamp(j.value("internal_resolution_scale", out_config.internal_resolution_scale), 1, 8);
        out_config.aspect_ratio = enum_from_int(
            j.value("aspect_ratio", static_cast<int>(out_config.aspect_ratio)),
            AspectRatioMode::Auto, 0, 4);
        out_config.hud_mode = enum_from_int(
            j.value("hud_mode", static_cast<int>(out_config.hud_mode)),
            HudLayoutMode::SafeCentered, 0, 1);
        out_config.target_fps = std::clamp(j.value("target_fps", out_config.target_fps), 0, 1000);
        out_config.frame_interpolation = j.value("frame_interpolation", out_config.frame_interpolation);
        out_config.vsync = enum_from_int(
            j.value("vsync", static_cast<int>(out_config.vsync)),
            VsyncMode::Fifo, 0, 2);
        out_config.fullscreen = j.value("fullscreen", out_config.fullscreen);
        out_config.window_width = std::clamp(j.value("window_width", out_config.window_width), 320, 7680);
        out_config.window_height = std::clamp(j.value("window_height", out_config.window_height), 240, 4320);

        out_config.shader_backend = enum_from_int(
            j.value("shader_backend", static_cast<int>(out_config.shader_backend)),
            ShaderBackendMode::BuiltinCrt, 0, 2);
        out_config.librashader_dll_path =
            j.value("librashader_dll_path", out_config.librashader_dll_path);
        out_config.librashader_preset_path =
            j.value("librashader_preset_path", out_config.librashader_preset_path);
        out_config.librashader_feed_480p =
            j.value("librashader_feed_480p", out_config.librashader_feed_480p);

        if (j.contains("librashader_params") && j["librashader_params"].is_object()) {
            out_config.librashader_params.clear();
            for (const auto& [k, v] : j["librashader_params"].items()) {
                if (v.is_number()) {
                    out_config.librashader_params[k] = v.get<float>();
                }
            }
        }

        if (j.contains("builtin_crt") && j["builtin_crt"].is_object()) {
            const auto& c = j["builtin_crt"];
            out_config.builtin_crt.scanline_strength =
                std::clamp(c.value("scanline_strength", out_config.builtin_crt.scanline_strength), 0.0f, 1.0f);
            out_config.builtin_crt.mask_strength =
                std::clamp(c.value("mask_strength", out_config.builtin_crt.mask_strength), 0.0f, 1.0f);
            out_config.builtin_crt.curvature =
                std::clamp(c.value("curvature", out_config.builtin_crt.curvature), 0.0f, 0.35f);
            out_config.builtin_crt.vignette =
                std::clamp(c.value("vignette", out_config.builtin_crt.vignette), 0.0f, 1.0f);
            out_config.builtin_crt.brightness =
                std::clamp(c.value("brightness", out_config.builtin_crt.brightness), 0.5f, 2.0f);
        }

        if (j.contains("fsr_enabled")) {
            out_config.fsr_enabled = j.value("fsr_enabled", out_config.fsr_enabled);
        } else {
            out_config.fsr_enabled = j.value("fsr_enabled_placeholder", out_config.fsr_enabled);
        }
        out_config.fsr_sharpness =
            std::clamp(j.value("fsr_sharpness", out_config.fsr_sharpness), 0.0f, 1.0f);

        out_config.master_volume = std::clamp(j.value("master_volume", out_config.master_volume), 0.0f, 1.0f);
        out_config.music_volume = std::clamp(j.value("music_volume", out_config.music_volume), 0.0f, 1.0f);
        out_config.sfx_volume = std::clamp(j.value("sfx_volume", out_config.sfx_volume), 0.0f, 1.0f);
        out_config.mute_audio = j.value("mute_audio", out_config.mute_audio);

        out_config.overlay_hotkey = j.value("overlay_hotkey", out_config.overlay_hotkey);
        out_config.pause_when_overlay_open =
            j.value("pause_when_overlay_open", out_config.pause_when_overlay_open);
        out_config.show_fps_hud = j.value("show_fps_hud", out_config.show_fps_hud);

        if (j.contains("controllers") && j["controllers"].is_array()) {
            const size_t count = std::min<size_t>(j["controllers"].size(), 4);
            for (size_t i = 0; i < count; ++i) {
                const auto& cj = j["controllers"][i];
                auto& dst = out_config.controllers[i];
                dst.enabled = cj.value("enabled", dst.enabled);
                dst.device_name = cj.value("device_name", dst.device_name);
                dst.prefer_gc_adapter = cj.value("prefer_gc_adapter", dst.prefer_gc_adapter);
                dst.stick_deadzone = std::clamp(cj.value("stick_deadzone", dst.stick_deadzone), 0.0f, 0.5f);
                dst.stick_outer_gate = std::clamp(cj.value("stick_outer_gate", dst.stick_outer_gate), 0.5f, 1.2f);
                dst.cstick_deadzone = std::clamp(cj.value("cstick_deadzone", dst.cstick_deadzone), 0.0f, 0.5f);
                dst.trigger_analog_deadzone =
                    std::clamp(cj.value("trigger_analog_deadzone", dst.trigger_analog_deadzone), 0.0f, 0.5f);
                dst.trigger_digital_threshold =
                    std::clamp(cj.value("trigger_digital_threshold", dst.trigger_digital_threshold), 0.1f, 1.0f);
                dst.rumble_enabled = cj.value("rumble_enabled", dst.rumble_enabled);
                if (cj.contains("button_map") && cj["button_map"].is_array()) {
                    const size_t map_count = std::min<size_t>(cj["button_map"].size(), 12);
                    for (size_t b = 0; b < map_count; ++b) {
                        if (cj["button_map"][b].is_number_integer()) {
                            dst.button_map[b] = cj["button_map"][b].get<int>();
                        }
                    }
                }
            }
        }
        return true;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to parse config JSON: ") + ex.what();
        }
        return false;
    }
}

bool save_config(const std::filesystem::path& file_path, const AppConfig& config, std::string* error_out) {
    try {
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path());
        }

        json controllers_json = json::array();
        for (size_t i = 0; i < 4; ++i) {
            const auto& c = i < config.controllers.size() ? config.controllers[i] : ControllerPortConfig{};
            controllers_json.push_back({
                {"enabled", c.enabled},
                {"device_name", c.device_name},
                {"prefer_gc_adapter", c.prefer_gc_adapter},
                {"stick_deadzone", c.stick_deadzone},
                {"stick_outer_gate", c.stick_outer_gate},
                {"cstick_deadzone", c.cstick_deadzone},
                {"trigger_analog_deadzone", c.trigger_analog_deadzone},
                {"trigger_digital_threshold", c.trigger_digital_threshold},
                {"rumble_enabled", c.rumble_enabled},
                {"button_map", c.button_map},
            });
        }

        json j = {
            {"schema_version", 1},
            {"rom_path", config.rom_path},
            {"mod_override_dir", config.mod_override_dir},
            {"card_slot_a_dir", config.card_slot_a_dir},
            {"card_slot_b_dir", config.card_slot_b_dir},
            {"first_run_completed", config.first_run_completed},
            {"internal_resolution_scale", config.internal_resolution_scale},
            {"aspect_ratio", static_cast<int>(config.aspect_ratio)},
            {"hud_mode", static_cast<int>(config.hud_mode)},
            {"target_fps", config.target_fps},
            {"frame_interpolation", config.frame_interpolation},
            {"vsync", static_cast<int>(config.vsync)},
            {"fullscreen", config.fullscreen},
            {"window_width", config.window_width},
            {"window_height", config.window_height},
            {"shader_backend", static_cast<int>(config.shader_backend)},
            {"librashader_dll_path", config.librashader_dll_path},
            {"librashader_preset_path", config.librashader_preset_path},
            {"librashader_feed_480p", config.librashader_feed_480p},
            {"librashader_params", config.librashader_params},
            {"builtin_crt", {
                {"scanline_strength", config.builtin_crt.scanline_strength},
                {"mask_strength", config.builtin_crt.mask_strength},
                {"curvature", config.builtin_crt.curvature},
                {"vignette", config.builtin_crt.vignette},
                {"brightness", config.builtin_crt.brightness},
            }},
            {"fsr_enabled", config.fsr_enabled},
            {"fsr_sharpness", config.fsr_sharpness},
            {"master_volume", config.master_volume},
            {"music_volume", config.music_volume},
            {"sfx_volume", config.sfx_volume},
            {"mute_audio", config.mute_audio},
            {"overlay_hotkey", config.overlay_hotkey},
            {"pause_when_overlay_open", config.pause_when_overlay_open},
            {"show_fps_hud", config.show_fps_hud},
            {"controllers", controllers_json},
        };

        std::ofstream out(file_path);
        if (!out) {
            if (error_out) {
                *error_out = "Unable to write config file: " + file_path.string();
            }
            return false;
        }
        out << j.dump(2) << '\n';
        return true;
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = std::string("Failed to save config JSON: ") + ex.what();
        }
        return false;
    }
}

const char* to_string(AspectRatioMode mode) {
    switch (mode) {
    case AspectRatioMode::Auto: return "Auto (Match Window)";
    case AspectRatioMode::Ratio4_3: return "4:3 (Original)";
    case AspectRatioMode::Ratio16_9: return "16:9 (Widescreen)";
    case AspectRatioMode::Ratio16_10: return "16:10";
    case AspectRatioMode::Ratio21_9: return "21:9 (Ultrawide)";
    }
    return "Unknown";
}

const char* to_string(HudLayoutMode mode) {
    switch (mode) {
    case HudLayoutMode::SafeCentered: return "Safe Centered (4:3/16:9)";
    case HudLayoutMode::EdgeAnchored: return "Edge Anchored";
    }
    return "Unknown";
}

const char* to_string(VsyncMode mode) {
    switch (mode) {
    case VsyncMode::Off: return "Off (Immediate)";
    case VsyncMode::Fifo: return "VSync (FIFO)";
    case VsyncMode::Mailbox: return "Fast Sync (Mailbox)";
    }
    return "Unknown";
}

const char* to_string(ShaderBackendMode mode) {
    switch (mode) {
    case ShaderBackendMode::None: return "None (Sharp)";
    case ShaderBackendMode::BuiltinCrt: return "Built-in CRT Shader";
    case ShaderBackendMode::Librashader: return "librashader (.slangp)";
    }
    return "Unknown";
}

float resolve_aspect_ratio(AspectRatioMode mode, uint32_t viewport_width, uint32_t viewport_height) {
    switch (mode) {
    case AspectRatioMode::Ratio4_3: return 4.0f / 3.0f;
    case AspectRatioMode::Ratio16_9: return 16.0f / 9.0f;
    case AspectRatioMode::Ratio16_10: return 16.0f / 10.0f;
    case AspectRatioMode::Ratio21_9: return 21.0f / 9.0f;
    case AspectRatioMode::Auto:
    default:
        if (viewport_height == 0) {
            return 16.0f / 9.0f;
        }
        return static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    }
}

} // namespace fzgx
