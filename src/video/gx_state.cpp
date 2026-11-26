#include "video/gx_state.hpp"
#include "assets/byte_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fzgx {

namespace {

std::array<float, 16> mul_col_major(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = sum;
        }
    }
    return r;
}

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

bool eval_alpha_cmp(float a, float ref, GxCompare cmp) {
    switch (cmp) {
    case GxCompare::Never: return false;
    case GxCompare::Less: return a < ref;
    case GxCompare::Equal: return std::fabs(a - ref) < (0.5f / 255.0f);
    case GxCompare::LEqual: return a <= ref;
    case GxCompare::Greater: return a > ref;
    case GxCompare::NEqual: return std::fabs(a - ref) >= (0.5f / 255.0f);
    case GxCompare::GEqual: return a >= ref;
    case GxCompare::Always:
    default: return true;
    }
}

std::array<float, 4> apply_swap(const std::array<float, 4>& c, const std::array<uint8_t, 4>& sw) {
    return {
        c[std::min<uint8_t>(sw[0], 3)],
        c[std::min<uint8_t>(sw[1], 3)],
        c[std::min<uint8_t>(sw[2], 3)],
        c[std::min<uint8_t>(sw[3], 3)],
    };
}

std::array<float, 3> eval_konst_rgb(uint8_t sel, const std::array<std::array<float, 4>, 4>& k) {
    if (sel <= 7) {
        const float f = static_cast<float>(8 - sel) / 8.0f;
        return {f, f, f};
    }
    if (sel >= 12 && sel <= 15) {
        const auto& r = k[sel - 12];
        return {r[0], r[1], r[2]};
    }
    if (sel >= 16 && sel <= 19) {
        const float v = k[sel - 16][0];
        return {v, v, v};
    }
    if (sel >= 20 && sel <= 23) {
        const float v = k[sel - 20][1];
        return {v, v, v};
    }
    if (sel >= 24 && sel <= 27) {
        const float v = k[sel - 24][2];
        return {v, v, v};
    }
    if (sel >= 28 && sel <= 31) {
        const float v = k[sel - 28][3];
        return {v, v, v};
    }
    return {1.0f, 1.0f, 1.0f};
}

float eval_konst_alpha(uint8_t sel, const std::array<std::array<float, 4>, 4>& k) {
    if (sel <= 7) {
        return static_cast<float>(8 - sel) / 8.0f;
    }
    if (sel >= 16 && sel <= 19) return k[sel - 16][0];
    if (sel >= 20 && sel <= 23) return k[sel - 20][1];
    if (sel >= 24 && sel <= 27) return k[sel - 24][2];
    if (sel >= 28 && sel <= 31) return k[sel - 28][3];
    return 1.0f;
}

std::array<float, 3> pick_color_arg(uint8_t arg,
                                    const std::array<std::array<float, 4>, 4>& regs,
                                    const std::array<float, 4>& tex,
                                    const std::array<float, 4>& ras,
                                    const std::array<float, 3>& konst) {
    switch (static_cast<GxTevColorArg>(arg)) {
    case GxTevColorArg::CPrev: return {regs[0][0], regs[0][1], regs[0][2]};
    case GxTevColorArg::APrev: return {regs[0][3], regs[0][3], regs[0][3]};
    case GxTevColorArg::C0: return {regs[1][0], regs[1][1], regs[1][2]};
    case GxTevColorArg::A0: return {regs[1][3], regs[1][3], regs[1][3]};
    case GxTevColorArg::C1: return {regs[2][0], regs[2][1], regs[2][2]};
    case GxTevColorArg::A1: return {regs[2][3], regs[2][3], regs[2][3]};
    case GxTevColorArg::C2: return {regs[3][0], regs[3][1], regs[3][2]};
    case GxTevColorArg::A2: return {regs[3][3], regs[3][3], regs[3][3]};
    case GxTevColorArg::TexC: return {tex[0], tex[1], tex[2]};
    case GxTevColorArg::TexA: return {tex[3], tex[3], tex[3]};
    case GxTevColorArg::RasC: return {ras[0], ras[1], ras[2]};
    case GxTevColorArg::RasA: return {ras[3], ras[3], ras[3]};
    case GxTevColorArg::One: return {1.0f, 1.0f, 1.0f};
    case GxTevColorArg::Half: return {0.5f, 0.5f, 0.5f};
    case GxTevColorArg::Konst: return konst;
    case GxTevColorArg::Zero:
    default: return {0.0f, 0.0f, 0.0f};
    }
}

float pick_alpha_arg(uint8_t arg,
                     const std::array<std::array<float, 4>, 4>& regs,
                     const std::array<float, 4>& tex,
                     const std::array<float, 4>& ras,
                     float konst_a) {
    switch (static_cast<GxTevAlphaArg>(arg)) {
    case GxTevAlphaArg::APrev: return regs[0][3];
    case GxTevAlphaArg::A0: return regs[1][3];
    case GxTevAlphaArg::A1: return regs[2][3];
    case GxTevAlphaArg::A2: return regs[3][3];
    case GxTevAlphaArg::TexA: return tex[3];
    case GxTevAlphaArg::RasA: return ras[3];
    case GxTevAlphaArg::Konst: return konst_a;
    case GxTevAlphaArg::Zero:
    default: return 0.0f;
    }
}

