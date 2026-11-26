#include "assets/avlz.hpp"
#include "assets/gma_model.hpp"
#include "assets/gx_texture.hpp"
#include "assets/u8_arc.hpp"
#include "audio/audio_engine.hpp"
#include "boot/boot.h"
#include "core/config.hpp"
#include "core/paths.hpp"
#include "disc/disc_image.hpp"
#include "input/input_manager.hpp"
#include "platform/platform_services.hpp"
#include "save/gci_card.hpp"
#include "ui/overlay.hpp"
#include "video/sofdec_player.hpp"
#include "video/vulkan_renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <zstd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

int run_headless_smoke() {
    using namespace fzgx;

    const AppPaths paths = resolve_app_paths();
    std::string err;
    if (!ensure_app_directories(paths, &err)) {
        std::cerr << "[smoke] Directory setup failed: " << err << "\n";
        return 1;
    }

    AppConfig cfg;
    if (!load_config(paths.config_file, cfg, &err)) {
        std::cerr << "[smoke] Config load failed: " << err << "\n";
        return 1;
    }

    GciCardManager card;
    card.mount_slot(0, cfg.card_slot_a_dir.empty() ? paths.card_slot_a_dir : std::filesystem::path(cfg.card_slot_a_dir));
    card.mount_slot(1, cfg.card_slot_b_dir.empty() ? paths.card_slot_b_dir : std::filesystem::path(cfg.card_slot_b_dir));

    InputManager input;
    DiscImage disc;
    AudioEngine audio;
    bind_platform_services(&disc, &input, &card, &audio);

    std::string vk_summary;
    VulkanRenderer::headless_probe(vk_summary);

    std::cout << "[smoke] fzero-gx-pc Milestone 3 headless check\n";
    std::cout << "[smoke] Config dir: " << paths.config_dir << "\n";
    std::cout << "[smoke] RVZ Zstandard: v" << ZSTD_versionString() << "\n";
    std::cout << "[smoke] FFmpeg Sofdec: " << (SofdecPlayer::is_ffmpeg_supported() ? "Enabled" : "Disabled") << "\n";
    std::cout << "[smoke] Card Slot A: " << card.slot_dir(0) << " (" << card.files(0).size() << " .gci)\n";
    std::cout << "[smoke] Card Slot B: " << card.slot_dir(1) << " (" << card.files(1).size() << " .gci)\n";
    std::cout << "[smoke] Vulkan probe: " << vk_summary << "\n";
    return 0;
}

