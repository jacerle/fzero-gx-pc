#pragma once

#include "assets/gma_model.hpp"
#include "assets/gx_texture.hpp"
#include "assets/u8_arc.hpp"
#include "audio/audio_engine.hpp"
#include "core/config.hpp"
#include "core/paths.hpp"
#include "disc/disc_image.hpp"
#include "game/dol_rel_loader.hpp"
#include "input/input_manager.hpp"
#include "save/gci_card.hpp"
#include "video/sofdec_player.hpp"
#include "video/vulkan_renderer.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fzgx {

class OverlayUI {
public:
    OverlayUI(SDL_Window* window,
              AppPaths paths,
              AppConfig& config,
              DiscImage& disc,
              GciCardManager& card,
              InputManager& input,
              AudioEngine& audio,
              SofdecPlayer& sofdec,
              VulkanRenderer& renderer);
    ~OverlayUI();

    bool is_overlay_open() const { return overlay_open_; }
    void set_overlay_open(bool open) { overlay_open_ = open; }
    void toggle_overlay() { overlay_open_ = !overlay_open_; }

    bool should_quit() const { return quit_requested_; }

    void draw();
    void tick_background_tasks();

    // Boot control. The overlay only requests a boot; main.cpp runs it because the game takes
    // over the host loop (the host frame runs from VIWaitForRetrace while the game is running).
    bool consume_boot_request();
    void set_game_running(bool running) { game_running_ = running; }
    void set_boot_status(std::string status) { boot_status_ = std::move(status); }

private:
    void draw_first_run_wizard();
    void draw_settings_window();
    void draw_hud_badge();

    void tab_general_rom();
    void tab_disc_assets();
    void tab_gx_viewer();
    void tab_video_aspect();
    void tab_shaders();
    void tab_controllers();
    void tab_audio_fmv();

    void open_gma_bytes(std::span<const uint8_t> gma_bytes,
                        const std::optional<std::vector<uint8_t>>& tpl_bytes,
                        const std::string& label);
    void open_tpl_bytes(std::span<const uint8_t> tpl_bytes, const std::string& label);
    void open_arc_bytes(std::span<const uint8_t> arc_bytes, const std::string& label);

    void start_rom_file_picker();
    void start_mod_dir_picker();
    void start_preset_file_picker();
    void start_librashader_lib_picker();
    void start_sha1_verification(bool logical_disc_stream);
    void try_mount_configured_rom();

    SDL_Window* window_{nullptr};
    AppPaths paths_;
    AppConfig& config_;
    DiscImage& disc_;
    GciCardManager& card_;
    InputManager& input_;
    AudioEngine& audio_;
    SofdecPlayer& sofdec_;
    VulkanRenderer& renderer_;

    bool overlay_open_{false};
    bool quit_requested_{false};
    bool config_dirty_{false};
    bool boot_requested_{false};
    bool game_running_{false};
    std::string boot_status_;
    std::string status_banner_;
    std::string error_banner_;
    std::string asset_action_status_;

    // Parsed executable & asset inspector state
    std::optional<DolInfo> dol_info_;
    std::optional<RelInfo> inspected_rel_;
    std::string inspected_rel_name_;
    std::optional<U8Archive> open_arc_;
    std::string open_arc_name_;
    std::optional<TplArchive> inspected_tpl_;
    std::string inspected_tpl_name_;

    // Asset browser filter state
    char asset_filter_[128]{};
    int asset_category_{0}; // 0=All, 1=.gma, 2=.tpl, 3=.arc, 4=.adx, 5=.sfd, 6=.rel

    // Async SHA-1 state
    std::jthread sha1_thread_;
    std::mutex sha1_mutex_;
    std::atomic<bool> sha1_running_{false};
    std::atomic<float> sha1_progress_{0.0f};
    std::string sha1_result_hex_;
    std::string sha1_error_;

    // Pending path picks delivered from SDL3 async file dialog callback
    std::mutex dialog_mutex_;
    std::string pending_rom_pick_;
    std::string pending_mod_dir_pick_;
    std::string pending_preset_pick_;
    std::string pending_libra_lib_pick_;

    int selected_port_{0};
};

} // namespace fzgx