float tev_bias_val(uint8_t b) {
    if (b == 1) return 0.5f;
    if (b == 2) return -0.5f;
    return 0.0f;
}

float tev_scale_val(uint8_t s) {
    switch (s) {
    case 1: return 2.0f;
    case 2: return 4.0f;
    case 3: return 0.5f;
    default: return 1.0f;
    }
}

} // namespace

GxTevConfig make_default_tev_config(bool textured, bool lit) {
    GxTevConfig cfg{};
    cfg.num_stages = 1;
    cfg.enable_lighting = lit;
    cfg.enable_textures = textured;

    auto& s0 = cfg.stages[0];
    if (textured) {
        s0.tex_map = 0;
        s0.tex_coord = 0;
        s0.color_in = {
            static_cast<uint8_t>(GxTevColorArg::Zero),
            static_cast<uint8_t>(GxTevColorArg::TexC),
            static_cast<uint8_t>(GxTevColorArg::RasC),
            static_cast<uint8_t>(GxTevColorArg::Zero),
        };
        s0.alpha_in = {
            static_cast<uint8_t>(GxTevAlphaArg::Zero),
            static_cast<uint8_t>(GxTevAlphaArg::TexA),
            static_cast<uint8_t>(GxTevAlphaArg::RasA),
            static_cast<uint8_t>(GxTevAlphaArg::Zero),
        };
    } else {
        s0.tex_map = 0xFF;
        s0.tex_coord = 0xFF;
        s0.color_in = {
            static_cast<uint8_t>(GxTevColorArg::Zero),
            static_cast<uint8_t>(GxTevColorArg::Zero),
            static_cast<uint8_t>(GxTevColorArg::Zero),
            static_cast<uint8_t>(GxTevColorArg::RasC),
        };
        s0.alpha_in = {
            static_cast<uint8_t>(GxTevAlphaArg::Zero),
            static_cast<uint8_t>(GxTevAlphaArg::Zero),
            static_cast<uint8_t>(GxTevAlphaArg::Zero),
            static_cast<uint8_t>(GxTevAlphaArg::RasA),
        };
    }
    return cfg;
}

GxTevConfig build_gma_submesh_tev(const GmaSubMesh& sm,
                                  const std::vector<GmaMaterialLayer>& layers) {
    const bool has_primary = sm.primary_tpl_index >= 0;
    const bool has_secondary = sm.secondary_tpl_index >= 0;
    GxTevConfig cfg = make_default_tev_config(has_primary, !sm.unlit);

    if (sm.blend_mode == GxBlendMode::Cutout) {
        cfg.alpha_comp0 = GxCompare::GEqual;
        cfg.alpha_ref0 = 0.5f;
        cfg.alpha_op = GxAlphaOp::And;
        cfg.alpha_comp1 = GxCompare::Always;
        cfg.alpha_ref1 = 0.0f;
    }

    if (has_primary && has_secondary) {
        bool sec_is_env = false;
        bool sec_is_alpha_mask = false;
        for (const auto& l : layers) {
            if (static_cast<int32_t>(l.tpl_index) == sm.secondary_tpl_index) {
                sec_is_env = l.is_env_map;
                sec_is_alpha_mask = l.is_alpha_mask;
                break;
            }
        }

        cfg.num_stages = 2;
        auto& s1 = cfg.stages[1];
        s1.tex_map = 1;
        s1.tex_coord = 1;
        s1.ras_chan = 0xFF;
        if (sec_is_alpha_mask) {
            // Preserve RGB from stage 0, multiply alpha by layer 1
            s1.color_in = {
                static_cast<uint8_t>(GxTevColorArg::Zero),
                static_cast<uint8_t>(GxTevColorArg::Zero),
                static_cast<uint8_t>(GxTevColorArg::Zero),
                static_cast<uint8_t>(GxTevColorArg::CPrev),
            };
            s1.alpha_in = {
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::APrev),
                static_cast<uint8_t>(GxTevAlphaArg::TexA),
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
            };
        } else if (sec_is_env) {
            // Subtle reflection/specular blend controlled by Konst0 (0.35)
            cfg.konst_regs[0] = {0.35f, 0.35f, 0.35f, 1.0f};
            s1.kcolor_sel = 12; // K0
            s1.color_in = {
                static_cast<uint8_t>(GxTevColorArg::CPrev),
                static_cast<uint8_t>(GxTevColorArg::TexC),
                static_cast<uint8_t>(GxTevColorArg::Konst),
                static_cast<uint8_t>(GxTevColorArg::Zero),
            };
            s1.alpha_in = {
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::APrev),
            };
        } else {
            // Modulate with secondary map (e.g. detail/lightmap)
            cfg.konst_regs[0] = {0.4f, 0.4f, 0.4f, 1.0f};
            s1.kcolor_sel = 12;
            s1.color_in = {
                static_cast<uint8_t>(GxTevColorArg::CPrev),
                static_cast<uint8_t>(GxTevColorArg::TexC),
                static_cast<uint8_t>(GxTevColorArg::Konst),
                static_cast<uint8_t>(GxTevColorArg::Zero),
            };
            s1.alpha_in = {
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::Zero),
                static_cast<uint8_t>(GxTevAlphaArg::APrev),
            };
        }
    }

    return cfg;
}