int run_inspect_asset(const char* rom_path, const char* fst_path) {
    using namespace fzgx;

    DiscImage disc;
    std::string err;
    if (!disc.mount(rom_path, {}, &err)) {
        std::cerr << "[inspect] Mount failed: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> bytes;
    if (!disc.read_file(fst_path, bytes)) {
        std::cerr << "[inspect] FST file not found: " << fst_path << "\n";
        return 1;
    }

    std::string path_str(fst_path);
    auto ends_ci = [&](const char* sfx) {
        const size_t sl = std::strlen(sfx);
        if (path_str.size() < sl) return false;
        for (size_t i = 0; i < sl; ++i) {
            if (std::tolower(static_cast<unsigned char>(path_str[path_str.size() - sl + i])) !=
                std::tolower(static_cast<unsigned char>(sfx[i]))) {
                return false;
            }
        }
        return true;
    };

    std::cout << "[inspect] Disc: " << disc.header().game_id << " (" << disc.file_count() << " FST files)\n";
    std::cout << "[inspect] File: " << fst_path << " (" << bytes.size() << " bytes)\n";

    if (ends_ci(".arc") || ends_ci(".arc.lz")) {
        U8Archive arc;
        if (!arc.load(bytes, true, &err)) {
            std::cerr << "[inspect] U8 ARC failed: " << err << "\n";
            return 1;
        }
        std::cout << "[inspect] U8 ARC: " << arc.file_count() << " files\n";
        for (const auto& e : arc.entries()) {
            if (e.is_dir) continue;
            std::cout << "  - " << e.path << " (" << e.size << " bytes)\n";
            if (e.path.ends_with(".gma") || e.path.ends_with(".gma.lz")) {
                GmaArchive gma;
                if (gma.load(arc.entry_bytes(e), true, &err)) {
                    std::cout << "      GMA: " << gma.valid_model_count() << " models, "
                              << gma.total_submesh_count() << " submeshes, "
                              << gma.total_triangle_count() << " tris\n";
                } else {
                    std::cout << "      GMA ERR: " << err << "\n";
                }
            } else if (e.path.ends_with(".tpl") || e.path.ends_with(".tpl.lz")) {
                TplArchive tpl;
                if (tpl.load(arc.entry_bytes(e), true, &err)) {
                    std::cout << "      TPL: " << tpl.valid_texture_count() << "/"
                              << tpl.textures().size() << " valid textures\n";
                } else {
                    std::cout << "      TPL ERR: " << err << "\n";
                }
            }
        }
        return 0;
    }

    if (ends_ci(".gma") || ends_ci(".gma.lz")) {
        GmaArchive gma;
        if (!gma.load(bytes, true, &err)) {
            std::cerr << "[inspect] GMA failed: " << err << "\n";
            return 1;
        }
        std::cout << "[inspect] GMA: " << gma.valid_model_count() << "/" << gma.models().size()
                  << " valid models, " << gma.total_submesh_count() << " submeshes, "
                  << gma.total_triangle_count() << " triangles\n";
        return 0;
    }

    if (ends_ci(".tpl") || ends_ci(".tpl.lz")) {
        TplArchive tpl;
        if (!tpl.load(bytes, true, &err)) {
            std::cerr << "[inspect] TPL failed: " << err << "\n";
            return 1;
        }
        std::cout << "[inspect] TPL: " << tpl.valid_texture_count() << "/" << tpl.textures().size()
                  << " valid textures\n";
        return 0;
    }

    if (ends_ci(".lz")) {
        std::vector<uint8_t> out;
        if (!decompress_avlz(bytes, out, &err)) {
            std::cerr << "[inspect] AV-LZSS failed: " << err << "\n";
            return 1;
        }
        std::cout << "[inspect] AV-LZSS decompressed: " << out.size() << " bytes\n";
        return 0;
    }

    return 0;
}

// One host frame: SDL events, input, overlay, FMV, render. Runs from the idle loop, and from
// VIWaitForRetrace (retrace hook) while a booted game owns the thread.
class HostLoop {
public:
    HostLoop(SDL_Window* window,
             fzgx::AppConfig& config,
             fzgx::InputManager& input,
             fzgx::AudioEngine& audio,
             fzgx::DiscImage& disc,
             fzgx::SofdecPlayer& sofdec,
             fzgx::VulkanRenderer& renderer,
             fzgx::OverlayUI& overlay)
        : window_(window), config_(config), input_(input), audio_(audio), disc_(disc), sofdec_(sofdec),
          renderer_(renderer), overlay_(overlay) {}

    // Returns false once the user asked to quit. `pace` applies the configured FPS cap; a
    // running game is paced by VIWaitForRetrace instead.
    bool run_frame(bool pace) {
        using namespace fzgx;

        const auto frame_start = std::chrono::steady_clock::now();
        const double dt_s = std::chrono::duration<double>(frame_start - last_tp_).count();
        last_tp_ = frame_start;

        float orbit_dx = 0.0f;
        float orbit_dy = 0.0f;
        float pan_dx = 0.0f;
        float pan_dy = 0.0f;
        float wheel_delta = 0.0f;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            input_.handle_sdl_event(ev);

            if (ev.type == SDL_EVENT_QUIT) {
                quit_ = true;
            } else if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       ev.type == SDL_EVENT_WINDOW_RESIZED) {
                renderer_.on_window_resized();
                int w = 0;
                int h = 0;
                SDL_GetWindowSize(window_, &w, &h);
                if (!config_.fullscreen && w > 0 && h > 0) {
                    config_.window_width = w;
                    config_.window_height = h;
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_F1) {
                    overlay_.toggle_overlay();
                } else if (ev.key.scancode == SDL_SCANCODE_F11) {
                    config_.fullscreen = !config_.fullscreen;
                    SDL_SetWindowFullscreen(window_, config_.fullscreen);
                }
            } else if (!overlay_.is_overlay_open() && renderer_.gx_backend().has_scene() &&
                       !ImGui::GetIO().WantCaptureMouse) {
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if ((ev.motion.state & SDL_BUTTON_LMASK) != 0) {
                        orbit_dx += ev.motion.xrel;
                        orbit_dy += ev.motion.yrel;
                    } else if ((ev.motion.state & (SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) != 0) {
                        pan_dx += ev.motion.xrel;
                        pan_dy += ev.motion.yrel;
                    }
                } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    wheel_delta += ev.wheel.y;
                }
            }
        }

        if (input_.poll_overlay_gamepad_toggle()) {
            overlay_.toggle_overlay();
        }

        overlay_.tick_background_tasks();
        input_.poll(config_);

        if (!overlay_.is_overlay_open() && renderer_.gx_backend().has_scene()) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            float mf = 0.0f, mr = 0.0f, mu = 0.0f;
            if (keys && !ImGui::GetIO().WantCaptureKeyboard) {
                if (keys[SDL_SCANCODE_W]) mf += 1.0f;
                if (keys[SDL_SCANCODE_S]) mf -= 1.0f;
                if (keys[SDL_SCANCODE_D]) mr += 1.0f;
                if (keys[SDL_SCANCODE_A]) mr -= 1.0f;
                if (keys[SDL_SCANCODE_E]) mu += 1.0f;
                if (keys[SDL_SCANCODE_Q]) mu -= 1.0f;
            }
            renderer_.gx_backend().update_camera(
                dt_s, input_.port_state(0),
                orbit_dx, orbit_dy, pan_dx, pan_dy, wheel_delta,
                mf, mr, mu);
        }

        if (sofdec_.is_playing()) {
            was_fmv_playing_ = true;
            if (sofdec_.update(dt_s, audio_) && sofdec_.frame_serial() != last_fmv_serial_) {
                last_fmv_serial_ = sofdec_.frame_serial();
                renderer_.upload_fmv_frame(sofdec_.latest_rgba().data(), sofdec_.width(), sofdec_.height());
            }
        } else if (was_fmv_playing_) {
            was_fmv_playing_ = false;
            last_fmv_serial_ = 0;
            renderer_.clear_fmv_frame();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        overlay_.draw();
        ImGui::Render();

        const double elapsed_s = std::chrono::duration<double>(frame_start - start_tp_).count();
        renderer_.render_frame(
            config_,
            input_.port_state(0),
            disc_.is_mounted(),
            elapsed_s,
            true);

        if (pace && config_.target_fps > 0 && config_.vsync != VsyncMode::Fifo) {
            const auto target_dur = std::chrono::duration<double>(1.0 / static_cast<double>(config_.target_fps));
            const auto elapsed_frame = std::chrono::steady_clock::now() - frame_start;
            if (elapsed_frame < target_dur) {
                std::this_thread::sleep_for(target_dur - elapsed_frame);
            }
        }
        return !quit_ && !overlay_.should_quit();
    }

