#include "assets/avlz.hpp"
#include "assets/gma_model.hpp"
#include "assets/gx_texture.hpp"
#include "assets/u8_arc.hpp"
#include "audio/audio_engine.hpp"
#include "core/config.hpp"
#include "core/paths.hpp"
#include "disc/disc_image.hpp"
#include "input/input_manager.hpp"
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
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace fzgx {
void bind_platform_services(DiscImage* disc, InputManager* input, GciCardManager* card, AudioEngine* audio);
} // namespace fzgx

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

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless-smoke") == 0) {
            return run_headless_smoke();
        }
        if (std::strcmp(argv[i], "--inspect-asset") == 0 && i + 2 < argc) {
            return run_inspect_asset(argv[i + 1], argv[i + 2]);
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

    bool running = true;
    const auto start_tp = std::chrono::steady_clock::now();
    auto last_tp = start_tp;
    uint64_t last_fmv_serial = 0;
    bool was_fmv_playing = false;

    while (running && !overlay.should_quit()) {
        const auto frame_start = std::chrono::steady_clock::now();
        const double dt_s = std::chrono::duration<double>(frame_start - last_tp).count();
        last_tp = frame_start;

        float orbit_dx = 0.0f;
        float orbit_dy = 0.0f;
        float pan_dx = 0.0f;
        float pan_dy = 0.0f;
        float wheel_delta = 0.0f;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            input.handle_sdl_event(ev);

            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       ev.type == SDL_EVENT_WINDOW_RESIZED) {
                renderer.on_window_resized();
                int w = 0;
                int h = 0;
                SDL_GetWindowSize(window, &w, &h);
                if (!config.fullscreen && w > 0 && h > 0) {
                    config.window_width = w;
                    config.window_height = h;
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                if (ev.key.scancode == SDL_SCANCODE_F1) {
                    overlay.toggle_overlay();
                } else if (ev.key.scancode == SDL_SCANCODE_F11) {
                    config.fullscreen = !config.fullscreen;
                    SDL_SetWindowFullscreen(window, config.fullscreen);
                }
            } else if (!overlay.is_overlay_open() && renderer.gx_backend().has_scene() &&
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

        if (input.poll_overlay_gamepad_toggle()) {
            overlay.toggle_overlay();
        }

        overlay.tick_background_tasks();
        input.poll(config);

        if (!overlay.is_overlay_open() && renderer.gx_backend().has_scene()) {
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
            renderer.gx_backend().update_camera(
                dt_s, input.port_state(0),
                orbit_dx, orbit_dy, pan_dx, pan_dy, wheel_delta,
                mf, mr, mu);
        }

        if (sofdec.is_playing()) {
            was_fmv_playing = true;
            if (sofdec.update(dt_s, audio) && sofdec.frame_serial() != last_fmv_serial) {
                last_fmv_serial = sofdec.frame_serial();
                renderer.upload_fmv_frame(sofdec.latest_rgba().data(), sofdec.width(), sofdec.height());
            }
        } else if (was_fmv_playing) {
            was_fmv_playing = false;
            last_fmv_serial = 0;
            renderer.clear_fmv_frame();
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        overlay.draw();
        ImGui::Render();

        const double elapsed_s = std::chrono::duration<double>(frame_start - start_tp).count();
        renderer.render_frame(
            config,
            input.port_state(0),
            disc.is_mounted(),
            elapsed_s,
            true);

        if (config.target_fps > 0 && config.vsync != VsyncMode::Fifo) {
            const auto target_dur = std::chrono::duration<double>(1.0 / static_cast<double>(config.target_fps));
            const auto elapsed_frame = std::chrono::steady_clock::now() - frame_start;
            if (elapsed_frame < target_dur) {
                std::this_thread::sleep_for(target_dur - elapsed_frame);
            }
        }
    }

    sofdec.close();
    audio.shutdown();
    save_config(paths.config_file, config);
    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
