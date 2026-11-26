#pragma once

#include "assets/gma_model.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace fzgx {

enum class GxCompare : uint8_t {
    Never = 0,
    Less = 1,
    Equal = 2,
    LEqual = 3,
    Greater = 4,
    NEqual = 5,
    GEqual = 6,
    Always = 7,
};

enum class GxAlphaOp : uint8_t {
    And = 0,
    Or = 1,
    Xor = 2,
    Xnor = 3,
};

enum class GxFogType : uint8_t {
    None = 0,
    PerspLin = 2,
    PerspExp = 4,
    PerspExp2 = 5,
    PerspRevExp = 6,
    PerspRevExp2 = 7,
    OrthoLin = 10,
    OrthoExp = 12,
};

enum class GxTevColorArg : uint8_t {
    CPrev = 0,
    APrev = 1,
    C0 = 2,
    A0 = 3,
    C1 = 4,
    A1 = 5,
    C2 = 6,
    A2 = 7,
    TexC = 8,
    TexA = 9,
    RasC = 10,
    RasA = 11,
    One = 12,
    Half = 13,
    Konst = 14,
    Zero = 15,
};

enum class GxTevAlphaArg : uint8_t {
    APrev = 0,
    A0 = 1,
    A1 = 2,
    A2 = 3,
    TexA = 4,
    RasA = 5,
    Konst = 6,
    Zero = 7,
};

struct GxTevStage {
    // Formula: out = clamp(d + (op ? -1 : +1) * (a*(1-c) + b*c) + bias) * scale
    std::array<uint8_t, 4> color_in{
        static_cast<uint8_t>(GxTevColorArg::Zero),
        static_cast<uint8_t>(GxTevColorArg::Zero),
        static_cast<uint8_t>(GxTevColorArg::Zero),
        static_cast<uint8_t>(GxTevColorArg::RasC),
    };
    uint8_t color_op{0};      // 0 = Add, 1 = Sub
    uint8_t color_bias{0};    // 0 = Zero, 1 = +0.5, 2 = -0.5
    uint8_t color_scale{0};   // 0 = x1, 1 = x2, 2 = x4, 3 = x0.5
    bool color_clamp{true};
    uint8_t color_out_reg{0}; // 0 = PREV, 1 = C0, 2 = C1, 3 = C2

    std::array<uint8_t, 4> alpha_in{
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::RasA),
    };
    uint8_t alpha_op{0};
    uint8_t alpha_bias{0};
    uint8_t alpha_scale{0};
    bool alpha_clamp{true};
    uint8_t alpha_out_reg{0};

    uint8_t tex_map{0xFF};   // 0..3 or 0xFF = none
    uint8_t tex_coord{0xFF}; // 0..1 or 0xFF = none
    uint8_t ras_chan{0};     // 0 = Color0A0, 1 = Color1A1, 0xFF = null
    uint8_t kcolor_sel{12};  // GX_TEV_KCSEL_K0 = 12..15, constants 0..7, RGB 16..31
    uint8_t kalpha_sel{20};  // GX_TEV_KASEL_K0_A = 20..23
    uint8_t ras_swap{0};
    uint8_t tex_swap{0};
};

struct GxTevConfig {
    uint32_t num_stages{1};
    std::array<GxTevStage, 16> stages{};
    std::array<std::array<float, 4>, 4> tev_regs{{
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
    }};
    std::array<std::array<float, 4>, 4> konst_regs{{
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
    }};
    std::array<std::array<uint8_t, 4>, 4> swap_table{{
        {0, 1, 2, 3},
        {0, 1, 2, 3},
        {0, 1, 2, 3},
        {0, 1, 2, 3},
    }};

    GxCompare alpha_comp0{GxCompare::Always};
    GxCompare alpha_comp1{GxCompare::Always};
    GxAlphaOp alpha_op{GxAlphaOp::And};
    float alpha_ref0{0.0f};
    float alpha_ref1{0.0f};

    GxFogType fog_type{GxFogType::None};
    float fog_start_z{10.0f};
    float fog_end_z{200.0f};
    float fog_near_z{0.1f};
    float fog_far_z{1000.0f};
    std::array<float, 4> fog_color{0.05f, 0.08f, 0.16f, 1.0f};

