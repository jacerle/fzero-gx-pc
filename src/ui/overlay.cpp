#include "ui/overlay.hpp"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace fzgx {

namespace {

void copy_string_to_buf(const std::string& src, char* dst, size_t dst_size) {
    if (dst_size == 0) return;
    const size_t n = std::min(src.size(), dst_size - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool icontains(const std::string& haystack, const char* needle) {
    if (!needle || needle[0] == '\0') return true;
    std::string h = haystack;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t sl = std::strlen(suffix);
    if (s.size() < sl) return false;
    for (size_t i = 0; i < sl; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - sl + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

bool has_asset_ext(const std::string& path, const char* ext_no_lz, const char* ext_lz) {
    return ends_with_ci(path, ext_no_lz) || ends_with_ci(path, ext_lz);
}

std::string make_companion_tpl_stem(std::string gma_path) {
    if (ends_with_ci(gma_path, ".gma.lz")) {
        gma_path.resize(gma_path.size() - 7);
    } else if (ends_with_ci(gma_path, ".gma")) {
        gma_path.resize(gma_path.size() - 4);
    }
    return gma_path;
}

} // namespace

OverlayUI::OverlayUI(SDL_Window* window,
                     AppPaths paths,
                     AppConfig& config,
                     DiscImage& disc,
                     GciCardManager& card,
                     InputManager& input,
                     AudioEngine& audio,
                     SofdecPlayer& sofdec,
                     VulkanRenderer& renderer)
    : window_(window),
      paths_(std::move(paths)),
      config_(config),
      disc_(disc),
      card_(card),
      input_(input),
      audio_(audio),
      sofdec_(sofdec),
      renderer_(renderer) {
    if (!config_.first_run_completed) {
        overlay_open_ = true;
    } else if (!config_.rom_path.empty()) {
        try_mount_configured_rom();
    }
}

OverlayUI::~OverlayUI() {
    if (sha1_thread_.joinable()) {
        sha1_thread_.request_stop();
        sha1_thread_.join();
    }
}

void OverlayUI::tick_background_tasks() {
    std::string rom_pick;
    std::string mod_pick;
    std::string preset_pick;
    std::string libra_pick;
    {
        std::lock_guard lock(dialog_mutex_);
        rom_pick.swap(pending_rom_pick_);
        mod_pick.swap(pending_mod_dir_pick_);
        preset_pick.swap(pending_preset_pick_);
        libra_pick.swap(pending_libra_lib_pick_);
    }

    if (!rom_pick.empty()) {
        config_.rom_path = rom_pick;
        config_dirty_ = true;
        try_mount_configured_rom();
    }
    if (!mod_pick.empty()) {
        config_.mod_override_dir = mod_pick;
        config_dirty_ = true;
        if (!config_.rom_path.empty()) {
            try_mount_configured_rom();
        }
    }
    if (!preset_pick.empty()) {
        config_.librashader_preset_path = preset_pick;
        config_.shader_backend = ShaderBackendMode::Librashader;
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
        renderer_.reload_librashader(config_);
    }
    if (!libra_pick.empty()) {
        config_.librashader_dll_path = libra_pick;
        config_dirty_ = true;
        renderer_.reload_librashader(config_);
    }
}

void OverlayUI::try_mount_configured_rom() {
    error_banner_.clear();
    status_banner_.clear();
    dol_info_.reset();
    inspected_rel_.reset();
    inspected_rel_name_.clear();
    if (config_.rom_path.empty()) {
        return;
    }
    std::string err;
    if (disc_.mount(config_.rom_path, config_.mod_override_dir, &err)) {
        const auto& h = disc_.header();
        status_banner_ = "Mounted " + h.game_id + " (" + h.container_format + "/" +
                         h.compression_name + ") - " + std::to_string(disc_.file_count()) + " FST entries.";
        if (!h.is_fzero_gx_usa) {
            error_banner_ = "Warning: Expected USA GFZE01, found " + h.game_id + ".";
        }
        std::vector<uint8_t> dol_bytes;
        if (disc_.read_dol(dol_bytes)) {
            DolInfo parsed{};
            if (parse_dol_header(dol_bytes, parsed)) {
                dol_info_ = std::move(parsed);
            }
        }
    } else {
        error_banner_ = err;
    }
}

void OverlayUI::start_sha1_verification(bool logical_disc_stream) {
    if (sha1_running_.load()) {
        return;
    }
    if (sha1_thread_.joinable()) {
        sha1_thread_.join();
    }
    sha1_running_.store(true);
    sha1_progress_.store(0.0f);
    {
        std::lock_guard lock(sha1_mutex_);
        sha1_result_hex_.clear();
        sha1_error_.clear();
    }

    const std::filesystem::path raw_path = config_.rom_path;
    sha1_thread_ = std::jthread([this, logical_disc_stream, raw_path](std::stop_token st) {
        std::string hex;
        auto cb = [&](uint64_t done, uint64_t total) -> bool {
            if (total > 0) {
                sha1_progress_.store(static_cast<float>(done) / static_cast<float>(total));
            }
            return !st.stop_requested();
        };
        const bool ok = logical_disc_stream && disc_.is_mounted()
                            ? disc_.compute_logical_sha1(hex, cb)
                            : DiscImage::compute_sha1(raw_path, hex, cb);
        std::lock_guard lock(sha1_mutex_);
        if (ok) {
            sha1_result_hex_ = hex;
        } else if (!st.stop_requested()) {
            sha1_error_ = "Failed computing SHA-1.";
        }
        sha1_running_.store(false);
    });
}

void OverlayUI::start_rom_file_picker() {
    static const SDL_DialogFileFilter filters[] = {
        {"GameCube Disc Images", "iso;gcm;rvz;wia"},
        {"All Files", "*"},
    };
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* filelist, int) {
            if (!userdata || !filelist || !filelist[0]) return;
            auto* self = static_cast<OverlayUI*>(userdata);
            std::lock_guard lock(self->dialog_mutex_);
            self->pending_rom_pick_ = filelist[0];
        },
        this, window_, filters, 2,
        config_.rom_path.empty() ? nullptr : config_.rom_path.c_str(), false);
}

void OverlayUI::start_mod_dir_picker() {
    SDL_ShowOpenFolderDialog(
        [](void* userdata, const char* const* filelist, int) {
            if (!userdata || !filelist || !filelist[0]) return;
            auto* self = static_cast<OverlayUI*>(userdata);
            std::lock_guard lock(self->dialog_mutex_);
            self->pending_mod_dir_pick_ = filelist[0];
        },
        this, window_,
        config_.mod_override_dir.empty() ? nullptr : config_.mod_override_dir.c_str(), false);
}

void OverlayUI::start_preset_file_picker() {
    static const SDL_DialogFileFilter filters[] = {
        {"RetroArch Slang Shader Preset", "slangp"},
        {"All Files", "*"},
    };
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* filelist, int) {
            if (!userdata || !filelist || !filelist[0]) return;
            auto* self = static_cast<OverlayUI*>(userdata);
            std::lock_guard lock(self->dialog_mutex_);
            self->pending_preset_pick_ = filelist[0];
        },
        this, window_, filters, 2,
        paths_.shaders_dir.string().c_str(), false);
}