bool evaluate_tev_cpu(const GxTevConfig& cfg,
                      const std::array<float, 4>& ras_color,
                      const std::array<std::array<float, 4>, 4>& tex_samples,
                      std::array<float, 4>& out_rgba) {
    std::array<std::array<float, 4>, 4> regs = cfg.tev_regs;
    const uint32_t n = std::clamp<uint32_t>(cfg.num_stages, 1u, 16u);

    for (uint32_t si = 0; si < n; ++si) {
        const auto& st = cfg.stages[si];
        const auto ras = apply_swap(ras_color, cfg.swap_table[std::min<uint8_t>(st.ras_swap, 3)]);
        std::array<float, 4> tex{1.0f, 1.0f, 1.0f, 1.0f};
        if (st.tex_map < 4 && cfg.enable_textures) {
            tex = apply_swap(tex_samples[st.tex_map], cfg.swap_table[std::min<uint8_t>(st.tex_swap, 3)]);
        }

        const auto kc = eval_konst_rgb(st.kcolor_sel, cfg.konst_regs);
        const float ka = eval_konst_alpha(st.kalpha_sel, cfg.konst_regs);

        const auto ca = pick_color_arg(st.color_in[0], regs, tex, ras, kc);
        const auto cb = pick_color_arg(st.color_in[1], regs, tex, ras, kc);
        const auto cc = pick_color_arg(st.color_in[2], regs, tex, ras, kc);
        const auto cd = pick_color_arg(st.color_in[3], regs, tex, ras, kc);

        const float csgn = (st.color_op == 1) ? -1.0f : 1.0f;
        const float cbias = tev_bias_val(st.color_bias);
        const float cscale = tev_scale_val(st.color_scale);
        std::array<float, 3> cout{};
        for (int k = 0; k < 3; ++k) {
            const float lerp = ca[k] * (1.0f - cc[k]) + cb[k] * cc[k];
            float v = (cd[k] + csgn * lerp + cbias) * cscale;
            if (st.color_clamp) v = clamp01(v);
            cout[k] = v;
        }

        const float aa = pick_alpha_arg(st.alpha_in[0], regs, tex, ras, ka);
        const float ab = pick_alpha_arg(st.alpha_in[1], regs, tex, ras, ka);
        const float ac = pick_alpha_arg(st.alpha_in[2], regs, tex, ras, ka);
        const float ad = pick_alpha_arg(st.alpha_in[3], regs, tex, ras, ka);

        const float asgn = (st.alpha_op == 1) ? -1.0f : 1.0f;
        const float abias = tev_bias_val(st.alpha_bias);
        const float ascale = tev_scale_val(st.alpha_scale);
        float aout = (ad + asgn * (aa * (1.0f - ac) + ab * ac) + abias) * ascale;
        if (st.alpha_clamp) aout = clamp01(aout);

        const uint8_t creg = std::min<uint8_t>(st.color_out_reg, 3);
        const uint8_t areg = std::min<uint8_t>(st.alpha_out_reg, 3);
        regs[creg][0] = cout[0];
        regs[creg][1] = cout[1];
        regs[creg][2] = cout[2];
        regs[areg][3] = aout;
    }

    out_rgba = regs[0];
    const bool c0 = eval_alpha_cmp(out_rgba[3], cfg.alpha_ref0, cfg.alpha_comp0);
    const bool c1 = eval_alpha_cmp(out_rgba[3], cfg.alpha_ref1, cfg.alpha_comp1);
    switch (cfg.alpha_op) {
    case GxAlphaOp::And: return c0 && c1;
    case GxAlphaOp::Or: return c0 || c1;
    case GxAlphaOp::Xor: return c0 != c1;
    case GxAlphaOp::Xnor: return c0 == c1;
    default: return true;
    }
}

std::array<float, 16> gx_make_perspective_mtx(float fov_y_deg, float aspect, float znear, float zfar) {
    const float half_rad = (fov_y_deg * 0.5f) * (3.1415926535f / 180.0f);
    const float cot = 1.0f / std::tan(half_rad);
    const float inv_range = 1.0f / std::max(1e-5f, zfar - znear);
    std::array<float, 16> m{};
    // Row-major matching GameCube C_MTXPerspective
    m[0] = cot / std::max(0.05f, aspect);
    m[5] = cot;
    m[10] = -znear * inv_range;
    m[11] = -(zfar * znear) * inv_range;
    m[14] = -1.0f;
    return m;
}

