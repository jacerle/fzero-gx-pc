#include "assets/gma_model.hpp"
#include "assets/avlz.hpp"
#include "assets/byte_reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fzgx {

namespace {

// GameCube GXAttr bit indices matching external/fzero-gx-decomp/include/dolphin/gx/GXPriv.h
constexpr uint32_t kAttrPnMtxIdx = 0;
constexpr uint32_t kAttrTex0MtxIdx = 1;
constexpr uint32_t kAttrTex7MtxIdx = 8;
constexpr uint32_t kAttrPos = 9;
constexpr uint32_t kAttrNrm = 10;
constexpr uint32_t kAttrClr0 = 11;
constexpr uint32_t kAttrClr1 = 12;
constexpr uint32_t kAttrTex0 = 13;
constexpr uint32_t kAttrTex2 = 15;
constexpr uint32_t kAttrTex7 = 20;
constexpr uint32_t kAttrNbt = 25;

GxWrapMode decode_wrap_mode(uint32_t bits) {
    switch (bits & 3u) {
    case 1: return GxWrapMode::Repeat;
    case 2: return GxWrapMode::Mirror;
    default: return GxWrapMode::Clamp;
    }
}

void decode_color_rgba8(ByteReader& br, float out[4]) {
    out[0] = static_cast<float>(br.read_u8()) / 255.0f;
    out[1] = static_cast<float>(br.read_u8()) / 255.0f;
    out[2] = static_cast<float>(br.read_u8()) / 255.0f;
    out[3] = static_cast<float>(br.read_u8()) / 255.0f;
}

bool read_gx_vertex(ByteReader& br, uint32_t mask, bool vtxfmt1, GxVertex& out) {
    out = GxVertex{};

    // Matrix indices (GX_VA_PNMTXIDX..GX_VA_TEX7MTXIDX) = 1 byte each when enabled
    for (uint32_t a = kAttrPnMtxIdx; a <= kAttrTex7MtxIdx; ++a) {
        if ((mask & (1u << a)) != 0) {
            br.read_u8();
        }
    }

    // Position (GX_VA_POS = bit 9): VTXFMT0 = 3x f32, VTXFMT1 = 3x s16 / 8192 (frac 13)
    if ((mask & (1u << kAttrPos)) != 0) {
        if (vtxfmt1) {
            constexpr float kPosScale = 1.0f / 8192.0f;
            out.pos[0] = static_cast<float>(br.read_s16_be()) * kPosScale;
            out.pos[1] = static_cast<float>(br.read_s16_be()) * kPosScale;
            out.pos[2] = static_cast<float>(br.read_s16_be()) * kPosScale;
        } else {
            out.pos[0] = br.read_f32_be();
            out.pos[1] = br.read_f32_be();
            out.pos[2] = br.read_f32_be();
        }
    }

    // Normal (GX_VA_NRM = bit 10) or NBT (bit 25)
    if ((mask & (1u << kAttrNbt)) != 0) {
        if (vtxfmt1) {
            constexpr float kNrmScale = 1.0f / 16384.0f;
            out.normal[0] = static_cast<float>(br.read_s16_be()) * kNrmScale;
            out.normal[1] = static_cast<float>(br.read_s16_be()) * kNrmScale;
            out.normal[2] = static_cast<float>(br.read_s16_be()) * kNrmScale;
            br.skip(12); // binormal + tangent (6 x s16)
        } else {
            out.normal[0] = br.read_f32_be();
            out.normal[1] = br.read_f32_be();
            out.normal[2] = br.read_f32_be();
            br.skip(24); // binormal + tangent (6 x f32)
        }
    } else if ((mask & (1u << kAttrNrm)) != 0) {
        if (vtxfmt1) {
            constexpr float kNrmScale = 1.0f / 16384.0f;
            out.normal[0] = static_cast<float>(br.read_s16_be()) * kNrmScale;
            out.normal[1] = static_cast<float>(br.read_s16_be()) * kNrmScale;
            out.normal[2] = static_cast<float>(br.read_s16_be()) * kNrmScale;
        } else {
            out.normal[0] = br.read_f32_be();
            out.normal[1] = br.read_f32_be();
            out.normal[2] = br.read_f32_be();
        }
    }

    // Colors (GX_VA_CLR0 = bit 11, GX_VA_CLR1 = bit 12): RGBA8 in both VTXFMT0 and VTXFMT1
    if ((mask & (1u << kAttrClr0)) != 0) {
        decode_color_rgba8(br, out.color);
    }
    if ((mask & (1u << kAttrClr1)) != 0) {
        float dummy[4];
        decode_color_rgba8(br, dummy);
    }

    // TexCoords (GX_VA_TEX0..GX_VA_TEX7 = bits 13..20)
    // avsystem.c: VTXFMT1 uses S16 frac 13 for TEX0..TEX2 and F32 frac 0 for TEX3..TEX7
    for (uint32_t a = kAttrTex0; a <= kAttrTex7; ++a) {
        if ((mask & (1u << a)) == 0) {
            continue;
        }
        float u = 0.0f;
        float v = 0.0f;
        if (vtxfmt1 && a <= kAttrTex2) {
            constexpr float kTexScale = 1.0f / 8192.0f;
            u = static_cast<float>(br.read_s16_be()) * kTexScale;
            v = static_cast<float>(br.read_s16_be()) * kTexScale;
        } else {
            u = br.read_f32_be();
            v = br.read_f32_be();
        }
        if (a == kAttrTex0) {
            out.uv0[0] = u;
            out.uv0[1] = v;
        } else if (a == kAttrTex0 + 1) {
            out.uv1[0] = u;
            out.uv1[1] = v;
        }
    }

    return br.ok();
}

void emit_triangle(std::vector<uint32_t>& indices,
                   uint32_t i0, uint32_t i1, uint32_t i2,
                   bool flip_winding) {
    if (i0 == i1 || i1 == i2 || i0 == i2) {
        return;
    }
    if (flip_winding) {
        indices.push_back(i0);
        indices.push_back(i2);
        indices.push_back(i1);
    } else {
        indices.push_back(i0);
        indices.push_back(i1);
        indices.push_back(i2);
    }
}

void compute_fallback_bounds(GmaModel& model) {
    float min_p[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float max_p[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    bool any = false;
    for (const auto& sm : model.submeshes) {
        for (const auto& v : sm.vertices) {
            for (int k = 0; k < 3; ++k) {
                if (std::isfinite(v.pos[k])) {
                    min_p[k] = std::min(min_p[k], v.pos[k]);
                    max_p[k] = std::max(max_p[k], v.pos[k]);
                    any = true;
                }
            }
        }
    }
    if (!any) {
        model.bound_center = {0.0f, 0.0f, 0.0f};
        model.bound_radius = 1.0f;
        return;
    }
    for (int k = 0; k < 3; ++k) {
        model.bound_center[k] = 0.5f * (min_p[k] + max_p[k]);
    }
    const float dx = max_p[0] - model.bound_center[0];
    const float dy = max_p[1] - model.bound_center[1];
    const float dz = max_p[2] - model.bound_center[2];
    model.bound_radius = std::max(0.1f, std::sqrt(dx * dx + dy * dy + dz * dz));
}

GxBlendMode derive_blend_mode(bool is_transparent, uint32_t mesh_flags, uint32_t blend_factors) {
    const uint32_t dst_factor = (blend_factors >> 4) & 0xFu;
    // GX_BL_ONE (2) destination with transparency => additive
    if (is_transparent && (mesh_flags & 0x40u) != 0 && dst_factor == 2u) {
        return GxBlendMode::Additive;
    }
    if (is_transparent) {
        return GxBlendMode::AlphaBlend;
    }
    if ((mesh_flags & 0x100u) != 0) {
        return GxBlendMode::Cutout;
    }
    return GxBlendMode::Opaque;
}

void populate_submesh_material(std::span<const uint8_t> mesh_hdr,
                               const std::vector<GmaMaterialLayer>& layers,
                               bool is_transparent,
                               GmaSubMesh& sm) {
    sm.mesh_flags = ByteReader::peek_be32(mesh_hdr, 0x00);
    sm.is_transparent = is_transparent;
    sm.double_sided = (sm.mesh_flags & 0x02u) != 0;
    sm.unlit = (sm.mesh_flags & 0x01u) != 0;
    sm.vtx_desc_mask = ByteReader::peek_be32(mesh_hdr, 0x1C);
    sm.has_vertex_color = (sm.vtx_desc_mask & (1u << kAttrClr0)) != 0;

    // Color logic from fn_80075080 (avdisp_mtx.c:498-534)
    const bool use_mat_rgb = (sm.mesh_flags & 0x88u) != 0;
    if (use_mat_rgb) {
        sm.material_rgba[0] = static_cast<float>(mesh_hdr[0x04]) / 255.0f;
        sm.material_rgba[1] = static_cast<float>(mesh_hdr[0x05]) / 255.0f;
        sm.material_rgba[2] = static_cast<float>(mesh_hdr[0x06]) / 255.0f;
    } else {
        sm.material_rgba[0] = 1.0f;
        sm.material_rgba[1] = 1.0f;
        sm.material_rgba[2] = 1.0f;
    }
    sm.alpha = static_cast<float>(mesh_hdr[0x11]) / 255.0f;
    if (sm.alpha <= 0.0f && !is_transparent) {
        sm.alpha = 1.0f;
    }
    sm.material_rgba[3] = sm.alpha;

    sm.ambient_rgba = {
        static_cast<float>(mesh_hdr[0x08]) / 255.0f,
        static_cast<float>(mesh_hdr[0x09]) / 255.0f,
        static_cast<float>(mesh_hdr[0x0A]) / 255.0f,
        static_cast<float>(mesh_hdr[0x0B]) / 255.0f,
    };
    sm.specular_rgba = {
        static_cast<float>(mesh_hdr[0x0C]) / 255.0f,
        static_cast<float>(mesh_hdr[0x0D]) / 255.0f,
        static_cast<float>(mesh_hdr[0x0E]) / 255.0f,
        static_cast<float>(mesh_hdr[0x0F]) / 255.0f,
    };

    const uint32_t blend_factors = ByteReader::peek_be32(mesh_hdr, 0x40);
    sm.blend_mode = derive_blend_mode(is_transparent, sm.mesh_flags, blend_factors);

    const uint8_t active_layers = mesh_hdr[0x12];
    for (uint8_t li = 0; li < active_layers && li < 3; ++li) {
        const uint16_t layer_idx = ByteReader::peek_be16(mesh_hdr, 0x16u + static_cast<size_t>(li) * 2u);
        if (layer_idx >= layers.size()) {
            continue;
        }
        const auto& l = layers[layer_idx];
        if ((l.flags & 0x00200000u) != 0) {
            continue; // Dynamic/runtime placeholder layer
        }
        if (sm.primary_tpl_index < 0 && !l.is_bump) {
            sm.primary_tpl_index = static_cast<int32_t>(l.tpl_index);
            sm.wrap_s = l.wrap_s;
            sm.wrap_t = l.wrap_t;
            sm.nearest_filter = l.nearest_filter;
        } else if (sm.secondary_tpl_index < 0) {
            sm.secondary_tpl_index = static_cast<int32_t>(l.tpl_index);
        }
    }
}

bool decode_stitched_stream(std::span<const uint8_t> vtx_pool,
                            std::span<const uint8_t> stream_bytes,
                            bool indices_u16,
                            uint32_t vtx_desc_mask,
                            std::vector<GxVertex>& out_vertices,
                            std::vector<uint32_t>& out_indices) {
    ByteReader br(stream_bytes);
    const size_t base_vertex = out_vertices.size();
    const bool has_clr = (vtx_desc_mask & (1u << kAttrClr0)) != 0;

    auto read_pool_vertex = [&](uint32_t byte_off, GxVertex& v) -> bool {
        if (static_cast<uint64_t>(byte_off) + 0x34ull > vtx_pool.size()) {
            return false;
        }
        v = GxVertex{};
        v.pos[0] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x00);
        v.pos[1] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x04);
        v.pos[2] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x08);
        v.normal[0] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x0C);
        v.normal[1] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x10);
        v.normal[2] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x14);
        v.uv0[0] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x18);
        v.uv0[1] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x1C);
        v.uv1[0] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x20);
        v.uv1[1] = ByteReader::peek_f32_be(vtx_pool, byte_off + 0x24);
        if (has_clr) {
            v.color[0] = static_cast<float>(vtx_pool[byte_off + 0x30]) / 255.0f;
            v.color[1] = static_cast<float>(vtx_pool[byte_off + 0x31]) / 255.0f;
            v.color[2] = static_cast<float>(vtx_pool[byte_off + 0x32]) / 255.0f;
            v.color[3] = static_cast<float>(vtx_pool[byte_off + 0x33]) / 255.0f;
        }
        return true;
    };

    while (br.remaining() >= (indices_u16 ? 2u : 4u)) {
        const uint32_t count = indices_u16 ? br.read_be16() : br.read_be32();
        if (!br.ok() || count == 0) {
            break;
        }
        if (count < 3) {
            br.skip(static_cast<size_t>(count) * (indices_u16 ? 2u : 4u));
            continue;
        }
        const uint32_t strip_start = static_cast<uint32_t>(out_vertices.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t off = indices_u16 ? br.read_be16() : br.read_be32();
            if (!br.ok()) {
                return out_vertices.size() > base_vertex;
            }
            GxVertex v{};
            if (!read_pool_vertex(off, v)) {
                return out_vertices.size() > base_vertex;
            }
            out_vertices.push_back(v);
        }
        for (uint32_t i = 2; i < count; ++i) {
            if ((i & 1u) == 0) {
                emit_triangle(out_indices, strip_start + i - 2, strip_start + i - 1, strip_start + i, false);
            } else {
                emit_triangle(out_indices, strip_start + i - 1, strip_start + i - 2, strip_start + i, false);
            }
        }
    }
    return true;
}

} // namespace