    bool enable_lighting{true};
    bool enable_textures{true};
    bool enable_fog{false};
};

// Builds a standard 1-stage TEV config (modulate Ras * Tex0 if textured, or Ras if untextured).
GxTevConfig make_default_tev_config(bool textured, bool lit);

// Builds a multi-stage TEV config from a GMA submesh and its material layers.
GxTevConfig build_gma_submesh_tev(const GmaSubMesh& sm,
                                  const std::vector<GmaMaterialLayer>& layers);

// CPU reference evaluator for the 16-stage TEV combiner + alpha compare (used by unit tests).
bool evaluate_tev_cpu(const GxTevConfig& cfg,
                      const std::array<float, 4>& ras_color,
                      const std::array<std::array<float, 4>, 4>& tex_samples,
                      std::array<float, 4>& out_rgba);

// Converts a row-major GameCube GX 4x4 projection matrix (Z in [-1, 0]) to a column-major
// Vulkan projection matrix (Z in [0, 1], Y-flipped), applying Hor+ aspect widening when !is_ortho.
std::array<float, 16> gx_build_vulkan_projection(const float gx_proj_row_major[16],
                                                 bool is_ortho,
                                                 float target_aspect,
                                                 bool enable_hor_plus,
                                                 float original_aspect = 4.0f / 3.0f);

// Standard GameCube GX perspective matrix (row-major, matching C_MTXPerspective).
std::array<float, 16> gx_make_perspective_mtx(float fov_y_deg, float aspect, float znear, float zfar);
std::array<float, 16> gx_make_ortho_mtx(float top, float bottom, float left, float right, float znear, float zfar);

struct GxDrawCommand {
    uint32_t first_index{0};
    uint32_t index_count{0};
    std::array<float, 16> mvp{};       // column-major
    std::array<float, 16> modelview{}; // column-major
    std::array<float, 16> normal_mat{};
    std::array<float, 4> mat_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> ambient_color{0.42f, 0.44f, 0.50f, 1.0f};
    std::array<float, 4> light_dir_intensity{0.45f, 0.78f, 0.42f, 1.0f};
    GxTevConfig tev{};
    GxCullMode cull{GxCullMode::Back};
    GxBlendMode blend{GxBlendMode::Opaque};
    bool depth_test{true};
    bool depth_write{true};
    GxCompare depth_func{GxCompare::LEqual};
    std::array<int32_t, 4> tex_ids{-1, -1, -1, -1}; // >= 0x10000 => EFB copy slot
    bool has_vertex_color{true};
    bool env_map_uv1{false};
};

struct GxEfbCopyCommand {
    uint32_t target_efb_id{0x10000u};
    uint32_t src_x{0};
    uint32_t src_y{0};
    uint32_t src_w{640};
    uint32_t src_h{480};
    uint32_t dst_w{320};
    uint32_t dst_h{240};
    bool clear_efb{false};
    std::array<float, 4> clear_rgba{0.03f, 0.04f, 0.08f, 1.0f};
};

struct GxCommandItem {
    enum class Type : uint8_t {
        Draw = 0,
        EfbCopy = 1,
    };
    Type type{Type::Draw};
    GxDrawCommand draw{};
    GxEfbCopyCommand efb_copy{};
};

struct GxFrameCommands {
    uint32_t frame_width{640};
    uint32_t frame_height{480};
    float aspect_ratio{4.0f / 3.0f};
    bool hor_plus{true};
    std::vector<GxVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GxCommandItem> items;
    uint32_t efb_copy_count{0};
    uint32_t max_tev_stages{1};
};

// Thread-safe GameCube GX State Machine & Live Command/FIFO Processor.
class GxStateProcessor {
public:
    static constexpr size_t kFifoRingBytes = 1024 * 1024;
    static constexpr uint32_t kEfbTexIdBase = 0x10000u;

    GxStateProcessor();

    void reset_state();
    void begin_frame(uint32_t width, uint32_t height, float target_aspect, bool enable_hor_plus = true);
    void end_frame();

    // Matrix & projection state
    void set_projection(const float row_major_4x4[16], bool is_ortho);
    void load_pos_mtx(uint32_t slot, const float row_major_3x4[12]);
    void set_current_mtx(uint32_t slot);