std::array<float, 16> gx_make_ortho_mtx(float top, float bottom, float left, float right, float znear, float zfar) {
    std::array<float, 16> m{};
    const float rl = std::max(1e-5f, right - left);
    const float tb = std::max(1e-5f, top - bottom);
    const float fn = std::max(1e-5f, zfar - znear);
    m[0] = 2.0f / rl;
    m[3] = -(right + left) / rl;
    m[5] = 2.0f / tb;
    m[7] = -(top + bottom) / tb;
    m[10] = -1.0f / fn;
    m[11] = -zfar / fn;
    m[15] = 1.0f;
    return m;
}

std::array<float, 16> gx_build_vulkan_projection(const float gx_proj_row_major[16],
                                                 bool is_ortho,
                                                 float target_aspect,
                                                 bool enable_hor_plus,
                                                 float original_aspect) {
    // Convert row-major GameCube projection (NDC Z in [-1, 0], Y up) to
    // column-major Vulkan projection (NDC Z in [0, 1], Y down):
    //   row1_vk = -row1_gx
    //   row2_vk = row2_gx + row3_gx
    std::array<float, 4> r0{gx_proj_row_major[0], gx_proj_row_major[1], gx_proj_row_major[2], gx_proj_row_major[3]};
    std::array<float, 4> r1{-gx_proj_row_major[4], -gx_proj_row_major[5], -gx_proj_row_major[6], -gx_proj_row_major[7]};
    std::array<float, 4> r2{
        gx_proj_row_major[8] + gx_proj_row_major[12],
        gx_proj_row_major[9] + gx_proj_row_major[13],
        gx_proj_row_major[10] + gx_proj_row_major[14],
        gx_proj_row_major[11] + gx_proj_row_major[15],
    };
    std::array<float, 4> r3{gx_proj_row_major[12], gx_proj_row_major[13], gx_proj_row_major[14], gx_proj_row_major[15]};

    if (!is_ortho && enable_hor_plus && target_aspect > 0.05f && original_aspect > 0.05f) {
        const float hor_scale = original_aspect / target_aspect;
        r0[0] *= hor_scale;
    }

    // Store column-major
    std::array<float, 16> col{};
    for (int c = 0; c < 4; ++c) {
        col[c * 4 + 0] = r0[c];
        col[c * 4 + 1] = r1[c];
        col[c * 4 + 2] = r2[c];
        col[c * 4 + 3] = r3[c];
    }
    return col;
}

GxStateProcessor::GxStateProcessor() {
    reset_state();
}

void GxStateProcessor::reset_state() {
    std::lock_guard lock(mu_);
    for (auto& mtx : pos_mtx_) {
        mtx = {1.0f, 0.0f, 0.0f, 0.0f,
               0.0f, 1.0f, 0.0f, 0.0f,
               0.0f, 0.0f, 1.0f, 0.0f};
    }
    current_mtx_ = 0;
    proj_mtx_ = gx_make_perspective_mtx(50.0f, 4.0f / 3.0f, 0.2f, 2000.0f);
    is_ortho_ = false;
    vtx_desc_mask_ = (1u << 9) | (1u << 10) | (1u << 11) | (1u << 13);
    use_vtxfmt1_ = false;
    cull_ = GxCullMode::Back;
    blend_ = GxBlendMode::Opaque;
    depth_test_ = true;
    depth_write_ = true;
    depth_func_ = GxCompare::LEqual;
    mat_color_ = {1.0f, 1.0f, 1.0f, 1.0f};
    ambient_color_ = {0.42f, 0.44f, 0.50f, 1.0f};
    light_dir_intensity_ = {0.45f, 0.78f, 0.42f, 1.0f};
    tev_ = make_default_tev_config(true, true);
    bound_tex_ = {-1, -1, -1, -1};
    in_primitive_ = false;
    prim_verts_.clear();
    fifo_staging_.clear();
}

void GxStateProcessor::begin_frame(uint32_t width, uint32_t height, float target_aspect, bool enable_hor_plus) {
    std::lock_guard lock(mu_);
    active_.frame_width = width;
    active_.frame_height = height;
    active_.aspect_ratio = target_aspect;
    active_.hor_plus = enable_hor_plus;
    active_.vertices.clear();
    active_.indices.clear();
    active_.items.clear();
    active_.efb_copy_count = 0;
    active_.max_tev_stages = 1;
    in_primitive_ = false;
    prim_verts_.clear();
    fifo_staging_.clear();
}

void GxStateProcessor::end_frame() {
    flush_fifo_commands();
    std::lock_guard lock(mu_);
    if (in_primitive_) {
        in_primitive_ = false;
        prim_verts_.clear();
    }
    completed_ = active_;
    ++frames_ended_;
}

void GxStateProcessor::set_projection(const float row_major_4x4[16], bool is_ortho) {
    std::lock_guard lock(mu_);
    std::memcpy(proj_mtx_.data(), row_major_4x4, sizeof(float) * 16);
    is_ortho_ = is_ortho;
}

void GxStateProcessor::load_pos_mtx(uint32_t slot, const float row_major_3x4[12]) {
    std::lock_guard lock(mu_);
    if (slot < pos_mtx_.size()) {
        std::memcpy(pos_mtx_[slot].data(), row_major_3x4, sizeof(float) * 12);
    }
}