bool decode_gx_display_list(std::span<const uint8_t> dl,
                            const GxDisplayListOptions& opts,
                            std::vector<GxVertex>& out_vertices,
                            std::vector<uint32_t>& out_indices,
                            std::string* error_out) {
    ByteReader br(dl);
    while (!br.at_eof()) {
        const uint8_t cmd = br.read_u8();
        if (!br.ok()) break;

        if (cmd == 0x00u) {
            continue; // GX_NOP
        }

        const uint8_t opcode = cmd & 0xF8u;
        const bool is_vtxfmt1 = opts.use_vtxfmt1 || ((cmd & 0x07u) == 1u);

        if (opcode == 0x80u || opcode == 0x90u || opcode == 0x98u || opcode == 0xA0u ||
            opcode == 0xA8u || opcode == 0xB0u || opcode == 0xB8u) {
            const uint16_t vcount = br.read_be16();
            if (!br.ok()) {
                if (error_out) *error_out = "Truncated GX primitive vertex count.";
                return false;
            }
            if (vcount == 0) {
                continue;
            }
            const uint32_t start_idx = static_cast<uint32_t>(out_vertices.size());
            out_vertices.reserve(out_vertices.size() + vcount);
            for (uint16_t i = 0; i < vcount; ++i) {
                GxVertex v{};
                if (!read_gx_vertex(br, opts.vtx_desc_mask, is_vtxfmt1, v)) {
                    if (error_out) *error_out = "Truncated vertex stream in GX display list.";
                    return false;
                }
                out_vertices.push_back(v);
            }

            if (opcode == 0x90u) { // GX_TRIANGLES
                for (uint32_t i = 0; i + 2 < vcount; i += 3) {
                    emit_triangle(out_indices, start_idx + i, start_idx + i + 1, start_idx + i + 2, opts.flip_winding);
                }
            } else if (opcode == 0x98u) { // GX_TRIANGLESTRIP
                for (uint32_t i = 2; i < vcount; ++i) {
                    if ((i & 1u) == 0) {
                        emit_triangle(out_indices, start_idx + i - 2, start_idx + i - 1, start_idx + i, opts.flip_winding);
                    } else {
                        emit_triangle(out_indices, start_idx + i - 1, start_idx + i - 2, start_idx + i, opts.flip_winding);
                    }
                }
            } else if (opcode == 0xA0u) { // GX_TRIANGLEFAN
                for (uint32_t i = 2; i < vcount; ++i) {
                    emit_triangle(out_indices, start_idx, start_idx + i - 1, start_idx + i, opts.flip_winding);
                }
            } else if (opcode == 0x80u) { // GX_QUADS
                for (uint32_t i = 0; i + 3 < vcount; i += 4) {
                    emit_triangle(out_indices, start_idx + i, start_idx + i + 1, start_idx + i + 2, opts.flip_winding);
                    emit_triangle(out_indices, start_idx + i, start_idx + i + 2, start_idx + i + 3, opts.flip_winding);
                }
            }
            // Lines (0xA8/0xB0) and Points (0xB8) vertices are consumed above; skipped for triangle mesh output.
        } else if (cmd == 0x08u || cmd == 0x10u) {
            // CP / XF load inside display list
            if (cmd == 0x08u) {
                br.skip(5);
            } else {
                const uint16_t xfer_hdr = br.read_be16();
                br.read_be16(); // addr
                const uint32_t words = ((xfer_hdr >> 12) & 0xFu) + 1u;
                br.skip(words * 4u);
            }
        } else if (cmd == 0x61u) {
            br.skip(4); // GX_LOAD_BP_REG
        } else if (cmd == 0x20u || cmd == 0x28u || cmd == 0x30u || cmd == 0x38u) {
            br.skip(4); // Indexed matrix load
        } else {
            if (error_out) {
                *error_out = "Unknown GX display list opcode: 0x" + std::to_string(cmd);
            }
            return false;
        }

        if (!br.ok()) {
            if (error_out) *error_out = "Unexpected end of GX display list.";
            return false;
        }
    }
    return true;
}

