#ifndef FZGX_PORT_PLATFORM_API_H
#define FZGX_PORT_PLATFORM_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C-callable host platform bridge. The native Dolphin SDK replacement (src/sdk/) and
 * decompiled C translation units reach host services (DiscImage, InputManager,
 * GciCardManager, GX state, AudioEngine) only through these functions.
 */

/* --- OS / time --- */
/* Local wall-clock time since 2000-01-01 00:00 in GameCube timer ticks (40.5 MHz), as OSGetTime. */
int64_t fzgx_os_time_ticks(void);

/* --- VI / frame pacing --- */
/* Blocks until the next host vertical retrace (host retrace hook, or 60 Hz pacing by default). */
void fzgx_vi_wait_for_retrace(void);

/* --- DVD / Disc FST --- */
bool fzgx_dvd_exists(const char* fst_path);
int32_t fzgx_dvd_read_file(const char* fst_path, void* out_buf, size_t max_bytes, size_t* out_read);
/* FST entry number of an absolute disc path ("" or "/" is the root, entry 0); -1 when absent. */
int32_t fzgx_dvd_path_to_entry(const char* fst_path);
/* Entry metadata; length honours the mod override directory. Returns false for a bad entry. */
bool fzgx_dvd_entry_info(int32_t entrynum, bool* out_is_dir, uint32_t* out_disc_offset, uint32_t* out_length);
/* Reads logical disc bytes; reads inside a file with a mod override come from the override. */
bool fzgx_dvd_read_abs(uint32_t disc_offset, void* dst, uint32_t length);

/* --- PAD / Controller (4 GameCube ports) --- */
typedef struct FzgxPadSnapshot {
    bool connected;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    int8_t substick_x;
    int8_t substick_y;
    uint8_t trigger_l;
    uint8_t trigger_r;
} FzgxPadSnapshot;

void fzgx_pad_read(int32_t port, FzgxPadSnapshot* out_pad);
void fzgx_pad_set_rumble(int32_t port, float low_freq, float high_freq, uint32_t duration_ms);

/* --- CARD / Dolphin GCI Memory Card (slots 0/1) --- */
/*
 * Files are addressed by their 64-byte big-endian GameCube directory entry (CARDDir, the .gci
 * header): game code (0x00), maker code (0x04) and NUL-padded file name (0x08) identify a file.
 * Payload offsets are relative to the first data block (the byte after the .gci header).
 */
#define FZGX_CARD_DENTRY_SIZE 64
/* A card is inserted in a slot when the host has a GCI folder mounted for it. */
bool fzgx_card_slot_mounted(int32_t slot);
/* Stable serial number of the slot's GCI folder; 0 when the slot is not mounted. */
uint64_t fzgx_card_serial(int32_t slot);
int32_t fzgx_card_file_count(int32_t slot);
/* Directory entry of host file `index` (0 .. fzgx_card_file_count - 1), as stored on disk. */
bool fzgx_card_file_dentry(int32_t slot, int32_t index, uint8_t out_dentry[FZGX_CARD_DENTRY_SIZE]);
/* Creates a file with a zero-filled payload of the entry's block count (0x38); fails if it exists. */
bool fzgx_card_create_file(int32_t slot, const uint8_t dentry[FZGX_CARD_DENTRY_SIZE]);
/* Replaces the directory entry of the file `key`; a different name renames the file. */
bool fzgx_card_set_dentry(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE],
                          const uint8_t dentry[FZGX_CARD_DENTRY_SIZE]);
bool fzgx_card_read_data(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE], uint32_t offset, void* dst,
                         uint32_t length);
bool fzgx_card_write_data(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE], uint32_t offset,
                          const void* src, uint32_t length);
bool fzgx_card_delete_file(int32_t slot, const uint8_t key[FZGX_CARD_DENTRY_SIZE]);

/* --- GX / Graphics Command Boundary --- */
void fzgx_gx_begin_frame(uint32_t internal_width, uint32_t internal_height, float aspect_ratio);
void fzgx_gx_end_frame(void);
void fzgx_gx_fifo_u8(uint8_t v);
void fzgx_gx_fifo_u16(uint16_t v);
void fzgx_gx_fifo_u32(uint32_t v);
void fzgx_gx_fifo_f32(float v);
void fzgx_gx_set_projection(const float row_major_4x4[16], bool is_ortho);
void fzgx_gx_load_pos_mtx(uint32_t slot, const float row_major_3x4[12]);
void fzgx_gx_set_current_mtx(uint32_t slot);
void fzgx_gx_set_vtx_desc(uint32_t attr_mask, bool use_vtxfmt1);
void fzgx_gx_set_cull_mode(uint8_t cull_mode);
void fzgx_gx_set_blend_mode(uint8_t blend_mode);
void fzgx_gx_set_z_mode(bool depth_test, uint8_t depth_func, bool depth_write);
void fzgx_gx_set_tev_num_stages(uint32_t num_stages);
void fzgx_gx_set_alpha_compare(uint8_t comp0, uint8_t ref0, uint8_t op, uint8_t comp1, uint8_t ref1);
void fzgx_gx_set_fog(uint8_t fog_type, float start_z, float end_z, float near_z, float far_z,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void fzgx_gx_request_efb_copy(uint32_t target_efb_id, uint32_t src_x, uint32_t src_y,
                              uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                              bool clear_efb);

/* --- Audio / FMV Middleware Boundary (SDL3 Audio + FFmpeg Sofdec .sfd) --- */
bool fzgx_av_init(void);
void fzgx_av_shutdown(void);
void fzgx_av_set_volumes(float master, float music, float sfx, bool muted);
bool fzgx_av_adx_play(const char* fst_path, bool loop);
void fzgx_av_adx_stop(void);
bool fzgx_av_sofdec_open(const char* fst_path);
void fzgx_av_sofdec_close(void);

#ifdef __cplusplus
}
#endif

#endif /* FZGX_PORT_PLATFORM_API_H */