void GxStateProcessor::set_current_mtx(uint32_t slot) {
    std::lock_guard lock(mu_);
    if (slot < pos_mtx_.size()) {
        current_mtx_ = slot;
    }
}

void GxStateProcessor::set_vtx_desc(uint32_t mask, bool use_vtxfmt1) {
    std::lock_guard lock(mu_);
    vtx_desc_mask_ = mask;
    use_vtxfmt1_ = use_vtxfmt1;
}

void GxStateProcessor::set_cull_mode(GxCullMode cull) {
    std::lock_guard lock(mu_);
    cull_ = cull;
}

void GxStateProcessor::set_blend_mode(GxBlendMode blend) {
    std::lock_guard lock(mu_);
    blend_ = blend;
}

void GxStateProcessor::set_z_mode(bool depth_test, GxCompare func, bool depth_write) {
    std::lock_guard lock(mu_);
    depth_test_ = depth_test;
    depth_func_ = func;
    depth_write_ = depth_write;
}

void GxStateProcessor::set_material_color(float r, float g, float b, float a) {
    std::lock_guard lock(mu_);
    mat_color_ = {r, g, b, a};
}

void GxStateProcessor::set_ambient_color(float r, float g, float b, float a) {
    std::lock_guard lock(mu_);
    ambient_color_ = {r, g, b, a};
}

void GxStateProcessor::set_light_dir(float x, float y, float z, float intensity) {
    std::lock_guard lock(mu_);
    light_dir_intensity_ = {x, y, z, intensity};
}

void GxStateProcessor::set_tev_num_stages(uint32_t num_stages) {
    std::lock_guard lock(mu_);
    tev_.num_stages = std::clamp<uint32_t>(num_stages, 1u, 16u);
}

void GxStateProcessor::set_tev_stage(uint32_t stage_idx, const GxTevStage& stage) {
    std::lock_guard lock(mu_);
    if (stage_idx < 16u) {
        tev_.stages[stage_idx] = stage;
    }
}

void GxStateProcessor::set_tev_reg(uint32_t reg_idx, float r, float g, float b, float a) {
    std::lock_guard lock(mu_);
    if (reg_idx < 4u) {
        tev_.tev_regs[reg_idx] = {r, g, b, a};
    }
}

void GxStateProcessor::set_tev_konst(uint32_t reg_idx, float r, float g, float b, float a) {
    std::lock_guard lock(mu_);
    if (reg_idx < 4u) {
        tev_.konst_regs[reg_idx] = {r, g, b, a};
    }
}

void GxStateProcessor::set_alpha_compare(GxCompare comp0, uint8_t ref0, GxAlphaOp op, GxCompare comp1, uint8_t ref1) {
    std::lock_guard lock(mu_);
    tev_.alpha_comp0 = comp0;
    tev_.alpha_ref0 = static_cast<float>(ref0) / 255.0f;
    tev_.alpha_op = op;
    tev_.alpha_comp1 = comp1;
    tev_.alpha_ref1 = static_cast<float>(ref1) / 255.0f;
}

void GxStateProcessor::set_fog(GxFogType type, float start_z, float end_z, float near_z, float far_z,
                               float r, float g, float b, float a) {
    std::lock_guard lock(mu_);
    tev_.fog_type = type;
    tev_.fog_start_z = start_z;
    tev_.fog_end_z = end_z;
    tev_.fog_near_z = near_z;
    tev_.fog_far_z = far_z;
    tev_.fog_color = {r, g, b, a};
    tev_.enable_fog = (type != GxFogType::None);
}

void GxStateProcessor::set_lighting_enabled(bool enabled) {
    std::lock_guard lock(mu_);
    tev_.enable_lighting = enabled;
}

void GxStateProcessor::set_textures_enabled(bool enabled) {
    std::lock_guard lock(mu_);
    tev_.enable_textures = enabled;
}

void GxStateProcessor::bind_texture(uint32_t tex_map, int32_t tex_id) {
    std::lock_guard lock(mu_);
    if (tex_map < 4u) {
        bound_tex_[tex_map] = tex_id;
    }
}

void GxStateProcessor::compute_mvp_and_mv(std::array<float, 16>& out_mvp,
                                          std::array<float, 16>& out_mv,
                                          std::array<float, 16>& out_nrm) const {
    const auto& pm = pos_mtx_[current_mtx_];
    // Convert row-major 3x4 -> column-major 4x4
    std::array<float, 16> mv{};
    mv[0] = pm[0];  mv[4] = pm[1];  mv[8]  = pm[2];  mv[12] = pm[3];
    mv[1] = pm[4];  mv[5] = pm[5];  mv[9]  = pm[6];  mv[13] = pm[7];
    mv[2] = pm[8];  mv[6] = pm[9];  mv[10] = pm[10]; mv[14] = pm[11];
    mv[3] = 0.0f;   mv[7] = 0.0f;   mv[11] = 0.0f;   mv[15] = 1.0f;

    const auto proj_vk = gx_build_vulkan_projection(
        proj_mtx_.data(), is_ortho_, active_.aspect_ratio, active_.hor_plus, 4.0f / 3.0f);

    out_mv = mv;
    out_nrm = mv;
    out_mvp = mul_col_major(proj_vk, mv);
}

