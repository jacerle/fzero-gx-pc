#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fzgx {

struct GxVertex {
    float pos[3]{0.0f, 0.0f, 0.0f};
    float normal[3]{0.0f, 1.0f, 0.0f};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float uv0[2]{0.0f, 0.0f};
    float uv1[2]{0.0f, 0.0f};
};

enum class GxCullMode : uint8_t {
    None = 0,
    Back = 1,
    Front = 2,
};

enum class GxBlendMode : uint8_t {
    Opaque = 0,
    AlphaBlend = 1,
    Additive = 2,
    Cutout = 3,
};

enum class GxWrapMode : uint8_t {
    Clamp = 0,
    Repeat = 1,
    Mirror = 2,
};

struct GmaMaterialLayer {
    uint32_t flags{0};
    uint16_t tpl_index{0};
    int8_t lod_bias_x10{0};
    uint8_t aniso{0};
    uint32_t blend_flags{0};
    GxWrapMode wrap_s{GxWrapMode::Clamp};
    GxWrapMode wrap_t{GxWrapMode::Clamp};
    bool nearest_filter{false};
    bool is_env_map{false};
    bool is_bump{false};
    bool is_alpha_mask{false};
};

struct GmaSubMesh {
    std::vector<GxVertex> vertices;
    std::vector<uint32_t> indices;

    std::array<float, 4> material_rgba{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> ambient_rgba{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> specular_rgba{0.0f, 0.0f, 0.0f, 1.0f};
    float alpha{1.0f};

    uint32_t mesh_flags{0};
    uint32_t vtx_desc_mask{0};
    bool is_transparent{false};
    bool double_sided{false};
    bool unlit{false};
    bool has_vertex_color{false};

    int32_t primary_tpl_index{-1};
    int32_t secondary_tpl_index{-1};
    GxWrapMode wrap_s{GxWrapMode::Repeat};
    GxWrapMode wrap_t{GxWrapMode::Repeat};
    bool nearest_filter{false};
    GxBlendMode blend_mode{GxBlendMode::Opaque};
};

struct GmaModel {
    std::string name;
    bool valid{false};
    uint32_t flags{0};
    std::array<float, 3> bound_center{0.0f, 0.0f, 0.0f};
    float bound_radius{1.0f};
    uint16_t opaque_count{0};
    uint16_t transparent_count{0};
    std::vector<GmaMaterialLayer> layers;
    std::vector<GmaSubMesh> submeshes;
};

struct GxDisplayListOptions {
    bool use_vtxfmt1{false};
    bool flip_winding{false};
    uint32_t vtx_desc_mask{0};
};

// Decodes a GameCube GX display list into indexed triangles (`out_vertices`, `out_indices`).
bool decode_gx_display_list(std::span<const uint8_t> dl,
                            const GxDisplayListOptions& opts,
                            std::vector<GxVertex>& out_vertices,
                            std::vector<uint32_t>& out_indices,
                            std::string* error_out = nullptr);

class GmaArchive {
public:
    static constexpr uint32_t kGcmfMagic = 0x47434D46u; // "GCMF"

    bool load(std::span<const uint8_t> raw_bytes,
              bool auto_decompress_lz = true,
              std::string* error_out = nullptr);
    void clear();

    bool is_loaded() const { return loaded_; }
    const std::vector<GmaModel>& models() const { return models_; }
    size_t valid_model_count() const;
    size_t total_submesh_count() const;
    size_t total_triangle_count() const;

private:
    bool parse_gcmf(std::span<const uint8_t> data,
                    const std::string& name,
                    GmaModel& out_model,
                    std::string* error_out);

    std::vector<GmaModel> models_;
    bool loaded_{false};
};

} // namespace fzgx
