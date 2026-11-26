#include "platform/platform_services.hpp"

#include "fzgx_port/platform_api.h"
#include "fzgx_port/mwcc_shim.h"

#include "audio/audio_engine.hpp"
#include "disc/disc_image.hpp"
#include "input/input_manager.hpp"
#include "save/gci_card.hpp"
#include "video/gx_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace fzgx {

struct PlatformServices {
    DiscImage* disc{nullptr};
    InputManager* input{nullptr};
    GciCardManager* card{nullptr};
    AudioEngine* audio{nullptr};
};

static PlatformServices g_services{};
static std::function<void()> g_vi_retrace_hook;

void bind_platform_services(DiscImage* disc, InputManager* input, GciCardManager* card, AudioEngine* audio) {
    g_services.disc = disc;
    g_services.input = input;
    g_services.card = card;
    g_services.audio = audio;
}

void set_vi_retrace_hook(std::function<void()> hook) {
    g_vi_retrace_hook = std::move(hook);
}

static GciCardManager* mounted_card(int32_t slot) {
    if (!g_services.card || slot < 0 || slot > 1 || g_services.card->slot_dir(slot).empty()) {
        return nullptr;
    }
    return g_services.card;
}

} // namespace fzgx

extern "C" {

int64_t fzgx_os_time_ticks(void) {
    using namespace std::chrono;
    // GameCube timer: bus clock (162 MHz) / 4 = 40.5 MHz = 81 ticks per 2 us. The RTC counts local time.
    const auto local_now = current_zone()->to_local(system_clock::now());
    const auto since_2000 = local_now - local_days{year{2000} / January / 1};
    return duration_cast<microseconds>(since_2000).count() * 81 / 2;
}

void fzgx_vi_wait_for_retrace(void) {
    if (fzgx::g_vi_retrace_hook) {
        fzgx::g_vi_retrace_hook(); // host frame; pacing below keeps the game at the NTSC rate
    }
    using clock = std::chrono::steady_clock;
    constexpr auto kNtscField = std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(16'683'350));
    static clock::time_point next_retrace{};
    const auto now = clock::now();
    if (next_retrace <= now - kNtscField) {
        next_retrace = now + kNtscField; // first call, or fell more than a field behind: resync
    }
    std::this_thread::sleep_until(next_retrace);
    next_retrace += kNtscField;
}

bool fzgx_dvd_exists(const char* fst_path) {
    if (!fzgx::g_services.disc || !fst_path) {
        return false;
    }
    return fzgx::g_services.disc->exists(fst_path);
}

int32_t fzgx_dvd_read_file(const char* fst_path, void* out_buf, size_t max_bytes, size_t* out_read) {
    if (out_read) {
        *out_read = 0;
    }
    if (!fzgx::g_services.disc || !fst_path || !out_buf) {
        return -1;
    }
    std::vector<uint8_t> bytes;
    if (!fzgx::g_services.disc->read_file(fst_path, bytes)) {
        return -1;
    }
    const size_t copy_len = std::min(bytes.size(), max_bytes);
    if (copy_len > 0) {
        std::memcpy(out_buf, bytes.data(), copy_len);
    }
    if (out_read) {
        *out_read = copy_len;
    }
    return 0;
}

int32_t fzgx_dvd_path_to_entry(const char* fst_path) {
    if (!fzgx::g_services.disc || !fst_path) {
        return -1;
    }
    return fzgx::g_services.disc->entry_number(fst_path);
}

bool fzgx_dvd_entry_info(int32_t entrynum, bool* out_is_dir, uint32_t* out_disc_offset, uint32_t* out_length) {
    if (!fzgx::g_services.disc || !fzgx::g_services.disc->is_mounted()) {
        return false;
    }
    bool is_dir = true;
    uint32_t disc_offset = 0;
    uint64_t length = 0;
    if (entrynum != 0) {
        const fzgx::FstEntry* entry = fzgx::g_services.disc->entry_at(entrynum);
        if (!entry) {
            return false;
        }
        is_dir = entry->is_dir;
        if (!is_dir) {
            disc_offset = static_cast<uint32_t>(entry->offset);
            length = fzgx::g_services.disc->effective_size(*entry);
        }
    }
    if (out_is_dir) {
        *out_is_dir = is_dir;
    }
    if (out_disc_offset) {
        *out_disc_offset = disc_offset;
    }
    if (out_length) {
        *out_length = static_cast<uint32_t>(std::min<uint64_t>(length, std::numeric_limits<uint32_t>::max()));
    }
    return true;
}