void OverlayUI::start_librashader_lib_picker() {
    static const SDL_DialogFileFilter filters[] = {
        {"Shared Library", "so;dll;dylib"},
        {"All Files", "*"},
    };
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* filelist, int) {
            if (!userdata || !filelist || !filelist[0]) return;
            auto* self = static_cast<OverlayUI*>(userdata);
            std::lock_guard lock(self->dialog_mutex_);
            self->pending_libra_lib_pick_ = filelist[0];
        },
        this, window_, filters, 2, nullptr, false);
}

void OverlayUI::draw() {
    if (config_.show_fps_hud) {
        draw_hud_badge();
    }
    if (!overlay_open_) {
        return;
    }
    if (!config_.first_run_completed) {
        draw_first_run_wizard();
    } else {
        draw_settings_window();
    }
}

void OverlayUI::draw_hud_badge() {
    const auto& d = renderer_.diagnostics();
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##HudBadge", nullptr, flags)) {
        std::string mode_lbl;
        if (sofdec_.is_playing()) {
            mode_lbl = sofdec_.label();
        } else if (renderer_.gx_backend().has_scene()) {
            const auto& gx = renderer_.gx_backend();
            mode_lbl = gx.scene_label() + " (" + std::to_string(gx.total_triangles()) + " tris)";
        } else if (disc_.is_mounted()) {
            mode_lbl = disc_.header().game_id;
        } else {
            mode_lbl = "Calibration Mode";
        }
        ImGui::Text("FPS: %.1f (%.2f ms) | GX %ux%u | %s | [%s / Guide: Menu]",
                    d.current_fps, d.avg_frame_ms, d.internal_width, d.internal_height,
                    mode_lbl.c_str(), config_.overlay_hotkey.c_str());
    }
    ImGui::End();
}

void OverlayUI::draw_first_run_wizard() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f, 490.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("F-Zero GX PC - First-Run Setup", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "Welcome to F-Zero GX PC (Milestone 2).\n"
            "No copyrighted game assets are distributed with this launcher. "
            "Select your legally dumped F-Zero GX USA (GFZE01) disc image (.rvz / .iso / .gcm) "
            "to mount the disc, play ADX music, and watch Sofdec .sfd FMVs, or continue into Calibration Mode.");
        ImGui::Separator();

        char rom_buf[512]{};
        copy_string_to_buf(config_.rom_path, rom_buf, sizeof(rom_buf));
        if (ImGui::InputText("Disc Image Path", rom_buf, sizeof(rom_buf))) {
            config_.rom_path = rom_buf;
            config_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse ROM...")) {
            start_rom_file_picker();
        }

        if (ImGui::Button("Verify & Mount Disc") && !config_.rom_path.empty()) {
            try_mount_configured_rom();
        }
        ImGui::SameLine();
        if (ImGui::Button("Verify Logical ISO SHA-1") && !config_.rom_path.empty()) {
            start_sha1_verification(disc_.is_mounted());
        }

        if (sha1_running_.load()) {
            ImGui::ProgressBar(sha1_progress_.load(), ImVec2(-1.0f, 0.0f), "Hashing Logical Disc...");
        } else {
            std::lock_guard lock(sha1_mutex_);
            if (!sha1_result_hex_.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Logical ISO SHA-1: %s", sha1_result_hex_.c_str());
            } else if (!sha1_error_.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", sha1_error_.c_str());
            }
        }

        if (!status_banner_.empty()) {
            ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.50f, 1.0f), "%s", status_banner_.c_str());
        }
        if (!error_banner_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", error_banner_.c_str());
        }

        ImGui::Spacing();
        ImGui::Text("Quick Display Preset:");
        int backend_idx = static_cast<int>(config_.shader_backend);
        const char* backends[] = {"None (Sharp)", "Built-in CRT Shader", "librashader (.slangp)"};
        if (ImGui::Combo("CRT / Shader Mode", &backend_idx, backends, 3)) {
            config_.shader_backend = static_cast<ShaderBackendMode>(backend_idx);
            config_dirty_ = true;
            renderer_.apply_video_config(config_);
            renderer_.reload_librashader(config_);
        }

        ImGui::Separator();
        if (disc_.is_mounted()) {
            if (ImGui::Button("Open Disc & Asset Browser", ImVec2(240.0f, 36.0f))) {
                config_.first_run_completed = true;
                save_config(paths_.config_file, config_);
                overlay_open_ = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Continue to Calibration / Test Scene", ImVec2(270.0f, 36.0f))) {
            config_.first_run_completed = true;
            save_config(paths_.config_file, config_);
            overlay_open_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(90.0f, 36.0f))) {
            quit_requested_ = true;
        }
    }
    ImGui::End();
}