void GxStateProcessor::emit_draw(uint32_t first_index, uint32_t index_count, bool has_vtx_color, bool env_map_uv1) {
    if (index_count == 0) return;

    GxCommandItem item{};
    item.type = GxCommandItem::Type::Draw;
    auto& d = item.draw;
    d.first_index = first_index;
    d.index_count = index_count;
    compute_mvp_and_mv(d.mvp, d.modelview, d.normal_mat);
    d.mat_color = mat_color_;
    d.ambient_color = ambient_color_;
    d.light_dir_intensity = light_dir_intensity_;
    d.tev = tev_;
    d.cull = cull_;
    d.blend = blend_;
    d.depth_test = depth_test_;
    d.depth_write = depth_write_;
    d.depth_func = depth_func_;
    d.tex_ids = bound_tex_;
    d.has_vertex_color = has_vtx_color;
    d.env_map_uv1 = env_map_uv1;

    active_.max_tev_stages = std::max(active_.max_tev_stages, tev_.num_stages);
    active_.items.push_back(std::move(item));
}

void GxStateProcessor::begin_primitive(uint8_t gx_prim_opcode, bool has_vertex_color, bool env_map_uv1) {
    std::lock_guard lock(mu_);
    in_primitive_ = true;
    active_prim_opcode_ = gx_prim_opcode & 0xF8u;
    active_prim_has_vtx_color_ = has_vertex_color;
    active_prim_env_map_ = env_map_uv1;
    prim_verts_.clear();
}

void GxStateProcessor::push_vertex(const GxVertex& v) {
    std::lock_guard lock(mu_);
    if (in_primitive_) {
        prim_verts_.push_back(v);
    }
}

void GxStateProcessor::end_primitive() {
    std::lock_guard lock(mu_);
    if (!in_primitive_) return;
    in_primitive_ = false;
    if (prim_verts_.size() < 3) {
        prim_verts_.clear();
        return;
    }

    const uint32_t vbase = static_cast<uint32_t>(active_.vertices.size());
    const uint32_t ibase = static_cast<uint32_t>(active_.indices.size());
    active_.vertices.insert(active_.vertices.end(), prim_verts_.begin(), prim_verts_.end());

    const uint32_t n = static_cast<uint32_t>(prim_verts_.size());
    if (active_prim_opcode_ == 0x90u) { // TRIANGLES
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            active_.indices.push_back(vbase + i);
            active_.indices.push_back(vbase + i + 1);
            active_.indices.push_back(vbase + i + 2);
        }
    } else if (active_prim_opcode_ == 0x98u) { // TRIANGLESTRIP
        for (uint32_t i = 2; i < n; ++i) {
            if ((i & 1u) == 0) {
                active_.indices.push_back(vbase + i - 2);
                active_.indices.push_back(vbase + i - 1);
                active_.indices.push_back(vbase + i);
            } else {
                active_.indices.push_back(vbase + i - 1);
                active_.indices.push_back(vbase + i - 2);
                active_.indices.push_back(vbase + i);
            }
        }
    } else if (active_prim_opcode_ == 0xA0u) { // TRIANGLEFAN
        for (uint32_t i = 2; i < n; ++i) {
            active_.indices.push_back(vbase);
            active_.indices.push_back(vbase + i - 1);
            active_.indices.push_back(vbase + i);
        }
    } else if (active_prim_opcode_ == 0x80u) { // QUADS
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            active_.indices.push_back(vbase + i);
            active_.indices.push_back(vbase + i + 1);
            active_.indices.push_back(vbase + i + 2);
            active_.indices.push_back(vbase + i);
            active_.indices.push_back(vbase + i + 2);
            active_.indices.push_back(vbase + i + 3);
        }
    }

    prim_verts_.clear();
    const uint32_t icount = static_cast<uint32_t>(active_.indices.size()) - ibase;
    emit_draw(ibase, icount, active_prim_has_vtx_color_, active_prim_env_map_);
}

void GxStateProcessor::request_efb_copy(const GxEfbCopyCommand& cmd) {
    std::lock_guard lock(mu_);
    GxCommandItem item{};
    item.type = GxCommandItem::Type::EfbCopy;
    item.efb_copy = cmd;
    ++active_.efb_copy_count;
    active_.items.push_back(std::move(item));
}

void GxStateProcessor::write_fifo_u8(uint8_t v) {
    std::lock_guard lock(mu_);
    if (fifo_staging_.size() < kFifoRingBytes) {
        fifo_staging_.push_back(v);
    }
}

void GxStateProcessor::write_fifo_be16(uint16_t v) {
    std::lock_guard lock(mu_);
    if (fifo_staging_.size() + 2 <= kFifoRingBytes) {
        fifo_staging_.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        fifo_staging_.push_back(static_cast<uint8_t>(v & 0xFFu));
    }
}

void GxStateProcessor::write_fifo_be32(uint32_t v) {
    std::lock_guard lock(mu_);
    if (fifo_staging_.size() + 4 <= kFifoRingBytes) {
        fifo_staging_.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
        fifo_staging_.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        fifo_staging_.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        fifo_staging_.push_back(static_cast<uint8_t>(v & 0xFFu));
    }
}

