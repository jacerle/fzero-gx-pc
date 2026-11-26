#pragma once

#include "assets/gma_model.hpp"
#include "assets/gx_texture.hpp"
#include "input/input_manager.hpp"
#include "video/gx_state.hpp"

#include <volk.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzgx {

enum class GxShadingMode : int {
    TevShaded = 0,
    UnlitTextured = 1,
    Normals = 2,
    FlatColor = 3,
};

struct GxViewerModelSummary {
    std::string name;
    bool valid{false};
    uint32_t submesh_count{0};
    uint32_t triangle_count{0};
    float bound_radius{1.0f};
};

struct GxViewerTexSummary {
    uint32_t index{0};
    bool valid{false};
    uint16_t width{0};
    uint16_t height{0};
    std::string format_name;
};

struct GxViewerState {
    int selected_model_index{-1}; // -1 = All models
    bool auto_rotate{true};
    float yaw_deg{35.0f};
    float pitch_deg{18.0f};
    float distance{8.0f};
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    float fov_y_deg{50.0f};

    bool lighting_enabled{true};
    bool textures_enabled{true};
    bool fog_enabled{true};
    bool transparent_pass_enabled{true};
    bool backface_culling_enabled{true};
    bool wireframe{false};
    GxShadingMode shading_mode{GxShadingMode::TevShaded};
    std::array<float, 3> background_color{0.03f, 0.04f, 0.08f};
    int preview_tpl_index{-1};
};

class GxVulkanBackend {
public:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kMaxDrawsPerFrame = 2048;

    GxVulkanBackend() = default;
    ~GxVulkanBackend();

    GxVulkanBackend(const GxVulkanBackend&) = delete;
    GxVulkanBackend& operator=(const GxVulkanBackend&) = delete;

    bool initialize(VkPhysicalDevice phys,
                    VkDevice device,
                    uint32_t graphics_family,
                    VkQueue graphics_queue,
                    VkCommandPool cmd_pool,
                    VkFormat color_format,
                    VkFormat depth_format,
                    bool supports_wireframe,
                    std::string* error_out = nullptr);
    void shutdown();

    bool upload_gma_scene(const GmaArchive& gma,
                          const TplArchive* tpl,
                          const std::string& scene_label,
                          std::string* error_out = nullptr);
    bool upload_tpl_preview(const TplArchive& tpl,
                            int tex_index,
                            const std::string& label,
                            std::string* error_out = nullptr);
    void enable_live_demo_scene();
    void clear_scene();

    bool has_scene() const { return has_scene_ || live_demo_active_ || external_live_active_; }
    bool is_tpl_preview() const { return is_tpl_preview_; }
    bool is_live_demo() const { return live_demo_active_; }
    void set_external_live_active(bool active) { external_live_active_ = active; }
    bool supports_wireframe() const { return supports_wireframe_; }
    const std::string& scene_label() const { return scene_label_; }
    size_t total_submeshes() const { return total_submeshes_; }
    size_t total_triangles() const { return total_triangles_; }
    uint32_t last_efb_copies() const { return last_efb_copies_; }
    uint32_t last_max_tev_stages() const { return last_max_tev_stages_; }
    const std::vector<GxViewerModelSummary>& model_summaries() const { return model_summaries_; }
    const std::vector<GxViewerTexSummary>& texture_summaries() const { return texture_summaries_; }

    GxViewerState& viewer_state() { return viewer_; }
    const GxViewerState& viewer_state() const { return viewer_; }

    void auto_frame_camera();
    void update_camera(double dt_s,
                       const PadState& pad1,
                       float orbit_dx,
                       float orbit_dy,
                       float pan_dx,
                       float pan_dy,
                       float wheel_delta,
                       float move_forward,
                       float move_right,
                       float move_up);

    // Records the complete 3D GX pass (including dynamic rendering & any mid-frame EFB copies)
    // into `offscreen_image`, leaving `offscreen_image` in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    void record_frame_to_offscreen(VkCommandBuffer cmd,
                                   uint32_t frame_index,
                                   VkImage offscreen_image,
                                   VkImageView offscreen_view,
                                   VkImage offscreen_depth_image,
                                   VkImageView offscreen_depth_view,
                                   uint32_t width,
                                   uint32_t height,
                                   float aspect_ratio,
                                   double elapsed_seconds,
                                   const PadState& pad1);