void OverlayUI::draw_settings_window() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(880.0f, 620.0f), ImGuiCond_FirstUseEver);

    bool open = overlay_open_;
    if (ImGui::Begin("F-Zero GX PC - Global Settings", &open)) {
        if (ImGui::BeginTabBar("SettingsTabs")) {
            if (ImGui::BeginTabItem("General & ROM")) {
                tab_general_rom();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Disc & Assets")) {
                tab_disc_assets();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("3D GX Viewer")) {
                tab_gx_viewer();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Video & Aspect")) {
                tab_video_aspect();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Shaders & Post-FX")) {
                tab_shaders();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controllers")) {
                tab_controllers();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio & FMV")) {
                tab_audio_fmv();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("Save Config")) {
            std::string err;
            if (save_config(paths_.config_file, config_, &err)) {
                config_dirty_ = false;
                status_banner_ = "Saved config to " + paths_.config_file.string();
            } else {
                error_banner_ = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Defaults")) {
            const std::string kept_rom = config_.rom_path;
            config_ = make_default_config();
            config_.rom_path = kept_rom;
            config_.first_run_completed = true;
            config_dirty_ = true;
            audio_.apply_config(config_);
            renderer_.apply_video_config(config_);
            renderer_.reload_librashader(config_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close Overlay (F1)")) {
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit App")) {
            quit_requested_ = true;
        }
        if (config_dirty_) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "(Unsaved changes)");
        }
    }
    ImGui::End();
    overlay_open_ = open;
}

void OverlayUI::tab_general_rom() {
    ImGui::Text("Config File: %s", paths_.config_file.string().c_str());

    char rom_buf[512]{};
    copy_string_to_buf(config_.rom_path, rom_buf, sizeof(rom_buf));
    if (ImGui::InputText("ROM Path (.rvz/.iso/.gcm)", rom_buf, sizeof(rom_buf))) {
        config_.rom_path = rom_buf;
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##rom")) {
        start_rom_file_picker();
    }

    char mod_buf[512]{};
    copy_string_to_buf(config_.mod_override_dir, mod_buf, sizeof(mod_buf));
    if (ImGui::InputText("Mod Override Dir", mod_buf, sizeof(mod_buf))) {
        config_.mod_override_dir = mod_buf;
        config_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##mod")) {
        start_mod_dir_picker();
    }

    if (ImGui::Button("Mount / Remount Disc")) {
        try_mount_configured_rom();
    }
    ImGui::SameLine();
    if (ImGui::Button("Verify Logical ISO SHA-1") && !config_.rom_path.empty()) {
        start_sha1_verification(disc_.is_mounted());
    }

    if (sha1_running_.load()) {
        ImGui::ProgressBar(sha1_progress_.load(), ImVec2(-1.0f, 0.0f), "Computing Logical SHA-1...");
    } else {
        std::lock_guard lock(sha1_mutex_);
        if (!sha1_result_hex_.empty()) {
            ImGui::Text("Logical ISO SHA-1: %s", sha1_result_hex_.c_str());
        } else if (!sha1_error_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", sha1_error_.c_str());
        }
    }

    if (!status_banner_.empty()) {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.50f, 1.0f), "%s", status_banner_.c_str());
    }
    if (!error_banner_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", error_banner_.c_str());
    }

    if (disc_.is_mounted()) {
        const auto& h = disc_.header();
        ImGui::SeparatorText("Mounted Disc Info");
        ImGui::Text("Game ID: %s | Format: %s (%s) | Title: %s",
                    h.game_id.c_str(), h.container_format.c_str(),
                    h.compression_name.c_str(), h.internal_title.c_str());
        ImGui::Text("Logical ISO: %.2f MiB | Chunk: %u KiB | DOL: 0x%X | FST Entries: %zu",
                    static_cast<double>(h.logical_iso_size) / (1024.0 * 1024.0),
                    h.chunk_size / 1024u, h.dol_offset, disc_.file_count());
    }

    ImGui::SeparatorText("Virtual Memory Cards (Dolphin GCI Folders)");
    ImGui::Text("Slot A (%zu files): %s", card_.files(0).size(), card_.slot_dir(0).string().c_str());
    ImGui::Text("Slot B (%zu files): %s", card_.files(1).size(), card_.slot_dir(1).string().c_str());
    if (ImGui::Button("Rescan Memory Cards")) {
        card_.refresh_slot(0);
        card_.refresh_slot(1);
    }

    ImGui::SeparatorText("Overlay Behavior");
    if (ImGui::Checkbox("Show FPS & Diagnostics Badge", &config_.show_fps_hud)) config_dirty_ = true;
    if (ImGui::Checkbox("Pause Simulation While Overlay Open", &config_.pause_when_overlay_open)) config_dirty_ = true;
}

void OverlayUI::open_gma_bytes(std::span<const uint8_t> gma_bytes,
                               const std::optional<std::vector<uint8_t>>& tpl_bytes,
                               const std::string& label) {
    GmaArchive gma;
    std::string err;
    if (!gma.load(gma_bytes, true, &err)) {
        asset_action_status_ = "GMA load failed (" + label + "): " + err;
        return;
    }

    TplArchive tpl;
    const TplArchive* tpl_ptr = nullptr;
    if (tpl_bytes.has_value() && !tpl_bytes->empty()) {
        std::string tpl_err;
        if (tpl.load(*tpl_bytes, true, &tpl_err)) {
            tpl_ptr = &tpl;
        }
    }

    sofdec_.close();
    audio_.clear_fmv_pcm();
    renderer_.clear_fmv_frame();

    if (renderer_.gx_backend().upload_gma_scene(gma, tpl_ptr, label, &err)) {
        const auto& gx = renderer_.gx_backend();
        asset_action_status_ = "Loaded 3D GMA: " + label + " (" +
                               std::to_string(gma.valid_model_count()) + " models, " +
                               std::to_string(gx.total_triangles()) + " tris" +
                               (tpl_ptr ? ", " + std::to_string(tpl_ptr->valid_texture_count()) + " textures)" : ", untextured)");
        overlay_open_ = false;
    } else {
        asset_action_status_ = "GX upload failed (" + label + "): " + err;
    }
}

void OverlayUI::open_tpl_bytes(std::span<const uint8_t> tpl_bytes, const std::string& label) {
    TplArchive tpl;
    std::string err;
    if (!tpl.load(tpl_bytes, true, &err)) {
        asset_action_status_ = "TPL load failed (" + label + "): " + err;
        return;
    }
    inspected_tpl_name_ = label;
    inspected_tpl_ = std::move(tpl);
    asset_action_status_ = "Loaded TPL: " + label + " (" +
                           std::to_string(inspected_tpl_->valid_texture_count()) + "/" +
                           std::to_string(inspected_tpl_->textures().size()) + " valid textures)";
}