private:
    SDL_Window* window_;
    fzgx::AppConfig& config_;
    fzgx::InputManager& input_;
    fzgx::AudioEngine& audio_;
    fzgx::DiscImage& disc_;
    fzgx::SofdecPlayer& sofdec_;
    fzgx::VulkanRenderer& renderer_;
    fzgx::OverlayUI& overlay_;
    std::chrono::steady_clock::time_point start_tp_{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point last_tp_{start_tp_};
    uint64_t last_fmv_serial_{0};
    bool was_fmv_playing_{false};
    bool quit_{false};
};

// Boots the native main.dol path. The game owns the thread until its entry returns; each
// VIWaitForRetrace runs one host frame. Quitting mid-game ends the process from the hook,
// since retail game code has no way back to the idle loop.
void run_game_boot(HostLoop& host, fzgx::OverlayUI& overlay, const std::function<void()>& shutdown_app) {
    overlay.set_game_running(true);
    overlay.set_boot_status({});
    fzgx::set_vi_retrace_hook([&host, &shutdown_app] {
        if (!host.run_frame(false)) {
            shutdown_app();
            std::exit(0);
        }
    });
    const FzgxBootResult result = fzgx_boot_main(fzgx_main_module_entry);
    fzgx::set_vi_retrace_hook({});
    overlay.set_game_running(false);

    std::string status;
    switch (result) {
    case FZGX_BOOT_NO_DISC:
        status = "Boot failed: no disc image is mounted.";
        break;
    case FZGX_BOOT_NO_ENTRY:
        status = "SDK booted in retail order (OSInit, DVDInit, VIInit, PADInit). main_rel does not "
                 "compile natively yet, so there is no game entry to run.";
        break;
    case FZGX_BOOT_ENTRY_RETURNED:
        status = "Game entry returned.";
        break;
    }
    std::cout << "[boot] " << status << "\n";
    overlay.set_boot_status(std::move(status));
}

} // namespace