void GxStateProcessor::write_fifo_f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_fifo_be32(bits);
}

void GxStateProcessor::flush_fifo_commands() {
    std::vector<uint8_t> buf;
    {
        std::lock_guard lock(mu_);
        if (fifo_staging_.empty()) return;
        last_fifo_bytes_ = fifo_staging_.size();
        buf.swap(fifo_staging_);
    }
    submit_fifo_bytes(buf, nullptr);
}

void GxStateProcessor::apply_bp_register(uint32_t reg_val) {
    const uint8_t reg_id = static_cast<uint8_t>((reg_val >> 24) & 0xFFu);
    if (reg_id == 0xF2u) { // GX_BP_REG_FOGCOLOR
        tev_.fog_color[0] = static_cast<float>((reg_val >> 16) & 0xFFu) / 255.0f;
        tev_.fog_color[1] = static_cast<float>((reg_val >> 8) & 0xFFu) / 255.0f;
        tev_.fog_color[2] = static_cast<float>(reg_val & 0xFFu) / 255.0f;
        tev_.fog_color[3] = 1.0f;
    } else if (reg_id == 0xF3u) { // GX_BP_REG_ALPHACOMPARE
        tev_.alpha_ref0 = static_cast<float>(reg_val & 0xFFu) / 255.0f;
        tev_.alpha_ref1 = static_cast<float>((reg_val >> 8) & 0xFFu) / 255.0f;
        tev_.alpha_comp0 = static_cast<GxCompare>((reg_val >> 16) & 7u);
        tev_.alpha_comp1 = static_cast<GxCompare>((reg_val >> 19) & 7u);
        tev_.alpha_op = static_cast<GxAlphaOp>((reg_val >> 22) & 3u);
    } else if (reg_id >= 0xC0u && reg_id <= 0xDFu) {
        const uint32_t stage = (reg_id - 0xC0u) >> 1;
        auto& st = tev_.stages[stage];
        if ((reg_id & 1u) == 0u) {
            // Color combiner
            st.color_in[3] = static_cast<uint8_t>(reg_val & 0xFu);
            st.color_in[2] = static_cast<uint8_t>((reg_val >> 4) & 0xFu);
            st.color_in[1] = static_cast<uint8_t>((reg_val >> 8) & 0xFu);
            st.color_in[0] = static_cast<uint8_t>((reg_val >> 12) & 0xFu);
            st.color_bias = static_cast<uint8_t>((reg_val >> 16) & 3u);
            st.color_op = static_cast<uint8_t>((reg_val >> 18) & 1u);
            st.color_clamp = ((reg_val >> 19) & 1u) != 0;
            st.color_scale = static_cast<uint8_t>((reg_val >> 20) & 3u);
            st.color_out_reg = static_cast<uint8_t>((reg_val >> 22) & 3u);
        } else {
            // Alpha combiner
            st.alpha_in[3] = static_cast<uint8_t>((reg_val >> 4) & 7u);
            st.alpha_in[2] = static_cast<uint8_t>((reg_val >> 7) & 7u);
            st.alpha_in[1] = static_cast<uint8_t>((reg_val >> 10) & 7u);
            st.alpha_in[0] = static_cast<uint8_t>((reg_val >> 13) & 7u);
            st.alpha_bias = static_cast<uint8_t>((reg_val >> 16) & 3u);
            st.alpha_op = static_cast<uint8_t>((reg_val >> 18) & 1u);
            st.alpha_clamp = ((reg_val >> 19) & 1u) != 0;
            st.alpha_scale = static_cast<uint8_t>((reg_val >> 20) & 3u);
            st.alpha_out_reg = static_cast<uint8_t>((reg_val >> 22) & 3u);
        }
        tev_.num_stages = std::max(tev_.num_stages, stage + 1u);
    } else if (reg_id == 0x00u) { // GX_BP_REG_GENMODE
        const uint32_t num_tev = ((reg_val >> 10) & 0xFu) + 1u;
        tev_.num_stages = std::clamp<uint32_t>(num_tev, 1u, 16u);
    }
}

void GxStateProcessor::apply_xf_register(uint16_t addr, uint32_t val) {
    if (addr == 0x100Au) { // AMBIENT0
        ambient_color_[0] = static_cast<float>((val >> 24) & 0xFFu) / 255.0f;
        ambient_color_[1] = static_cast<float>((val >> 16) & 0xFFu) / 255.0f;
        ambient_color_[2] = static_cast<float>((val >> 8) & 0xFFu) / 255.0f;
        ambient_color_[3] = static_cast<float>(val & 0xFFu) / 255.0f;
    } else if (addr == 0x100Cu) { // MATERIAL0
        mat_color_[0] = static_cast<float>((val >> 24) & 0xFFu) / 255.0f;
        mat_color_[1] = static_cast<float>((val >> 16) & 0xFFu) / 255.0f;
        mat_color_[2] = static_cast<float>((val >> 8) & 0xFFu) / 255.0f;
        mat_color_[3] = static_cast<float>(val & 0xFFu) / 255.0f;
    }
}

