#pragma once

#include <volk.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fzgx {

struct LibrashaderParam {
    std::string name;
    std::string description;
    float initial{0.0f};
    float minimum{0.0f};
    float maximum{1.0f};
    float step{0.01f};
    float current{0.0f};
};

class LibrashaderBridge {
public:
    LibrashaderBridge() = default;
    ~LibrashaderBridge();

    LibrashaderBridge(const LibrashaderBridge&) = delete;
    LibrashaderBridge& operator=(const LibrashaderBridge&) = delete;

    bool try_load_library(const std::filesystem::path& explicit_path = {});
    bool is_library_loaded() const { return library_loaded_; }
    const std::string& loaded_library_path() const { return loaded_lib_path_; }
    const std::string& status_message() const { return status_message_; }

    bool load_preset(const std::filesystem::path& preset_path,
                     VkInstance instance,
                     VkPhysicalDevice physical_device,
                     VkDevice device,
                     VkQueue queue,
                     uint32_t frames_in_flight,
                     const std::map<std::string, float>& saved_params = {});

    void unload_preset();
    bool has_active_preset() const { return chain_ != nullptr; }
    const std::string& active_preset_path() const { return active_preset_path_; }

    std::vector<LibrashaderParam>& params() { return params_; }
    const std::vector<LibrashaderParam>& params() const { return params_; }
    bool set_param(const std::string& name, float value);

    bool render_frame(VkCommandBuffer cmd,
                      size_t frame_count,
                      VkImage src_image,
                      VkFormat src_format,
                      uint32_t src_width,
                      uint32_t src_height,
                      VkImage dst_image,
                      VkFormat dst_format,
                      uint32_t dst_width,
                      uint32_t dst_height,
                      float aspect_ratio,
                      float fps,
                      uint32_t frametime_delta_ms);

private:
    struct Impl;
    Impl* impl_{nullptr};
    void* chain_{nullptr};
    bool library_loaded_{false};
    std::string loaded_lib_path_;
    std::string active_preset_path_;
    std::string status_message_{"Not initialized"};
    std::vector<LibrashaderParam> params_;
};

} // namespace fzgx