    // Vertex descriptor & raster/blend state
    void set_vtx_desc(uint32_t mask, bool use_vtxfmt1 = false);
    void set_cull_mode(GxCullMode cull);
    void set_blend_mode(GxBlendMode blend);
    void set_z_mode(bool depth_test, GxCompare func, bool depth_write);
    void set_material_color(float r, float g, float b, float a);
    void set_ambient_color(float r, float g, float b, float a);
    void set_light_dir(float x, float y, float z, float intensity);

    // TEV, Alpha Compare & Fog state
    void set_tev_num_stages(uint32_t num_stages);
    void set_tev_stage(uint32_t stage_idx, const GxTevStage& stage);
    void set_tev_reg(uint32_t reg_idx, float r, float g, float b, float a);
    void set_tev_konst(uint32_t reg_idx, float r, float g, float b, float a);
    void set_alpha_compare(GxCompare comp0, uint8_t ref0, GxAlphaOp op, GxCompare comp1, uint8_t ref1);
    void set_fog(GxFogType type, float start_z, float end_z, float near_z, float far_z,
                 float r, float g, float b, float a);
    void set_lighting_enabled(bool enabled);
    void set_textures_enabled(bool enabled);
    void bind_texture(uint32_t tex_map, int32_t tex_id);

    // Immediate primitive submission (GXBegin / GXPosition / GXEnd style)
    void begin_primitive(uint8_t gx_prim_opcode, bool has_vertex_color = true, bool env_map_uv1 = false);
    void push_vertex(const GxVertex& v);
    void end_primitive();

    // EFB copy-to-texture
    void request_efb_copy(const GxEfbCopyCommand& cmd);

    // Raw GX FIFO stream submission (from C bridge fzgx_gx_fifo_*)
    void write_fifo_u8(uint8_t v);
    void write_fifo_be16(uint16_t v);
    void write_fifo_be32(uint32_t v);
    void write_fifo_f32(float v);
    void flush_fifo_commands();

    bool submit_fifo_bytes(std::span<const uint8_t> bytes, std::string* error_out = nullptr);

    const GxFrameCommands& completed_frame() const { return completed_; }
    const GxFrameCommands& active_frame() const { return active_; }
    uint64_t frames_ended() const;
    size_t last_fifo_bytes() const;

private:
    void compute_mvp_and_mv(std::array<float, 16>& out_mvp,
                            std::array<float, 16>& out_mv,
                            std::array<float, 16>& out_nrm) const;
    void emit_draw(uint32_t first_index, uint32_t index_count, bool has_vtx_color, bool env_map_uv1);
    void apply_bp_register(uint32_t reg_val);
    void apply_xf_register(uint16_t addr, uint32_t val);

    mutable std::mutex mu_;

    std::array<std::array<float, 12>, 10> pos_mtx_{};
    uint32_t current_mtx_{0};
    std::array<float, 16> proj_mtx_{};
    bool is_ortho_{false};

    uint32_t vtx_desc_mask_{(1u << 9) | (1u << 10) | (1u << 11) | (1u << 13)};
    bool use_vtxfmt1_{false};
    GxCullMode cull_{GxCullMode::Back};
    GxBlendMode blend_{GxBlendMode::Opaque};
    bool depth_test_{true};
    bool depth_write_{true};
    GxCompare depth_func_{GxCompare::LEqual};

    std::array<float, 4> mat_color_{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> ambient_color_{0.42f, 0.44f, 0.50f, 1.0f};
    std::array<float, 4> light_dir_intensity_{0.45f, 0.78f, 0.42f, 1.0f};
    GxTevConfig tev_{};
    std::array<int32_t, 4> bound_tex_{-1, -1, -1, -1};

    // Active primitive assembly
    bool in_primitive_{false};
    uint8_t active_prim_opcode_{0x90u};
    bool active_prim_has_vtx_color_{true};
    bool active_prim_env_map_{false};
    std::vector<GxVertex> prim_verts_;

    // Raw FIFO staging
    std::vector<uint8_t> fifo_staging_;
    size_t last_fifo_bytes_{0};
    uint64_t frames_ended_{0};

    GxFrameCommands active_{};
    GxFrameCommands completed_{};
};

GxStateProcessor& global_gx_state();

} // namespace fzgx