bool fzgx_dvd_read_abs(uint32_t disc_offset, void* dst, uint32_t length) {
    if (!fzgx::g_services.disc || (!dst && length != 0)) {
        return false;
    }
    return fzgx::g_services.disc->read_logical(disc_offset, dst, length);
}

void fzgx_pad_read(int32_t port, FzgxPadSnapshot* out_pad) {
    if (!out_pad) {
        return;
    }
    std::memset(out_pad, 0, sizeof(*out_pad));
    if (!fzgx::g_services.input || port < 0 || port >= 4) {
        return;
    }
    const auto& p = fzgx::g_services.input->port_state(port);
    out_pad->connected = p.connected;
    out_pad->buttons = p.buttons;
    out_pad->stick_x = p.stick_x;
    out_pad->stick_y = p.stick_y;
    out_pad->substick_x = p.substick_x;
    out_pad->substick_y = p.substick_y;
    out_pad->trigger_l = p.trigger_l;
    out_pad->trigger_r = p.trigger_r;
}

void fzgx_pad_set_rumble(int32_t port, float low_freq, float high_freq, uint32_t duration_ms) {
    if (fzgx::g_services.input && port >= 0 && port < 4) {
        fzgx::g_services.input->set_rumble(port, low_freq, high_freq, duration_ms);
    }
}

bool fzgx_card_slot_mounted(int32_t slot) {
    return fzgx::mounted_card(slot) != nullptr;
}

uint64_t fzgx_card_serial(int32_t slot) {
    const fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    if (!card) {
        return 0;
    }
    // FNV-1a over the folder path: stable per folder, distinct between folders.
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : card->slot_dir(slot).lexically_normal().generic_string()) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash != 0 ? hash : 1;
}

int32_t fzgx_card_file_count(int32_t slot) {
    const fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    return card ? static_cast<int32_t>(card->files(slot).size()) : 0;
}

bool fzgx_card_file_dentry(int32_t slot, int32_t index, uint8_t out_dentry[FZGX_CARD_DENTRY_SIZE]) {
    const fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    if (!card || !out_dentry || index < 0 || static_cast<size_t>(index) >= card->files(slot).size()) {
        return false;
    }
    const auto& header = card->files(slot)[static_cast<size_t>(index)].header;
    std::memcpy(out_dentry, header.data(), header.size());
    return true;
}

bool fzgx_card_create_file(int32_t slot, const uint8_t dentry[FZGX_CARD_DENTRY_SIZE]) {
    fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    if (!card || !dentry) {
        return false;
    }
    const size_t blocks = (static_cast<size_t>(dentry[0x38]) << 8) | dentry[0x39];
    return card->create_entry(slot, dentry, std::vector<uint8_t>(blocks * fzgx::GciCardManager::kBlockSize, 0));
}

bool fzgx_card_set_dentry(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE],
                          const uint8_t dentry[FZGX_CARD_DENTRY_SIZE]) {
    fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    return card && key && dentry && card->update_entry_header(slot, key, dentry);
}

bool fzgx_card_read_data(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE], uint32_t offset, void* dst,
                         uint32_t length) {
    const fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    return card && key && card->read_entry_data(slot, key, offset, static_cast<uint8_t*>(dst), length);
}

bool fzgx_card_write_data(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE], uint32_t offset,
                          const void* src, uint32_t length) {
    fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    return card && key && card->write_entry_data(slot, key, offset, static_cast<const uint8_t*>(src), length);
}

bool fzgx_card_delete_file(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE]) {
    fzgx::GciCardManager* card = fzgx::mounted_card(slot);
    return card && key && card->delete_entry(slot, key);
}