void OverlayUI::open_arc_bytes(std::span<const uint8_t> arc_bytes, const std::string& label) {
    U8Archive arc;
    std::string err;
    if (!arc.load(arc_bytes, true, &err)) {
        asset_action_status_ = "U8 ARC load failed (" + label + "): " + err;
        return;
    }
    open_arc_name_ = label;
    open_arc_ = std::move(arc);
    asset_action_status_ = "Opened ARC: " + label + " (" +
                           std::to_string(open_arc_->file_count()) + " inner files)";
}

void OverlayUI::tab_disc_assets() {
    if (!disc_.is_mounted()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "No disc mounted. Select your F-Zero GX (.rvz / .iso / .gcm) in General & ROM.");
        return;
    }

    const auto& h = disc_.header();
    ImGui::Text("Disc: %s (%s / %s) | %zu FST entries",
                h.game_id.c_str(), h.container_format.c_str(),
                h.compression_name.c_str(), disc_.file_count());

    if (dol_info_.has_value()) {
        ImGui::Text("main.dol: Entry 0x%08X | Code %.1f KiB | Data %.1f KiB | BSS %.1f KiB | SHA-1 %.12s...",
                    dol_info_->entry_point,
                    static_cast<double>(dol_info_->total_code_bytes) / 1024.0,
                    static_cast<double>(dol_info_->total_data_bytes) / 1024.0,
                    static_cast<double>(dol_info_->bss_size) / 1024.0,
                    dol_info_->sha1_hex.c_str());
    }

    if (!asset_action_status_.empty()) {
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.55f, 1.0f), "%s", asset_action_status_.c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##asset_filter", "Filter path (e.g. bgm, car, mute, rel)...",
                             asset_filter_, sizeof(asset_filter_));
    ImGui::SameLine();
    ImGui::RadioButton("All", &asset_category_, 0); ImGui::SameLine();
    ImGui::RadioButton("3D (.gma)", &asset_category_, 1); ImGui::SameLine();
    ImGui::RadioButton("Tex (.tpl)", &asset_category_, 2); ImGui::SameLine();
    ImGui::RadioButton("Arc (.arc)", &asset_category_, 3); ImGui::SameLine();
    ImGui::RadioButton("ADX", &asset_category_, 4); ImGui::SameLine();
    ImGui::RadioButton("FMV", &asset_category_, 5); ImGui::SameLine();
    ImGui::RadioButton("REL", &asset_category_, 6);

    const bool has_subpanel = inspected_rel_.has_value() || open_arc_.has_value() || inspected_tpl_.has_value();
    const float table_h = has_subpanel ? 195.0f : 330.0f;
    if (ImGui::BeginTable("FstTable", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Disc Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        int row_id = 0;
        for (const auto& entry : disc_.files()) {
            if (entry.is_dir) continue;
            if (!icontains(entry.path, asset_filter_)) continue;
            if (asset_category_ == 1 && !has_asset_ext(entry.path, ".gma", ".gma.lz")) continue;
            if (asset_category_ == 2 && !has_asset_ext(entry.path, ".tpl", ".tpl.lz")) continue;
            if (asset_category_ == 3 && !has_asset_ext(entry.path, ".arc", ".arc.lz")) continue;
            if (asset_category_ == 4 && !ends_with_ci(entry.path, ".adx")) continue;
            if (asset_category_ == 5 && !ends_with_ci(entry.path, ".sfd")) continue;
            if (asset_category_ == 6 && !ends_with_ci(entry.path, ".rel")) continue;

            ImGui::TableNextRow();
            ImGui::PushID(row_id++);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry.path.c_str());

            ImGui::TableSetColumnIndex(1);
            if (entry.size >= 1024 * 1024) {
                ImGui::Text("%.2f MiB", static_cast<double>(entry.size) / (1024.0 * 1024.0));
            } else {
                ImGui::Text("%.1f KiB", static_cast<double>(entry.size) / 1024.0);
            }

            ImGui::TableSetColumnIndex(2);
            if (has_asset_ext(entry.path, ".gma", ".gma.lz")) {
                if (ImGui::SmallButton("View 3D")) {
                    std::vector<uint8_t> gma_bytes;
                    if (disc_.read_file(entry.path, gma_bytes)) {
                        const std::string stem = make_companion_tpl_stem(entry.path);
                        std::optional<std::vector<uint8_t>> tpl_bytes;
                        std::vector<uint8_t> tmp;
                        if (disc_.read_file(stem + ".tpl.lz", tmp) || disc_.read_file(stem + ".tpl", tmp)) {
                            tpl_bytes = std::move(tmp);
                        }
                        open_gma_bytes(gma_bytes, tpl_bytes, entry.path);
                    } else {
                        asset_action_status_ = "Failed reading " + entry.path;
                    }
                }
                ImGui::SameLine();
            } else if (has_asset_ext(entry.path, ".tpl", ".tpl.lz")) {
                if (ImGui::SmallButton("Inspect TPL")) {
                    std::vector<uint8_t> tpl_bytes;
                    if (disc_.read_file(entry.path, tpl_bytes)) {
                        open_tpl_bytes(tpl_bytes, entry.path);
                    } else {
                        asset_action_status_ = "Failed reading " + entry.path;
                    }
                }
                ImGui::SameLine();
            } else if (has_asset_ext(entry.path, ".arc", ".arc.lz")) {
                if (ImGui::SmallButton("Open ARC")) {
                    std::vector<uint8_t> arc_bytes;
                    if (disc_.read_file(entry.path, arc_bytes)) {
                        open_arc_bytes(arc_bytes, entry.path);
                    } else {
                        asset_action_status_ = "Failed reading " + entry.path;
                    }
                }
                ImGui::SameLine();
            } else if (ends_with_ci(entry.path, ".adx")) {
                if (ImGui::SmallButton("Play")) {
                    std::vector<uint8_t> bytes;
                    std::string err;
                    if (disc_.read_file(entry.path, bytes) &&
                        audio_.play_adx(std::move(bytes), entry.path, true, &err)) {
                        asset_action_status_ = "Playing ADX: " + entry.path;
                    } else {
                        asset_action_status_ = "ADX play failed: " + err;
                    }
                }
                ImGui::SameLine();
            } else if (ends_with_ci(entry.path, ".sfd")) {
                if (ImGui::SmallButton("Play FMV")) {
                    std::vector<uint8_t> bytes;
                    std::string err;
                    audio_.stop_adx();
                    audio_.clear_fmv_pcm();
                    if (disc_.read_file(entry.path, bytes) &&
                        sofdec_.open_from_memory(std::move(bytes), entry.path, &err)) {
                        asset_action_status_ = "Playing Sofdec FMV: " + entry.path + " (Press F1 to toggle menu)";
                        overlay_open_ = false;
                    } else {
                        asset_action_status_ = "FMV open failed: " + err;
                    }
                }
                ImGui::SameLine();
            } else if (ends_with_ci(entry.path, ".rel")) {
                if (ImGui::SmallButton("Inspect")) {
                    std::vector<uint8_t> bytes;
                    RelInfo rel{};
                    if (disc_.read_file(entry.path, bytes) && parse_rel_header(bytes, rel)) {
                        inspected_rel_ = std::move(rel);
                        inspected_rel_name_ = entry.path;
                    } else {
                        asset_action_status_ = "Failed parsing REL header: " + entry.path;
                    }
                }
                ImGui::SameLine();
            }

            if (ImGui::SmallButton("Extract")) {
                std::vector<uint8_t> bytes;
                if (disc_.read_file(entry.path, bytes)) {
                    const std::filesystem::path base =
                        config_.mod_override_dir.empty()
                            ? (paths_.data_dir / "extracted")
                            : std::filesystem::path(config_.mod_override_dir);
                    const std::filesystem::path out_file = base / entry.path;
                    std::error_code ec;
                    std::filesystem::create_directories(out_file.parent_path(), ec);
                    std::ofstream out(out_file, std::ios::binary);
                    if (out && out.write(reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<std::streamsize>(bytes.size()))) {
                        asset_action_status_ = "Extracted to " + out_file.string();
                    } else {
                        asset_action_status_ = "Failed writing " + out_file.string();
                    }
                } else {
                    asset_action_status_ = "Failed reading " + entry.path + " from disc.";
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (open_arc_.has_value()) {
        ImGui::SeparatorText(("U8 Archive Explorer: " + open_arc_name_).c_str());
        if (ImGui::SmallButton("Close Archive")) {
            open_arc_.reset();
        } else if (ImGui::BeginTable("ArcTable", 3,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                     ImVec2(0.0f, 130.0f))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Archive Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 175.0f);
            ImGui::TableHeadersRow();

            int aid = 0;
            for (const auto& ae : open_arc_->entries()) {
                if (ae.is_dir) continue;
                ImGui::TableNextRow();
                ImGui::PushID(100000 + aid++);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(ae.path.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.1f KiB", static_cast<double>(ae.size) / 1024.0);
                ImGui::TableSetColumnIndex(2);

                if (has_asset_ext(ae.path, ".gma", ".gma.lz")) {
                    if (ImGui::SmallButton("View 3D")) {
                        const auto gspan = open_arc_->entry_bytes(ae);
                        const std::string stem = make_companion_tpl_stem(ae.path);
                        std::optional<std::vector<uint8_t>> tpl_bytes;
                        std::vector<uint8_t> tmp;
                        if (open_arc_->read_file(stem + ".tpl", tmp) ||
                            open_arc_->read_file(stem + ".tpl.lz", tmp)) {
                            tpl_bytes = std::move(tmp);
                        } else {
                            // Prefer a .tpl in the same directory (e.g. bfalcon_02.gma -> bfalcon_sv0.tpl),
                            // then fallback to the first .tpl in the archive.
                            const auto slash = ae.path.rfind('/');
                            const std::string dir = (slash != std::string::npos) ? ae.path.substr(0, slash + 1) : std::string{};
                            const U8Entry* fallback_tpl = nullptr;
                            for (const auto& cand : open_arc_->entries()) {
                                if (!cand.is_dir && has_asset_ext(cand.path, ".tpl", ".tpl.lz")) {
                                    if (!dir.empty() && cand.path.starts_with(dir)) {
                                        fallback_tpl = &cand;
                                        break;
                                    }
                                    if (!fallback_tpl) {
                                        fallback_tpl = &cand;
                                    }
                                }
                            }
                            if (fallback_tpl) {
                                auto sp = open_arc_->entry_bytes(*fallback_tpl);
                                tpl_bytes.emplace(sp.begin(), sp.end());
                            }
                        }
                        open_gma_bytes(gspan, tpl_bytes, open_arc_name_ + ":" + ae.path);
                    }
                    ImGui::SameLine();
                } else if (has_asset_ext(ae.path, ".tpl", ".tpl.lz")) {
                    if (ImGui::SmallButton("Inspect TPL")) {
                        open_tpl_bytes(open_arc_->entry_bytes(ae), open_arc_name_ + ":" + ae.path);
                    }
                    ImGui::SameLine();
                }

                if (ImGui::SmallButton("Extract")) {
                    const auto sp = open_arc_->entry_bytes(ae);
                    const std::filesystem::path base =
                        config_.mod_override_dir.empty()
                            ? (paths_.data_dir / "extracted" / "arc")
                            : (std::filesystem::path(config_.mod_override_dir) / "arc");
                    const std::filesystem::path out_file = base / ae.path;
                    std::error_code ec;
                    std::filesystem::create_directories(out_file.parent_path(), ec);
                    std::ofstream out(out_file, std::ios::binary);
                    if (out && out.write(reinterpret_cast<const char*>(sp.data()),
                                         static_cast<std::streamsize>(sp.size()))) {
                        asset_action_status_ = "Extracted ARC entry to " + out_file.string();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (inspected_tpl_.has_value()) {
        ImGui::SeparatorText(("TPL Inspector: " + inspected_tpl_name_).c_str());
        if (ImGui::SmallButton("Close TPL Inspector")) {
            inspected_tpl_.reset();
        } else {
            const auto& texs = inspected_tpl_->textures();
            ImGui::SameLine();
            ImGui::Text("%zu entries (%zu valid)", texs.size(), inspected_tpl_->valid_texture_count());
            if (ImGui::BeginTable("TplTable", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 115.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < texs.size(); ++i) {
                    const auto& t = texs[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(200000 + static_cast<int>(i));
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%zu", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%ux%u", t.width, t.height);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s%s", gx_tex_fmt_name(t.format), t.valid ? "" : " (empty)");
                    ImGui::TableSetColumnIndex(3);
                    if (t.valid && ImGui::SmallButton("View")) {
                        std::string err;
                        if (renderer_.gx_backend().upload_tpl_preview(*inspected_tpl_, static_cast<int>(i),
                                                                      inspected_tpl_name_, &err)) {
                            overlay_open_ = false;
                        } else {
                            asset_action_status_ = err;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    if (inspected_rel_.has_value()) {
        const auto& r = *inspected_rel_;
        ImGui::SeparatorText(("REL Inspector: " + inspected_rel_name_).c_str());
        ImGui::Text("ID: %u | Version: %u | Sections: %u | BSS: 0x%X | SHA-1: %.16s...",
                    r.id, r.version, r.num_sections, r.bss_size, r.sha1_hex.c_str());
        if (ImGui::SmallButton("Close REL Inspector")) {
            inspected_rel_.reset();
        }
    }
}

void OverlayUI::tab_gx_viewer() {
    auto& gx = renderer_.gx_backend();
    if (ImGui::Button("Launch Live GX / EFB Test Scene")) {
        sofdec_.close();
        audio_.clear_fmv_pcm();
        renderer_.clear_fmv_frame();
        gx.enable_live_demo_scene();
        overlay_open_ = false;
        return;
    }

    if (!gx.has_scene()) {
        ImGui::TextWrapped(
            "No 3D GX model or texture is currently loaded in the viewport.\n"
            "Click 'Launch Live GX / EFB Test Scene' above to test live GX draws, multi-stage TEV, "
            "Hor+ projection, and mid-frame EFB copy-to-texture, or open 'Disc & Assets' to load a .gma/.tpl/.arc.");
        return;
    }

    auto& vs = gx.viewer_state();
    ImGui::SameLine();
    if (ImGui::Button("Return to Calibration Scene")) {
        gx.clear_scene();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset / Auto-Frame Camera")) {
        gx.auto_frame_camera();
    }

    ImGui::Text("Active Scene: %s", gx.scene_label().c_str());
    ImGui::Text("Models: %zu | Submeshes: %zu | Triangles: %zu | Textures: %zu | Max TEV Stages: %u | EFB Copies: %u",
                gx.model_summaries().size(), gx.total_submeshes(),
                gx.total_triangles(), gx.texture_summaries().size(),
                gx.last_max_tev_stages(), gx.last_efb_copies());

    ImGui::SeparatorText("Model & Camera");
    const auto& models = gx.model_summaries();
    std::string preview_lbl = "All Models";
    if (vs.selected_model_index >= 0 && static_cast<size_t>(vs.selected_model_index) < models.size()) {
        preview_lbl = models[static_cast<size_t>(vs.selected_model_index)].name;
    }
    if (ImGui::BeginCombo("Selected Model", preview_lbl.c_str())) {
        if (ImGui::Selectable("All Models", vs.selected_model_index == -1)) {
            vs.selected_model_index = -1;
            gx.auto_frame_camera();
        }
        for (size_t i = 0; i < models.size(); ++i) {
            if (!models[i].valid) continue;
            const std::string item = std::to_string(i) + ": " + models[i].name +
                                     " (" + std::to_string(models[i].triangle_count) + " tris)";
            if (ImGui::Selectable(item.c_str(), vs.selected_model_index == static_cast<int>(i))) {
                vs.selected_model_index = static_cast<int>(i);
                gx.auto_frame_camera();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Checkbox("Auto-Rotate", &vs.auto_rotate);
    ImGui::SliderFloat("Field of View (deg)", &vs.fov_y_deg, 20.0f, 100.0f);
    ImGui::SliderFloat("Yaw (deg)", &vs.yaw_deg, -360.0f, 360.0f);
    ImGui::SliderFloat("Pitch (deg)", &vs.pitch_deg, -89.0f, 89.0f);
    ImGui::DragFloat("Orbit Distance", &vs.distance, 0.25f, 0.25f, 50000.0f);
    ImGui::ColorEdit3("Viewport Background", vs.background_color.data());

    ImGui::SeparatorText("GX TEV & Rasterizer State");
    int shade_idx = static_cast<int>(vs.shading_mode);
    const char* shade_items[] = {"GX TEV Shaded", "Unlit Textured", "View Normals", "Vertex / Material Flat"};
    if (ImGui::Combo("Shading Mode", &shade_idx, shade_items, 4)) {
        vs.shading_mode = static_cast<GxShadingMode>(shade_idx);
    }
    ImGui::Checkbox("Enable Textures", &vs.textures_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Enable Lighting", &vs.lighting_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Enable Fog", &vs.fog_enabled);
    ImGui::Checkbox("Transparent Pass", &vs.transparent_pass_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Backface Culling", &vs.backface_culling_enabled);
    if (!gx.supports_wireframe()) {
        ImGui::BeginDisabled();
        bool wf = false;
        ImGui::Checkbox("Wireframe (GPU unsupported)", &wf);
        ImGui::EndDisabled();
    } else {
        ImGui::Checkbox("Wireframe", &vs.wireframe);
    }

    ImGui::TextDisabled("Controls when overlay closed: Left-Drag Orbit | Right-Drag Pan | Wheel Zoom | WASD/QE + Gamepad");
}

void OverlayUI::tab_video_aspect() {
    const auto& d = renderer_.diagnostics();
    ImGui::Text("GPU: %s | Present: %s | Offscreen: %ux%u | Swapchain: %ux%u",
                d.gpu_name.c_str(), d.active_present_mode.c_str(),
                d.internal_width, d.internal_height, d.swapchain_width, d.swapchain_height);
    ImGui::Separator();

    if (ImGui::SliderInt("Internal Resolution Scale", &config_.internal_resolution_scale, 1, 8, "%dx (x 480p)")) {
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
    }

    int ar_idx = static_cast<int>(config_.aspect_ratio);
    const char* ar_items[] = {"Auto (Match Window)", "4:3 (Original)", "16:9 (Widescreen)", "16:10", "21:9 (Ultrawide)"};
    if (ImGui::Combo("Aspect Ratio (Hor+)", &ar_idx, ar_items, 5)) {
        config_.aspect_ratio = static_cast<AspectRatioMode>(ar_idx);
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
    }

    int hud_idx = static_cast<int>(config_.hud_mode);
    const char* hud_items[] = {"Safe Centered (4:3/16:9)", "Edge Anchored"};
    if (ImGui::Combo("Race HUD Anchoring", &hud_idx, hud_items, 2)) {
        config_.hud_mode = static_cast<HudLayoutMode>(hud_idx);
        config_dirty_ = true;
    }

    const int fps_options[] = {60, 120, 144, 240, 0};
    const char* fps_labels[] = {"60 FPS (Native Physics)", "120 FPS (Interpolated)", "144 FPS (Interpolated)",
                                "240 FPS (Interpolated)", "Uncapped"};
    int fps_idx = 0;
    for (int i = 0; i < 5; ++i) {
        if (config_.target_fps == fps_options[i]) fps_idx = i;
    }
    if (ImGui::Combo("Target Framerate", &fps_idx, fps_labels, 5)) {
        config_.target_fps = fps_options[fps_idx];
        config_dirty_ = true;
    }

    if (ImGui::Checkbox("Frame Interpolation (>60 FPS)", &config_.frame_interpolation)) {
        config_dirty_ = true;
    }

    int vsync_idx = static_cast<int>(config_.vsync);
    const char* vsync_items[] = {"Off (Immediate)", "VSync (FIFO)", "Fast Sync (Mailbox)"};
    if (ImGui::Combo("VSync Mode", &vsync_idx, vsync_items, 3)) {
        config_.vsync = static_cast<VsyncMode>(vsync_idx);
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
    }

    if (ImGui::Checkbox("Fullscreen", &config_.fullscreen)) {
        config_dirty_ = true;
        SDL_SetWindowFullscreen(window_, config_.fullscreen);
    }

    if (!d.frame_time_history.empty()) {
        ImGui::PlotLines("Frame Time (ms)", d.frame_time_history.data(),
                         static_cast<int>(d.frame_time_history.size()), 0, nullptr, 0.0f, 33.3f, ImVec2(0, 70));
    }
}

void OverlayUI::tab_shaders() {
    int mode = static_cast<int>(config_.shader_backend);
    const char* modes[] = {"None (Sharp)", "Built-in CRT Shader", "librashader (.slangp)"};
    if (ImGui::Combo("Post-Processing Mode", &mode, modes, 3)) {
        config_.shader_backend = static_cast<ShaderBackendMode>(mode);
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
        renderer_.reload_librashader(config_);
    }

    if (ImGui::Checkbox("Feed 480p Resolution to CRT Shader", &config_.librashader_feed_480p)) {
        config_dirty_ = true;
        renderer_.apply_video_config(config_);
    }

    if (ImGui::Checkbox("AMD FSR 1.0 Upscaling (EASU + RCAS)", &config_.fsr_enabled)) {
        config_dirty_ = true;
    }
    if (config_.fsr_enabled) {
        if (ImGui::SliderFloat("FSR RCAS Sharpness", &config_.fsr_sharpness, 0.0f, 1.0f)) {
            config_dirty_ = true;
        }
    }

    if (config_.shader_backend == ShaderBackendMode::BuiltinCrt) {
        ImGui::SeparatorText("Built-in CRT Parameters");
        auto& c = config_.builtin_crt;
        if (ImGui::SliderFloat("Scanline Strength", &c.scanline_strength, 0.0f, 1.0f)) config_dirty_ = true;
        if (ImGui::SliderFloat("Phosphor Mask Strength", &c.mask_strength, 0.0f, 1.0f)) config_dirty_ = true;
        if (ImGui::SliderFloat("Screen Curvature", &c.curvature, 0.0f, 0.30f)) config_dirty_ = true;
        if (ImGui::SliderFloat("Vignette", &c.vignette, 0.0f, 1.0f)) config_dirty_ = true;
        if (ImGui::SliderFloat("Brightness Boost", &c.brightness, 0.8f, 1.6f)) config_dirty_ = true;
    }

    if (config_.shader_backend == ShaderBackendMode::Librashader) {
        ImGui::SeparatorText("librashader Runtime");
        const auto& lib = renderer_.librashader();
        ImGui::TextWrapped("Status: %s", lib.status_message().c_str());

        char dll_buf[512]{};
        copy_string_to_buf(config_.librashader_dll_path, dll_buf, sizeof(dll_buf));
        if (ImGui::InputText("librashader Library (.so/.dll)", dll_buf, sizeof(dll_buf))) {
            config_.librashader_dll_path = dll_buf;
            config_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##lib")) {
            start_librashader_lib_picker();
        }

        char preset_buf[512]{};
        copy_string_to_buf(config_.librashader_preset_path, preset_buf, sizeof(preset_buf));
        if (ImGui::InputText("Shader Preset (.slangp)", preset_buf, sizeof(preset_buf))) {
            config_.librashader_preset_path = preset_buf;
            config_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##preset")) {
            start_preset_file_picker();
        }

        if (ImGui::Button("Reload librashader & Preset")) {
            renderer_.reload_librashader(config_);
        }

        auto& params = renderer_.librashader().params();
        if (!params.empty()) {
            ImGui::SeparatorText("Preset Runtime Parameters");
            for (auto& p : params) {
                if (ImGui::SliderFloat(p.description.c_str(), &p.current, p.minimum, p.maximum)) {
                    renderer_.librashader().set_param(p.name, p.current);
                    config_.librashader_params[p.name] = p.current;
                    config_dirty_ = true;
                }
            }
        }
    }
}

void OverlayUI::tab_controllers() {
    const char* port_names[] = {"Port 1", "Port 2", "Port 3", "Port 4"};
    ImGui::Combo("GameCube Port", &selected_port_, port_names, 4);

    auto& pc = config_.controllers[static_cast<size_t>(selected_port_)];
    const auto& state = input_.port_state(selected_port_);

    if (ImGui::Checkbox("Port Enabled", &pc.enabled)) config_dirty_ = true;
    ImGui::SameLine();
    ImGui::Text("Active Device: %s", state.device_label.c_str());

    const auto devices = input_.connected_device_names();
    if (ImGui::BeginCombo("Input Device", pc.device_name.c_str())) {
        if (ImGui::Selectable("Auto", pc.device_name == "Auto")) {
            pc.device_name = "Auto";
            config_dirty_ = true;
        }
        if (ImGui::Selectable("Keyboard", pc.device_name == "Keyboard")) {
            pc.device_name = "Keyboard";
            config_dirty_ = true;
        }
        for (const auto& d : devices) {
            if (ImGui::Selectable(d.c_str(), pc.device_name == d)) {
                pc.device_name = d;
                config_dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SeparatorText("Analog Calibration (F-Zero GX Precision)");
    if (ImGui::SliderFloat("Main Stick Deadzone", &pc.stick_deadzone, 0.0f, 0.40f)) config_dirty_ = true;
    if (ImGui::SliderFloat("Main Stick Outer Gate", &pc.stick_outer_gate, 0.65f, 1.15f)) config_dirty_ = true;
    if (ImGui::SliderFloat("C-Stick Deadzone", &pc.cstick_deadzone, 0.0f, 0.40f)) config_dirty_ = true;
    if (ImGui::SliderFloat("L/R Analog Deadzone", &pc.trigger_analog_deadzone, 0.0f, 0.30f)) config_dirty_ = true;
    if (ImGui::SliderFloat("L/R Full-Click Threshold", &pc.trigger_digital_threshold, 0.30f, 1.0f)) config_dirty_ = true;
    if (ImGui::Checkbox("Rumble Enabled", &pc.rumble_enabled)) config_dirty_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Test Rumble")) {
        input_.test_rumble(selected_port_);
    }

    ImGui::SeparatorText("Live State Monitor");
    ImGui::Text("Main Stick: (%+.2f, %+.2f) -> GC (%d, %d)",
                state.norm_stick_x, state.norm_stick_y, state.stick_x, state.stick_y);
    ImGui::Text("C-Stick:    (%+.2f, %+.2f) -> GC (%d, %d)",
                state.norm_substick_x, state.norm_substick_y, state.substick_x, state.substick_y);
    ImGui::ProgressBar(state.norm_trigger_l, ImVec2(220.0f, 0.0f),
                       (state.buttons & kGcBtnL) ? "L Trigger [CLICK]" : "L Trigger (Analog)");
    ImGui::SameLine();
    ImGui::ProgressBar(state.norm_trigger_r, ImVec2(220.0f, 0.0f),
                       (state.buttons & kGcBtnR) ? "R Trigger [CLICK]" : "R Trigger (Analog)");

    ImGui::Text("Buttons Mask: 0x%04X", state.buttons);
}

void OverlayUI::tab_audio_fmv() {
    const AudioStatus st = audio_.status();
    ImGui::Text("SDL3 Audio Device: %s (%s)", st.device_name.c_str(), st.device_open ? "Active" : "Closed");
    ImGui::Text("FFmpeg Sofdec (.sfd) Support: %s", SofdecPlayer::is_ffmpeg_supported() ? "Enabled" : "Disabled");
    ImGui::Separator();

    bool vol_changed = false;
    vol_changed |= ImGui::Checkbox("Mute All Audio", &config_.mute_audio);
    vol_changed |= ImGui::SliderFloat("Master Volume", &config_.master_volume, 0.0f, 1.0f);
    vol_changed |= ImGui::SliderFloat("Music Volume (ADX / FMV)", &config_.music_volume, 0.0f, 1.0f);
    vol_changed |= ImGui::SliderFloat("SFX / Engine Volume (AX)", &config_.sfx_volume, 0.0f, 1.0f);
    if (vol_changed) {
        config_dirty_ = true;
        audio_.apply_config(config_);
    }

    ImGui::SeparatorText("CRI ADX Music Player");
    if (st.adx_playing) {
        ImGui::Text("Now Playing: %s (%u Hz, %u ch)",
                    st.adx_label.c_str(), st.adx_info.sample_rate, st.adx_info.channels);
        const float prog = st.adx_info.total_samples > 0
                               ? static_cast<float>(st.adx_current_sample) /
                                     static_cast<float>(st.adx_info.total_samples)
                               : 0.0f;
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 0.0f));
        if (ImGui::Button(st.adx_paused ? "Resume ADX" : "Pause ADX")) {
            audio_.set_adx_paused(!st.adx_paused);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop ADX")) {
            audio_.stop_adx();
        }
        ImGui::SameLine();
        bool loop = st.adx_loop;
        if (ImGui::Checkbox("Loop Track", &loop)) {
            audio_.set_adx_loop(loop);
        }
    } else {
        ImGui::TextDisabled("No ADX track playing. Pick any .adx file in the Disc & Assets tab.");
    }

    ImGui::SeparatorText("CRI Sofdec (.sfd) FMV Player");
    ImGui::Text("Status: %s", sofdec_.status_message().c_str());
    if (sofdec_.is_playing()) {
        if (sofdec_.duration_sec() > 0.0) {
            ImGui::ProgressBar(static_cast<float>(sofdec_.current_time_sec() / sofdec_.duration_sec()),
                               ImVec2(-1.0f, 0.0f));
        }
        if (ImGui::Button(sofdec_.is_paused() ? "Resume FMV" : "Pause FMV")) {
            sofdec_.set_paused(!sofdec_.is_paused());
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop FMV")) {
            sofdec_.close();
            audio_.clear_fmv_pcm();
            renderer_.clear_fmv_frame();
        }
    } else {
        ImGui::TextDisabled("No .sfd movie playing. Pick any .sfd file in the Disc & Assets tab.");
    }
}

} // namespace fzgx