int main(int argc, char* argv[]) {
    bool boot_on_start = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless-smoke") == 0) {
            return run_headless_smoke();
        }
        if (std::strcmp(argv[i], "--inspect-asset") == 0 && i + 2 < argc) {
            return run_inspect_asset(argv[i + 1], argv[i + 2]);
        }
        if (std::strcmp(argv[i], "--boot") == 0) {
            boot_on_start = true;
        }
    }

    using namespace fzgx;

    const AppPaths paths = resolve_app_paths();
    std::string err;
    ensure_app_directories(paths, &err);

    AppConfig config;
    load_config(paths.config_file, config, &err);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_WindowFlags win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN;
    }

    SDL_Window* window = SDL_CreateWindow(
        "F-Zero GX PC (Milestone 3)",
        config.window_width,
        config.window_height,
        win_flags);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    GciCardManager card;
    card.mount_slot(0, config.card_slot_a_dir.empty() ? paths.card_slot_a_dir : std::filesystem::path(config.card_slot_a_dir));
    card.mount_slot(1, config.card_slot_b_dir.empty() ? paths.card_slot_b_dir : std::filesystem::path(config.card_slot_b_dir));

    InputManager input;
    input.refresh_devices();

    AudioEngine audio;
    if (!audio.initialize(&err)) {
        std::cerr << "AudioEngine warning: " << err << "\n";
    }
    audio.apply_config(config);

    DiscImage disc;
    SofdecPlayer sofdec;
    bind_platform_services(&disc, &input, &card, &audio);

    VulkanRenderer renderer;
    if (!renderer.initialize(window, config, &err)) {
        std::cerr << "VulkanRenderer::initialize failed: " << err << "\n";
        audio.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    OverlayUI overlay(window, paths, config, disc, card, input, audio, sofdec, renderer);
    HostLoop host(window, config, input, audio, disc, sofdec, renderer, overlay);

    const std::function<void()> shutdown_app = [&] {
        sofdec.close();
        audio.shutdown();
        save_config(paths.config_file, config);
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    while (host.run_frame(true)) {
        if (overlay.consume_boot_request() || std::exchange(boot_on_start, false)) {
            run_game_boot(host, overlay, shutdown_app);
        }
    }

    shutdown_app();
    return 0;
}