void fzgx_gx_begin_frame(uint32_t width, uint32_t height, float aspect_ratio) {
    fzgx::global_gx_state().begin_frame(width, height, aspect_ratio, true);
}
void fzgx_gx_end_frame(void) {
    fzgx::global_gx_state().end_frame();
}
void fzgx_gx_fifo_u8(uint8_t v) {
    fzgx::global_gx_state().write_fifo_u8(v);
}
void fzgx_gx_fifo_u16(uint16_t v) {
    fzgx::global_gx_state().write_fifo_be16(v);
}
void fzgx_gx_fifo_u32(uint32_t v) {
    fzgx::global_gx_state().write_fifo_be32(v);
}
void fzgx_gx_fifo_f32(float v) {
    fzgx::global_gx_state().write_fifo_f32(v);
}
void fzgx_gx_set_projection(const float row_major_4x4[16], bool is_ortho) {
    if (row_major_4x4) {
        fzgx::global_gx_state().set_projection(row_major_4x4, is_ortho);
    }
}
void fzgx_gx_load_pos_mtx(uint32_t slot, const float row_major_3x4[12]) {
    if (row_major_3x4) {
        fzgx::global_gx_state().load_pos_mtx(slot, row_major_3x4);
    }
}
void fzgx_gx_set_current_mtx(uint32_t slot) {
    fzgx::global_gx_state().set_current_mtx(slot);
}
void fzgx_gx_set_vtx_desc(uint32_t attr_mask, bool use_vtxfmt1) {
    fzgx::global_gx_state().set_vtx_desc(attr_mask, use_vtxfmt1);
}
void fzgx_gx_set_cull_mode(uint8_t cull_mode) {
    fzgx::global_gx_state().set_cull_mode(static_cast<fzgx::GxCullMode>(cull_mode));
}
void fzgx_gx_set_blend_mode(uint8_t blend_mode) {
    fzgx::global_gx_state().set_blend_mode(static_cast<fzgx::GxBlendMode>(blend_mode));
}
void fzgx_gx_set_z_mode(bool depth_test, uint8_t depth_func, bool depth_write) {
    fzgx::global_gx_state().set_z_mode(depth_test, static_cast<fzgx::GxCompare>(depth_func), depth_write);
}
void fzgx_gx_set_tev_num_stages(uint32_t num_stages) {
    fzgx::global_gx_state().set_tev_num_stages(num_stages);
}
void fzgx_gx_set_alpha_compare(uint8_t comp0, uint8_t ref0, uint8_t op, uint8_t comp1, uint8_t ref1) {
    fzgx::global_gx_state().set_alpha_compare(
        static_cast<fzgx::GxCompare>(comp0), ref0,
        static_cast<fzgx::GxAlphaOp>(op),
        static_cast<fzgx::GxCompare>(comp1), ref1);
}
void fzgx_gx_set_fog(uint8_t fog_type, float start_z, float end_z, float near_z, float far_z,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    fzgx::global_gx_state().set_fog(
        static_cast<fzgx::GxFogType>(fog_type), start_z, end_z, near_z, far_z,
        static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
}
void fzgx_gx_request_efb_copy(uint32_t target_efb_id, uint32_t src_x, uint32_t src_y,
                              uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                              bool clear_efb) {
    fzgx::GxEfbCopyCommand cmd{};
    cmd.target_efb_id = target_efb_id;
    cmd.src_x = src_x;
    cmd.src_y = src_y;
    cmd.src_w = src_w;
    cmd.src_h = src_h;
    cmd.dst_w = dst_w;
    cmd.dst_h = dst_h;
    cmd.clear_efb = clear_efb;
    fzgx::global_gx_state().request_efb_copy(cmd);
}

bool fzgx_av_init(void) {
    return fzgx::g_services.audio != nullptr;
}

void fzgx_av_shutdown(void) {
    if (fzgx::g_services.audio) {
        fzgx::g_services.audio->stop_adx();
    }
}

void fzgx_av_set_volumes(float master, float music, float sfx, bool muted) {
    if (fzgx::g_services.audio) {
        fzgx::g_services.audio->set_volumes(master, music, sfx, muted);
    }
}

bool fzgx_av_adx_play(const char* fst_path, bool loop) {
    if (!fzgx::g_services.disc || !fzgx::g_services.audio || !fst_path) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!fzgx::g_services.disc->read_file(fst_path, bytes)) {
        return false;
    }
    return fzgx::g_services.audio->play_adx(std::move(bytes), fst_path, loop);
}

void fzgx_av_adx_stop(void) {
    if (fzgx::g_services.audio) {
        fzgx::g_services.audio->stop_adx();
    }
}

bool fzgx_av_sofdec_open(const char*) { return false; }
void fzgx_av_sofdec_close(void) {}

} // extern "C"