void GmaArchive::clear() {
    models_.clear();
    loaded_ = false;
}

size_t GmaArchive::valid_model_count() const {
    return static_cast<size_t>(std::count_if(models_.begin(), models_.end(),
                                              [](const GmaModel& m) { return m.valid; }));
}

size_t GmaArchive::total_submesh_count() const {
    size_t n = 0;
    for (const auto& m : models_) {
        if (m.valid) n += m.submeshes.size();
    }
    return n;
}

size_t GmaArchive::total_triangle_count() const {
    size_t n = 0;
    for (const auto& m : models_) {
        if (!m.valid) continue;
        for (const auto& sm : m.submeshes) {
            n += sm.indices.size() / 3u;
        }
    }
    return n;
}

bool GmaArchive::load(std::span<const uint8_t> raw_bytes,
                      bool auto_decompress_lz,
                      std::string* error_out) {
    clear();
    if (raw_bytes.size() < 8) {
        if (error_out) *error_out = "Empty or truncated GMA buffer.";
        return false;
    }

    std::vector<uint8_t> decompressed;
    std::span<const uint8_t> data = raw_bytes;
    if (auto_decompress_lz && ByteReader::peek_be32(raw_bytes, 0) != kGcmfMagic && is_avlz_buffer(raw_bytes)) {
        if (!decompress_avlz(raw_bytes, decompressed, error_out)) {
            return false;
        }
        data = decompressed;
    }

    if (data.size() < 8) {
        if (error_out) *error_out = "GMA buffer too small after decompression.";
        return false;
    }

    // Standalone single GCMF model blob?
    if (ByteReader::peek_be32(data, 0) == kGcmfMagic) {
        models_.resize(1);
        if (!parse_gcmf(data, "SingleModel", models_[0], error_out)) {
            clear();
            return false;
        }
        loaded_ = true;
        return true;
    }

    const uint32_t model_count = ByteReader::peek_be32(data, 0x00);
    const uint32_t data_base = ByteReader::peek_be32(data, 0x04);
    if (model_count > 16384u ||
        8ull + static_cast<uint64_t>(model_count) * 8ull > data.size() ||
        data_base > data.size()) {
        if (error_out) *error_out = "Invalid GMA container header.";
        return false;
    }

    const size_t name_base = 8u + static_cast<size_t>(model_count) * 8u;
    models_.resize(model_count);

    for (uint32_t i = 0; i < model_count; ++i) {
        const size_t ent_off = 8u + static_cast<size_t>(i) * 8u;
        const uint32_t model_off = ByteReader::peek_be32(data, ent_off + 0);
        const uint32_t name_off = ByteReader::peek_be32(data, ent_off + 4);

        if (model_off == 0xFFFFFFFFu) {
            models_[i].valid = false;
            models_[i].name = "(empty)";
            continue;
        }

        const uint64_t abs_off = static_cast<uint64_t>(data_base) + model_off;
        if (abs_off >= data.size()) {
            models_[i].valid = false;
            models_[i].name = "(out-of-bounds)";
            continue;
        }

        std::string mname = ByteReader::read_cstr_at(data, name_base + name_off);
        if (mname.empty()) {
            mname = "Model_" + std::to_string(i);
        }

        std::string gcmf_err;
        if (!parse_gcmf(data.subspan(static_cast<size_t>(abs_off)), mname, models_[i], &gcmf_err)) {
            models_[i].valid = false;
            models_[i].name = mname + " [parse error: " + gcmf_err + "]";
        }
    }

    loaded_ = true;
    return true;
}

