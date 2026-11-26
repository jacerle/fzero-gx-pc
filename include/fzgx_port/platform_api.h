#ifndef FZGX_PORT_PLATFORM_API_H
#define FZGX_PORT_PLATFORM_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C-callable host platform bridge for future linkage with decompiled C translation units.
 * Milestone 1 backs DVD, PAD, and CARD with DiscImage, InputManager, and GciCardManager,
 * and establishes clean GX and Audio/Video (SDL3 Audio + FFmpeg Sofdec) interface contracts.
 */

/* --- DVD / Disc FST --- */
bool fzgx_dvd_exists(const char* fst_path);
int32_t fzgx_dvd_read_file(const char* fst_path, void* out_buf, size_t max_bytes, size_t* out_read);

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

/* --- CARD / Dolphin GCI Memory Card --- */
int32_t fzgx_card_file_count(int32_t slot);
int32_t fzgx_card_read(int32_t slot, const char* file_name, void* out_buf, size_t max_bytes, size_t* out_read);

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