private:
    struct GpuBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        void* mapped{nullptr};
    };

    struct GpuTexture {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool valid{false};
    };

    struct GpuSubMesh {
        GpuBuffer vbo;
        GpuBuffer ibo;
        uint32_t index_count{0};
        std::array<float, 4> material_rgba{1.0f, 1.0f, 1.0f, 1.0f};
        GxTevConfig tev{};
        bool is_transparent{false};
        bool double_sided{false};
        bool unlit{false};
        bool has_vertex_color{false};
        bool env_map_uv1{false};
        GxBlendMode blend_mode{GxBlendMode::Opaque};
        VkDescriptorSet mat_desc_set{VK_NULL_HANDLE};
    };

    struct GpuModel {
        std::string name;
        bool valid{false};
        std::array<float, 3> bound_center{0.0f, 0.0f, 0.0f};
        float bound_radius{1.0f};
        std::vector<GpuSubMesh> submeshes;
    };

    bool create_buffer(VkDeviceSize size,
                       VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props,
                       GpuBuffer& out_buf,
                       const void* initial_data = nullptr,
                       bool keep_mapped = false);
    void destroy_buffer(GpuBuffer& buf);

    bool upload_rgba_texture(uint32_t width,
                             uint32_t height,
                             const uint8_t* rgba,
                             GpuTexture& out_tex);
    bool ensure_efb_texture(uint32_t efb_id, uint32_t width, uint32_t height);
    void destroy_texture(GpuTexture& tex);

    VkSampler get_or_create_sampler(GxWrapMode wrap_s, GxWrapMode wrap_t, bool nearest);
    VkPipeline get_or_create_pipeline(GxCullMode cull,
                                      GxBlendMode blend,
                                      bool depth_test,
                                      bool depth_write,
                                      GxCompare depth_func,
                                      bool wireframe);
    VkDescriptorSet allocate_material_descriptor(const std::array<VkImageView, 4>& views,
                                                 const std::array<VkSampler, 4>& samplers);

    void populate_live_demo_frame(uint32_t width,
                                  uint32_t height,
                                  float aspect,
                                  double elapsed_seconds,
                                  const PadState& pad1);
    void record_live_frame_commands(VkCommandBuffer cmd,
                                    uint32_t fi,
                                    VkImage offscreen_image,
                                    VkImageView offscreen_view,
                                    VkImage offscreen_depth_image,
                                    VkImageView offscreen_depth_view,
                                    uint32_t width,
                                    uint32_t height,
                                    const GxFrameCommands& frame);
    void record_gma_frame_commands(VkCommandBuffer cmd,
                                   uint32_t fi,
                                   VkImage offscreen_image,
                                   VkImageView offscreen_view,
                                   VkImage offscreen_depth_image,
                                   VkImageView offscreen_depth_view,
                                   uint32_t width,
                                   uint32_t height,
                                   float aspect_ratio);

    void destroy_scene_resources();
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const;
    bool execute_oneshot(const std::function<void(VkCommandBuffer)>& fn);

    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    uint32_t graphics_family_{0};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    VkFormat color_format_{VK_FORMAT_R8G8B8A8_UNORM};
    VkFormat depth_format_{VK_FORMAT_D32_SFLOAT};
    bool supports_wireframe_{false};
    VkDeviceSize min_ubo_align_{256};

    VkShaderModule vert_shader_{VK_NULL_HANDLE};
    VkShaderModule frag_shader_{VK_NULL_HANDLE};

    VkDescriptorSetLayout ubo_desc_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout mat_desc_layout_{VK_NULL_HANDLE};
    VkDescriptorPool ubo_desc_pool_{VK_NULL_HANDLE};
    VkDescriptorPool mat_desc_pool_{VK_NULL_HANDLE};
    VkDescriptorPool live_mat_desc_pool_{VK_NULL_HANDLE};

    std::array<VkDescriptorSet, kFramesInFlight> ubo_desc_sets_{};
    std::array<GpuBuffer, kFramesInFlight> cam_ubos_{};
    std::array<GpuBuffer, kFramesInFlight> tev_ubos_{};
    VkDeviceSize cam_stride_{256};
    VkDeviceSize tev_stride_{768};

    // Per-frame dynamic vertex/index streaming buffers for live GX draws
    std::array<GpuBuffer, kFramesInFlight> dyn_vbos_{};
    std::array<GpuBuffer, kFramesInFlight> dyn_ibos_{};

    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    std::unordered_map<uint32_t, VkPipeline> pipeline_cache_;
    std::unordered_map<uint32_t, VkSampler> sampler_cache_;

    GpuTexture white_fallback_;
    GpuTexture checker_fallback_;
    std::vector<GpuTexture> scene_textures_;
    std::unordered_map<uint32_t, GpuTexture> efb_textures_;
    std::unordered_map<uint64_t, VkDescriptorSet> live_mat_cache_;

    std::vector<GpuModel> scene_models_;
    std::vector<GxViewerModelSummary> model_summaries_;
    std::vector<GxViewerTexSummary> texture_summaries_;

    std::string scene_label_;
    size_t total_submeshes_{0};
    size_t total_triangles_{0};
    uint32_t last_efb_copies_{0};
    uint32_t last_max_tev_stages_{1};
    bool has_scene_{false};
    bool is_tpl_preview_{false};
    bool live_demo_active_{false};
    bool external_live_active_{false};
    GxViewerState viewer_{};
};

} // namespace fzgx