bool GmaArchive::parse_gcmf(std::span<const uint8_t> data,
                            const std::string& name,
                            GmaModel& out_model,
                            std::string* error_out) {
    out_model = GmaModel{};
    out_model.name = name;

    if (data.size() < 0x40) {
        if (error_out) *error_out = "GCMF header smaller than 0x40 bytes.";
        return false;
    }
    if (ByteReader::peek_be32(data, 0x00) != kGcmfMagic) {
        if (error_out) *error_out = "Missing GCMF magic.";
        return false;
    }

    out_model.flags = ByteReader::peek_be32(data, 0x04);
    out_model.bound_center[0] = ByteReader::peek_f32_be(data, 0x08);
    out_model.bound_center[1] = ByteReader::peek_f32_be(data, 0x0C);
    out_model.bound_center[2] = ByteReader::peek_f32_be(data, 0x10);
    out_model.bound_radius = ByteReader::peek_f32_be(data, 0x14);
    const uint16_t layer_count = ByteReader::peek_be16(data, 0x18);
    out_model.opaque_count = ByteReader::peek_be16(data, 0x1A);
    out_model.transparent_count = ByteReader::peek_be16(data, 0x1C);
    const uint32_t mesh_offset = ByteReader::peek_be32(data, 0x20);

    if (0x40ull + static_cast<uint64_t>(layer_count) * 0x20ull > data.size() ||
        mesh_offset >= data.size()) {
        if (error_out) *error_out = "GCMF layer or mesh offset out of bounds.";
        return false;
    }

    out_model.layers.resize(layer_count);
    for (uint16_t i = 0; i < layer_count; ++i) {
        const size_t loff = 0x40u + static_cast<size_t>(i) * 0x20u;
        GmaMaterialLayer& l = out_model.layers[i];
        l.flags = ByteReader::peek_be32(data, loff + 0x00);
        l.tpl_index = ByteReader::peek_be16(data, loff + 0x04);
        l.lod_bias_x10 = static_cast<int8_t>(data[loff + 0x06]);
        l.aniso = data[loff + 0x07];
        l.blend_flags = ByteReader::peek_be32(data, loff + 0x10);
        l.wrap_s = decode_wrap_mode((l.flags >> 2) & 3u);
        l.wrap_t = decode_wrap_mode((l.flags >> 4) & 3u);
        l.nearest_filter = (l.flags & 0x800u) != 0;
        l.is_env_map = (l.flags & 0x01u) != 0;
        l.is_bump = (l.flags & 0x02u) != 0;
        l.is_alpha_mask = (l.flags & 0x2000u) != 0;
    }

    const uint32_t total_meshes = static_cast<uint32_t>(out_model.opaque_count) + out_model.transparent_count;
    const bool is_stitched = (out_model.flags & 0x18u) != 0;

    if (is_stitched) {
        // Stitched/indexed model: AvDispData header at mesh_offset (0x20 bytes),
        // followed by contiguous 0x60-byte mesh headers, plus vertex pool at mesh_offset + vtx_pool_off.
        if (static_cast<uint64_t>(mesh_offset) + 0x20ull > data.size()) {
            if (error_out) *error_out = "Stitched GCMF AvDispData header out of bounds.";
            return false;
        }
        const auto av_hdr = data.subspan(mesh_offset, 0x20);
        const uint32_t vtx_pool_off = ByteReader::peek_be32(av_hdr, 0x0C);
        const uint32_t pool_flags = ByteReader::peek_be32(av_hdr, 0x1C);
        const bool indices_u16 = (pool_flags & 1u) != 0;

        const uint64_t vtx_pool_abs = static_cast<uint64_t>(mesh_offset) + vtx_pool_off;
        if (vtx_pool_abs > data.size()) {
            if (error_out) *error_out = "Stitched GCMF vertex pool out of bounds.";
            return false;
        }
        const auto vtx_pool = data.subspan(static_cast<size_t>(vtx_pool_abs));

        size_t mesh_hdr_cursor = static_cast<size_t>(mesh_offset) + 0x20u;
        // Estimate start of strip index streams right after all mesh headers;
        // fn_800786B0 passes arg1->offset as vertex pool, wait: in avdisp_draw.c:
        // result = (u8*)arg1 + arg1->offset (which is at AvDispData + offset), so check +0x0C vs +0x08!
        // In avdisp_mat.c:1785, v2 = arg2 + arg2->offset (at +0x08), and arg3 (passed in as arg1 + arg1->offset)
        // starts the strip stream right after mesh headers or at av_hdr +0x0C.
        const uint32_t off_08 = ByteReader::peek_be32(av_hdr, 0x08);
        const uint32_t off_0C = ByteReader::peek_be32(av_hdr, 0x0C);
        const size_t pool_abs = static_cast<size_t>(mesh_offset) + (off_08 < data.size() ? off_08 : off_0C);
        const size_t stream_abs = static_cast<size_t>(mesh_offset) + off_0C;
        const auto actual_pool = (pool_abs < data.size()) ? data.subspan(pool_abs) : vtx_pool;
        size_t stream_cursor = stream_abs;

        for (uint32_t m = 0; m < total_meshes; ++m) {
            if (mesh_hdr_cursor + 0x60u > data.size()) break;
            const auto mesh_hdr = data.subspan(mesh_hdr_cursor, 0x60u);
            mesh_hdr_cursor += 0x60u;

            GmaSubMesh sm{};
            const bool is_trans = (m >= out_model.opaque_count);
            populate_submesh_material(mesh_hdr, out_model.layers, is_trans, sm);

            const uint8_t dl_flags = mesh_hdr[0x13];
            for (int dli = 0; dli < 2; ++dli) {
                if ((dl_flags & (1u << dli)) == 0) continue;
                const uint32_t count_elems = ByteReader::peek_be32(mesh_hdr, 0x28u + static_cast<size_t>(dli) * 4u);
                const size_t stream_bytes = static_cast<size_t>(count_elems) * (indices_u16 ? 2u : 4u);
                if (stream_cursor + stream_bytes <= data.size()) {
                    decode_stitched_stream(actual_pool, data.subspan(stream_cursor, stream_bytes),
                                           indices_u16, sm.vtx_desc_mask, sm.vertices, sm.indices);
                    stream_cursor += stream_bytes;
                }
            }
            if (!sm.indices.empty()) {
                out_model.submeshes.push_back(std::move(sm));
            }
        }
    } else {
        // Standard display-list model
        size_t cursor = mesh_offset;
        for (uint32_t m = 0; m < total_meshes; ++m) {
            if (cursor + 0x60u > data.size()) {
                break;
            }
            const auto mesh_hdr = data.subspan(cursor, 0x60u);
            cursor += 0x60u;

            GmaSubMesh sm{};
            const bool is_trans = (m >= out_model.opaque_count);
            populate_submesh_material(mesh_hdr, out_model.layers, is_trans, sm);

            const uint8_t dl_flags = mesh_hdr[0x13];
            const uint32_t dl0_size = ByteReader::peek_be32(mesh_hdr, 0x28);
            const uint32_t dl1_size = ByteReader::peek_be32(mesh_hdr, 0x2C);

            GxDisplayListOptions dl_opts{};
            dl_opts.use_vtxfmt1 = (sm.mesh_flags & 0x00800000u) != 0; // overridden by opcode bottom 3 bits if cmd uses VAT1
            dl_opts.vtx_desc_mask = sm.vtx_desc_mask;

            if ((dl_flags & 0x01u) != 0) {
                if (cursor + dl0_size > data.size()) break;
                dl_opts.flip_winding = false;
                decode_gx_display_list(data.subspan(cursor, dl0_size), dl_opts, sm.vertices, sm.indices);
                cursor += dl0_size;
            }
            if ((dl_flags & 0x02u) != 0) {
                if (cursor + dl1_size > data.size()) break;
                dl_opts.flip_winding = true;
                decode_gx_display_list(data.subspan(cursor, dl1_size), dl_opts, sm.vertices, sm.indices);
                cursor += dl1_size;
            }
            if ((dl_flags & 0x0Cu) != 0) {
                if (cursor + 0x20u > data.size()) break;
                const auto ext_hdr = data.subspan(cursor, 0x20u);
                const uint32_t ext_dl0 = ByteReader::peek_be32(ext_hdr, 0x08);
                const uint32_t ext_dl1 = ByteReader::peek_be32(ext_hdr, 0x0C);
                cursor += 0x20u;
                if (ext_dl0 > 0 && cursor + ext_dl0 <= data.size()) {
                    dl_opts.flip_winding = false;
                    decode_gx_display_list(data.subspan(cursor, ext_dl0), dl_opts, sm.vertices, sm.indices);
                    cursor += ext_dl0;
                }
                if (ext_dl1 > 0 && cursor + ext_dl1 <= data.size()) {
                    dl_opts.flip_winding = true;
                    decode_gx_display_list(data.subspan(cursor, ext_dl1), dl_opts, sm.vertices, sm.indices);
                    cursor += ext_dl1;
                }
            }

            if (!sm.indices.empty()) {
                out_model.submeshes.push_back(std::move(sm));
            }
        }
    }

    if (!std::isfinite(out_model.bound_radius) || out_model.bound_radius <= 0.0f) {
        compute_fallback_bounds(out_model);
    }

    out_model.valid = true;
    return true;
}

} // namespace fzgx
