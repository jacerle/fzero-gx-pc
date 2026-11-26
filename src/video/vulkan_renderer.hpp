#pragma once

#include "core/config.hpp"
#include "input/input_manager.hpp"
#include "video/gx_vulkan.hpp"
#include "video/librashader_bridge.hpp"

#include <SDL3/SDL.h>
#include <volk.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fzgx {

struct RendererDiagnostics {
    std::string gpu_name{"Unavailable"};
    std::string vulkan_api_version{"1.3"};
    uint32_t internal_width{1280};
    uint32_t internal_height{960};
    uint32_t swapchain_width{1280};
    uint32_t swapchain_height{720};
    float current_fps{0.0f};
    float avg_frame_ms{0.0f};
    std::vector<float> frame_time_history;
    bool using_librashader{false};
    bool using_fsr{false};
    std::string active_present_mode{"FIFO"};
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool initialize(SDL_Window* window, const AppConfig& config, std::string* error_out = nullptr);
    void shutdown();

    void on_window_resized();
    void apply_video_config(const AppConfig& config);
    void reload_librashader(const AppConfig& config);

    // Renders the synthetic 480p/HD F-Zero calibration scene (or active Sofdec FMV frame),
    // applies post-FX (librashader or built-in CRT), renders Dear ImGui on top, and presents.
    void render_frame(const AppConfig& config,
                      const PadState& pad1,
                      bool rom_mounted,
                      double elapsed_seconds,
                      bool draw_overlay);

    void upload_fmv_frame(const uint8_t* rgba, uint32_t width, uint32_t height);
    void clear_fmv_frame();

    const RendererDiagnostics& diagnostics() const { return diag_; }
    LibrashaderBridge& librashader() { return librashader_; }
    const LibrashaderBridge& librashader() const { return librashader_; }
    GxVulkanBackend& gx_backend() { return gx_backend_; }
    const GxVulkanBackend& gx_backend() const { return gx_backend_; }

    static bool headless_probe(std::string& out_summary);

private:
    static constexpr uint32_t kFramesInFlight = 2;

    bool create_instance_and_device(SDL_Window* window, std::string* error_out);
    bool create_swapchain(VsyncMode vsync, std::string* error_out);
    void destroy_swapchain();
    bool create_offscreen_target(uint32_t width, uint32_t height, std::string* error_out);
    void destroy_offscreen_target();
    bool create_fsr_targets(uint32_t width, uint32_t height, std::string* error_out);
    void destroy_fsr_targets();
    bool ensure_fmv_resources(uint32_t width, uint32_t height);
    void destroy_fmv_resources();
    bool create_pipelines(std::string* error_out);
    void destroy_pipelines();
    bool init_imgui(SDL_Window* window, std::string* error_out);
    void shutdown_imgui();

    void update_offscreen_dimensions(const AppConfig& config);
    void record_image_barrier(VkCommandBuffer cmd,
                              VkImage image,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2 src_access,
                              VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access);

    SDL_Window* window_{nullptr};
    VkInstance instance_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    uint32_t graphics_family_{0};
    VkQueue graphics_queue_{VK_NULL_HANDLE};

    VkCommandPool command_pool_{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
    std::array<VkSemaphore, kFramesInFlight> image_available_{};
    std::array<VkFence, kFramesInFlight> in_flight_{};
    uint32_t current_frame_{0};
    uint64_t total_frame_count_{0};

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat swapchain_format_{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D swapchain_extent_{1280, 720};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkSemaphore> render_finished_;
    bool swapchain_dirty_{false};
    VsyncMode active_vsync_{VsyncMode::Fifo};

    // Offscreen GX color & depth targets
    VkImage offscreen_image_{VK_NULL_HANDLE};
    VkDeviceMemory offscreen_memory_{VK_NULL_HANDLE};
    VkImageView offscreen_view_{VK_NULL_HANDLE};
    VkFormat offscreen_format_{VK_FORMAT_R8G8B8A8_UNORM};
    VkImage offscreen_depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory offscreen_depth_memory_{VK_NULL_HANDLE};
    VkImageView offscreen_depth_view_{VK_NULL_HANDLE};
    VkFormat offscreen_depth_format_{VK_FORMAT_D32_SFLOAT};
    bool supports_wireframe_{false};
    uint32_t offscreen_width_{1280};
    uint32_t offscreen_height_{960};

    // Dynamic FMV frame texture + per-frame staging buffers
    VkImage fmv_image_{VK_NULL_HANDLE};
    VkDeviceMemory fmv_memory_{VK_NULL_HANDLE};
    uint32_t fmv_width_{0};
    uint32_t fmv_height_{0};
    VkImageLayout fmv_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkBuffer, kFramesInFlight> fmv_staging_buf_{};
    std::array<VkDeviceMemory, kFramesInFlight> fmv_staging_mem_{};
    std::array<void*, kFramesInFlight> fmv_staging_mapped_{};
    std::vector<uint8_t> pending_fmv_rgba_;
    uint32_t pending_fmv_width_{0};
    uint32_t pending_fmv_height_{0};
    bool pending_fmv_dirty_{false};
    bool has_fmv_frame_{false};

    // Pipelines & descriptors
    VkPipelineLayout scene_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline scene_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout present_desc_layout_{VK_NULL_HANDLE};
    VkDescriptorPool present_desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet present_desc_set_{VK_NULL_HANDLE};
    VkDescriptorSet present_desc_set_fsr_{VK_NULL_HANDLE};
    VkSampler linear_sampler_{VK_NULL_HANDLE};
    VkPipelineLayout present_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline present_pipeline_{VK_NULL_HANDLE};

    // AMD FSR 1.0 (EASU + RCAS) resources
    VkImage fsr_easu_image_{VK_NULL_HANDLE};
    VkDeviceMemory fsr_easu_memory_{VK_NULL_HANDLE};
    VkImageView fsr_easu_view_{VK_NULL_HANDLE};
    VkImage fsr_rcas_image_{VK_NULL_HANDLE};
    VkDeviceMemory fsr_rcas_memory_{VK_NULL_HANDLE};
    VkImageView fsr_rcas_view_{VK_NULL_HANDLE};
    uint32_t fsr_width_{0};
    uint32_t fsr_height_{0};
    VkDescriptorPool fsr_desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet fsr_easu_desc_set_{VK_NULL_HANDLE};
    VkDescriptorSet fsr_rcas_desc_set_{VK_NULL_HANDLE};
    VkPipelineLayout fsr_pipe_layout_{VK_NULL_HANDLE};
    VkPipeline fsr_easu_pipeline_{VK_NULL_HANDLE};
    VkPipeline fsr_rcas_pipeline_{VK_NULL_HANDLE};

    bool imgui_initialized_{false};
    GxVulkanBackend gx_backend_;
    LibrashaderBridge librashader_;
    RendererDiagnostics diag_{};
    std::deque<float> frame_times_ms_;
    uint64_t last_frame_ticks_ns_{0};
};

} // namespace fzgx