bool GxStateProcessor::submit_fifo_bytes(std::span<const uint8_t> bytes, std::string* error_out) {
    std::lock_guard lock(mu_);
    ByteReader br(bytes);
    while (!br.at_eof()) {
        const uint8_t cmd = br.read_u8();
        if (!br.ok()) break;

        if (cmd == 0x00u) {
            continue;
        }
        if (cmd == 0x08u) { // CP reg
            const uint8_t cp_reg = br.read_u8();
            const uint32_t val = br.read_be32();
            if (!br.ok()) {
                if (error_out) *error_out = "Truncated GX CP register packet.";
                return false;
            }
            if ((cp_reg & 0xF0u) == 0x50u) {
                // VCD_LO bits: keep simple attr enable sync if needed
                (void)val;
            }
        } else if (cmd == 0x10u) { // XF reg
            const uint16_t count_m1 = br.read_be16();
            const uint16_t base_addr = br.read_be16();
            const uint32_t count = static_cast<uint32_t>(count_m1 & 0x0FFFu) + 1u;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t val = br.read_be32();
                if (!br.ok()) {
                    if (error_out) *error_out = "Truncated GX XF register packet.";
                    return false;
                }
                apply_xf_register(static_cast<uint16_t>(base_addr + i), val);
            }
        } else if (cmd == 0x61u) { // BP reg
            const uint32_t val = br.read_be32();
            if (!br.ok()) {
                if (error_out) *error_out = "Truncated GX BP register packet.";
                return false;
            }
            apply_bp_register(val);
        } else if (cmd == 0x20u || cmd == 0x28u || cmd == 0x30u || cmd == 0x38u) {
            br.skip(4);
        } else if ((cmd & 0xF8u) >= 0x80u && (cmd & 0xF8u) <= 0xB8u) {
            // Draw command: decode remaining display list chunk for this primitive
            const size_t prim_start = br.tell() - 1;
            const uint16_t vcount = ByteReader::peek_be16(bytes, prim_start + 1);
            // Compute exact vertex byte size from vtx_desc_mask_
            const bool is_fmt1 = use_vtxfmt1_ || ((cmd & 0x07u) == 1u);
            size_t vbytes = 0;
            for (uint32_t a = 0; a <= 8; ++a) {
                if (vtx_desc_mask_ & (1u << a)) vbytes += 1;
            }
            if (vtx_desc_mask_ & (1u << 9)) vbytes += (is_fmt1 ? 6u : 12u);
            if (vtx_desc_mask_ & (1u << 25)) vbytes += (is_fmt1 ? 18u : 36u);
            else if (vtx_desc_mask_ & (1u << 10)) vbytes += (is_fmt1 ? 6u : 12u);
            if (vtx_desc_mask_ & (1u << 11)) vbytes += 4u;
            if (vtx_desc_mask_ & (1u << 12)) vbytes += 4u;
            for (uint32_t a = 13; a <= 20; ++a) {
                if (vtx_desc_mask_ & (1u << a)) vbytes += ((is_fmt1 && a <= 15) ? 4u : 8u);
            }
            const size_t total_prim_bytes = 3u + static_cast<size_t>(vcount) * vbytes;
            if (prim_start + total_prim_bytes > bytes.size()) {
                if (error_out) *error_out = "Truncated GX FIFO draw packet.";
                return false;
            }

            std::vector<GxVertex> tmp_v;
            std::vector<uint32_t> tmp_i;
            GxDisplayListOptions opts{};
            opts.use_vtxfmt1 = use_vtxfmt1_;
            opts.vtx_desc_mask = vtx_desc_mask_;
            if (!decode_gx_display_list(bytes.subspan(prim_start, total_prim_bytes),
                                        opts, tmp_v, tmp_i, error_out)) {
                return false;
            }
            br.seek(prim_start + total_prim_bytes);

            if (!tmp_i.empty()) {
                const uint32_t vbase = static_cast<uint32_t>(active_.vertices.size());
                const uint32_t ibase = static_cast<uint32_t>(active_.indices.size());
                active_.vertices.insert(active_.vertices.end(), tmp_v.begin(), tmp_v.end());
                for (uint32_t idx : tmp_i) {
                    active_.indices.push_back(vbase + idx);
                }
                emit_draw(ibase, static_cast<uint32_t>(tmp_i.size()),
                          (vtx_desc_mask_ & (1u << 11)) != 0, false);
            }
        } else {
            if (error_out) {
                *error_out = "Unsupported GX FIFO opcode: 0x" + std::to_string(cmd);
            }
            return false;
        }
    }
    return true;
}

uint64_t GxStateProcessor::frames_ended() const {
    std::lock_guard lock(mu_);
    return frames_ended_;
}

size_t GxStateProcessor::last_fifo_bytes() const {
    std::lock_guard lock(mu_);
    return last_fifo_bytes_;
}

GxStateProcessor& global_gx_state() {
    static GxStateProcessor instance;
    return instance;
}

} // namespace fzgx
