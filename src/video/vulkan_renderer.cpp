#include "video/vulkan_renderer.hpp"

#include "embedded_shaders.hpp"

#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace fzgx {

namespace {

struct ScenePushConstants {
    float time_sec;
    float aspect_ratio;
    float hud_edge_anchored;
    float rom_mounted;
    float stick_x;
    float stick_y;
    float trigger_l;
    float trigger_r;
    float res_x;
    float res_y;
};

struct PresentPushConstants {
    float crt_enabled;
    float scanline_strength;
    float mask_strength;
    float curvature;
    float vignette;
    float brightness;
    float src_width;
    float src_height;
    float dst_width;
    float dst_height;
};

struct FsrPushConstants {
    float src_width;
    float src_height;
    float dst_width;
    float dst_height;
    float sharpness;
    float pad0;
    float pad1;
    float pad2;
};

VkShaderModule create_shader_module(VkDevice device, std::span<const uint32_t> code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size_bytes();
    ci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& available,
                                     VsyncMode requested,
                                     std::string& out_label) {
    auto has = [&](VkPresentModeKHR m) {
        return std::find(available.begin(), available.end(), m) != available.end();
    };
    if (requested == VsyncMode::Off && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        out_label = "Immediate (VSync Off)";
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (requested == VsyncMode::Mailbox && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
        out_label = "Mailbox (Fast Sync)";
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    out_label = "FIFO (VSync On)";
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::headless_probe(std::string& out_summary) {
    if (volkInitialize() != VK_SUCCESS) {
        out_summary = "volkInitialize() failed (no Vulkan ICD loader found)";
        return false;
    }
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "fzero-gx-pc-smoke";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        out_summary = "vkCreateInstance failed";
        return false;
    }
    volkLoadInstance(inst);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    if (count > 0) {
        vkEnumeratePhysicalDevices(inst, &count, devs.data());
    }
    std::string first_gpu = "None";
    if (!devs.empty()) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devs[0], &props);
        first_gpu = props.deviceName;
    }
    vkDestroyInstance(inst, nullptr);
    out_summary = "Vulkan loader OK (" + std::to_string(count) + " device(s), primary: " + first_gpu + ")";
    return true;
}

bool VulkanRenderer::initialize(SDL_Window* window, const AppConfig& config, std::string* error_out) {
    window_ = window;
    last_frame_ticks_ns_ = SDL_GetTicksNS();

    if (!create_instance_and_device(window, error_out)) {
        return false;
    }
    if (!create_swapchain(config.vsync, error_out)) {
        return false;
    }
    update_offscreen_dimensions(config);
    if (!create_offscreen_target(offscreen_width_, offscreen_height_, error_out)) {
        return false;
    }
    if (!create_pipelines(error_out)) {
        return false;
    }
    if (!create_fsr_targets(swapchain_extent_.width, swapchain_extent_.height, error_out)) {
        return false;
    }
    // Rebind descriptors now that pipelines & targets both exist
    create_offscreen_target(offscreen_width_, offscreen_height_, nullptr);
    create_fsr_targets(swapchain_extent_.width, swapchain_extent_.height, nullptr);
    if (!gx_backend_.initialize(physical_device_, device_, graphics_family_, graphics_queue_,
                                command_pool_, offscreen_format_, offscreen_depth_format_,
                                supports_wireframe_, error_out)) {
        return false;
    }
    if (!init_imgui(window, error_out)) {
        return false;
    }

    librashader_.try_load_library(config.librashader_dll_path);
    reload_librashader(config);
    return true;
}

void VulkanRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    librashader_.unload_preset();
    shutdown_imgui();
    gx_backend_.shutdown();
    destroy_pipelines();
    destroy_fmv_resources();
    destroy_fsr_targets();
    destroy_offscreen_target();
    destroy_swapchain();

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (image_available_[i]) vkDestroySemaphore(device_, image_available_[i], nullptr);
        if (in_flight_[i]) vkDestroyFence(device_, in_flight_[i], nullptr);
        image_available_[i] = VK_NULL_HANDLE;
        in_flight_[i] = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::create_instance_and_device(SDL_Window* window, std::string* error_out) {
    if (volkInitialize() != VK_SUCCESS) {
        if (error_out) *error_out = "Failed to initialize Volk / Vulkan loader.";
        return false;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
        if (error_out) *error_out = std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError();
        return false;
    }

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "F-Zero GX PC";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "fzero-gx-pc";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = sdl_ext_count;
    ici.ppEnabledExtensionNames = sdl_exts;

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        if (error_out) *error_out = "vkCreateInstance failed (Vulkan 1.3 required).";
        return false;
    }
    volkLoadInstance(instance_);

    if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
        if (error_out) *error_out = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
        return false;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
    if (dev_count == 0) {
        if (error_out) *error_out = "No Vulkan physical devices found.";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());

    int best_score = -1;
    for (auto dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        uint32_t q_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(q_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &q_count, qprops.data());

        uint32_t chosen_q = UINT32_MAX;
        for (uint32_t q = 0; q < q_count; ++q) {
            VkBool32 present_ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, q, surface_, &present_ok);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_ok) {
                chosen_q = q;
                break;
            }
        }
        if (chosen_q == UINT32_MAX) {
            continue;
        }

        int score = 10;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 50;

        if (score > best_score) {
            best_score = score;
            physical_device_ = dev;
            graphics_family_ = chosen_q;
            diag_.gpu_name = props.deviceName;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        if (error_out) *error_out = "No Vulkan 1.3 GPU supporting graphics+present found.";
        return false;
    }

    VkPhysicalDeviceFeatures supported_feats{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supported_feats);
    supports_wireframe_ = (supported_feats.fillModeNonSolid == VK_TRUE);

    VkFormatProperties d32_props{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, VK_FORMAT_D32_SFLOAT, &d32_props);
    if ((d32_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        offscreen_depth_format_ = VK_FORMAT_D32_SFLOAT;
    } else {
        offscreen_depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    }

    VkPhysicalDeviceFeatures enabled_feats{};
    enabled_feats.fillModeNonSolid = supports_wireframe_ ? VK_TRUE : VK_FALSE;

    float q_prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphics_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &q_prio;

    VkPhysicalDeviceVulkan13Features feat13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    feat13.dynamicRendering = VK_TRUE;
    feat13.synchronization2 = VK_TRUE;

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feat13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled_feats;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
        if (error_out) *error_out = "vkCreateDevice failed.";
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = graphics_family_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &command_pool_) != VK_SUCCESS) {
        if (error_out) *error_out = "vkCreateCommandPool failed.";
        return false;
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    if (vkAllocateCommandBuffers(device_, &cbai, command_buffers_.data()) != VK_SUCCESS) {
        if (error_out) *error_out = "vkAllocateCommandBuffers failed.";
        return false;
    }

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &image_available_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fci, nullptr, &in_flight_[i]) != VK_SUCCESS) {
            if (error_out) *error_out = "Failed creating synchronization objects.";
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::create_swapchain(VsyncMode vsync, std::string* error_out) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    VkSurfaceFormatKHR chosen_fmt = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_fmt = f;
            break;
        }
    }

    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, modes.data());
    VkPresentModeKHR present_mode = choose_present_mode(modes, vsync, diag_.active_present_mode);

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    VkExtent2D extent{};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return true; // Minimized; defer rebuild
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_fmt.format;
    sci.imageColorSpace = chosen_fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        if (error_out) *error_out = "vkCreateSwapchainKHR failed.";
        return false;
    }

    swapchain_format_ = chosen_fmt.format;
    swapchain_extent_ = extent;
    active_vsync_ = vsync;
    diag_.swapchain_width = extent.width;
    diag_.swapchain_height = extent.height;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());

    swapchain_views_.resize(count);
    render_finished_.resize(count);
    VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = swapchain_images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapchain_format_;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &ivci, nullptr, &swapchain_views_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sem_ci, nullptr, &render_finished_[i]) != VK_SUCCESS) {
            if (error_out) *error_out = "Failed creating swapchain image views/semaphores.";
            return false;
        }
    }

    swapchain_dirty_ = false;
    return true;
}

void VulkanRenderer::destroy_swapchain() {
    for (auto s : render_finished_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(device_, s, nullptr);
    }
    render_finished_.clear();
    for (auto v : swapchain_views_) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device_, v, nullptr);
    }
    swapchain_views_.clear();
    swapchain_images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::update_offscreen_dimensions(const AppConfig& config) {
    const float aspect = resolve_aspect_ratio(
        config.aspect_ratio, swapchain_extent_.width, swapchain_extent_.height);

    const bool force_480p =
        (config.shader_backend == ShaderBackendMode::Librashader ||
         config.shader_backend == ShaderBackendMode::BuiltinCrt) &&
        config.librashader_feed_480p;

    const uint32_t base_h = force_480p ? 480u : static_cast<uint32_t>(480 * std::clamp(config.internal_resolution_scale, 1, 8));
    const uint32_t base_w = std::max(16u, static_cast<uint32_t>(std::lround(static_cast<float>(base_h) * aspect)));
    offscreen_width_ = base_w;
    offscreen_height_ = base_h;
    diag_.internal_width = offscreen_width_;
    diag_.internal_height = offscreen_height_;
}

bool VulkanRenderer::create_offscreen_target(uint32_t width, uint32_t height, std::string* error_out) {
    destroy_offscreen_target();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = offscreen_format_;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ici, nullptr, &offscreen_image_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating offscreen GX target image.";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, offscreen_image_, &req);
    const uint32_t mem_idx = find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        if (error_out) *error_out = "No suitable device-local memory for offscreen image.";
        return false;
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_idx;
    if (vkAllocateMemory(device_, &mai, nullptr, &offscreen_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, offscreen_image_, offscreen_memory_, 0) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed allocating/binding offscreen image memory.";
        return false;
    }

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = offscreen_image_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = offscreen_format_;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &ivci, nullptr, &offscreen_view_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating offscreen image view.";
        return false;
    }

    // Offscreen depth target for 3D GX rendering
    VkImageCreateInfo dici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dici.imageType = VK_IMAGE_TYPE_2D;
    dici.format = offscreen_depth_format_;
    dici.extent = {width, height, 1};
    dici.mipLevels = 1;
    dici.arrayLayers = 1;
    dici.samples = VK_SAMPLE_COUNT_1_BIT;
    dici.tiling = VK_IMAGE_TILING_OPTIMAL;
    dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &dici, nullptr, &offscreen_depth_image_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating offscreen depth image.";
        return false;
    }

    VkMemoryRequirements dreq{};
    vkGetImageMemoryRequirements(device_, offscreen_depth_image_, &dreq);
    const uint32_t dmem = find_memory_type(physical_device_, dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmem == UINT32_MAX) {
        if (error_out) *error_out = "No suitable device-local memory for offscreen depth image.";
        return false;
    }
    VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    dmai.allocationSize = dreq.size;
    dmai.memoryTypeIndex = dmem;
    if (vkAllocateMemory(device_, &dmai, nullptr, &offscreen_depth_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, offscreen_depth_image_, offscreen_depth_memory_, 0) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed allocating/binding offscreen depth image memory.";
        return false;
    }

    VkImageViewCreateInfo divci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    divci.image = offscreen_depth_image_;
    divci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    divci.format = offscreen_depth_format_;
    divci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &divci, nullptr, &offscreen_depth_view_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating offscreen depth image view.";
        return false;
    }

    if (present_desc_set_ != VK_NULL_HANDLE && linear_sampler_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo dii{};
        dii.sampler = linear_sampler_;
        dii.imageView = offscreen_view_;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wds.dstSet = present_desc_set_;
        wds.dstBinding = 0;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.pImageInfo = &dii;
        vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
    }
    if (fsr_easu_desc_set_ != VK_NULL_HANDLE && linear_sampler_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo dii{linear_sampler_, offscreen_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wds.dstSet = fsr_easu_desc_set_;
        wds.dstBinding = 0;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.pImageInfo = &dii;
        vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
    }

    return true;
}

bool VulkanRenderer::create_fsr_targets(uint32_t width, uint32_t height, std::string* error_out) {
    destroy_fsr_targets();
    if (width == 0 || height == 0) return true;

    auto make_target = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = offscreen_format_;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ici, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, img, &req);
        const uint32_t midx = find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (midx == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = midx;
        if (vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS ||
            vkBindImageMemory(device_, img, mem, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = img;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = offscreen_format_;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(device_, &ivci, nullptr, &view) == VK_SUCCESS;
    };

    if (!make_target(fsr_easu_image_, fsr_easu_memory_, fsr_easu_view_) ||
        !make_target(fsr_rcas_image_, fsr_rcas_memory_, fsr_rcas_view_)) {
        destroy_fsr_targets();
        if (error_out) *error_out = "Failed creating AMD FSR 1.0 intermediate targets.";
        return false;
    }

    fsr_width_ = width;
    fsr_height_ = height;

    if (linear_sampler_ != VK_NULL_HANDLE) {
        if (fsr_rcas_desc_set_ != VK_NULL_HANDLE) {
            VkDescriptorImageInfo dii{linear_sampler_, fsr_easu_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wds.dstSet = fsr_rcas_desc_set_;
            wds.dstBinding = 0;
            wds.descriptorCount = 1;
            wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds.pImageInfo = &dii;
            vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
        }
        if (present_desc_set_fsr_ != VK_NULL_HANDLE) {
            VkDescriptorImageInfo dii{linear_sampler_, fsr_rcas_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wds.dstSet = present_desc_set_fsr_;
            wds.dstBinding = 0;
            wds.descriptorCount = 1;
            wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds.pImageInfo = &dii;
            vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
        }
    }
    return true;
}

void VulkanRenderer::destroy_fsr_targets() {
    auto kill = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v != VK_NULL_HANDLE) { vkDestroyImageView(device_, v, nullptr); v = VK_NULL_HANDLE; }
        if (i != VK_NULL_HANDLE) { vkDestroyImage(device_, i, nullptr); i = VK_NULL_HANDLE; }
        if (m != VK_NULL_HANDLE) { vkFreeMemory(device_, m, nullptr); m = VK_NULL_HANDLE; }
    };
    kill(fsr_easu_view_, fsr_easu_image_, fsr_easu_memory_);
    kill(fsr_rcas_view_, fsr_rcas_image_, fsr_rcas_memory_);
    fsr_width_ = 0;
    fsr_height_ = 0;
}

void VulkanRenderer::destroy_offscreen_target() {
    if (offscreen_depth_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, offscreen_depth_view_, nullptr);
        offscreen_depth_view_ = VK_NULL_HANDLE;
    }
    if (offscreen_depth_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, offscreen_depth_image_, nullptr);
        offscreen_depth_image_ = VK_NULL_HANDLE;
    }
    if (offscreen_depth_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, offscreen_depth_memory_, nullptr);
        offscreen_depth_memory_ = VK_NULL_HANDLE;
    }
    if (offscreen_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, offscreen_view_, nullptr);
        offscreen_view_ = VK_NULL_HANDLE;
    }
    if (offscreen_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, offscreen_image_, nullptr);
        offscreen_image_ = VK_NULL_HANDLE;
    }
    if (offscreen_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, offscreen_memory_, nullptr);
        offscreen_memory_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::ensure_fmv_resources(uint32_t width, uint32_t height) {
    if (fmv_image_ != VK_NULL_HANDLE && fmv_width_ == width && fmv_height_ == height) {
        return true;
    }
    vkDeviceWaitIdle(device_);
    destroy_fmv_resources();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ici, nullptr, &fmv_image_) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, fmv_image_, &req);
    const uint32_t img_mem = find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mem == UINT32_MAX) {
        destroy_fmv_resources();
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = img_mem;
    if (vkAllocateMemory(device_, &mai, nullptr, &fmv_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, fmv_image_, fmv_memory_, 0) != VK_SUCCESS) {
        destroy_fmv_resources();
        return false;
    }

    const VkDeviceSize buf_bytes = static_cast<VkDeviceSize>(width) * height * 4;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = buf_bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bci, nullptr, &fmv_staging_buf_[i]) != VK_SUCCESS) {
            destroy_fmv_resources();
            return false;
        }
        VkMemoryRequirements breq{};
        vkGetBufferMemoryRequirements(device_, fmv_staging_buf_[i], &breq);
        const uint32_t bmem = find_memory_type(
            physical_device_, breq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bmem == UINT32_MAX) {
            destroy_fmv_resources();
            return false;
        }
        VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bmai.allocationSize = breq.size;
        bmai.memoryTypeIndex = bmem;
        if (vkAllocateMemory(device_, &bmai, nullptr, &fmv_staging_mem_[i]) != VK_SUCCESS ||
            vkBindBufferMemory(device_, fmv_staging_buf_[i], fmv_staging_mem_[i], 0) != VK_SUCCESS ||
            vkMapMemory(device_, fmv_staging_mem_[i], 0, buf_bytes, 0, &fmv_staging_mapped_[i]) != VK_SUCCESS) {
            destroy_fmv_resources();
            return false;
        }
    }

    fmv_width_ = width;
    fmv_height_ = height;
    fmv_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VulkanRenderer::destroy_fmv_resources() {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (fmv_staging_mapped_[i] && fmv_staging_mem_[i] != VK_NULL_HANDLE) {
            vkUnmapMemory(device_, fmv_staging_mem_[i]);
            fmv_staging_mapped_[i] = nullptr;
        }
        if (fmv_staging_buf_[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, fmv_staging_buf_[i], nullptr);
            fmv_staging_buf_[i] = VK_NULL_HANDLE;
        }
        if (fmv_staging_mem_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device_, fmv_staging_mem_[i], nullptr);
            fmv_staging_mem_[i] = VK_NULL_HANDLE;
        }
    }
    if (fmv_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, fmv_image_, nullptr);
        fmv_image_ = VK_NULL_HANDLE;
    }
    if (fmv_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, fmv_memory_, nullptr);
        fmv_memory_ = VK_NULL_HANDLE;
    }
    fmv_width_ = 0;
    fmv_height_ = 0;
    fmv_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::upload_fmv_frame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    pending_fmv_rgba_.resize(bytes);
    std::memcpy(pending_fmv_rgba_.data(), rgba, bytes);
    pending_fmv_width_ = width;
    pending_fmv_height_ = height;
    pending_fmv_dirty_ = true;
    has_fmv_frame_ = true;
}

void VulkanRenderer::clear_fmv_frame() {
    has_fmv_frame_ = false;
    pending_fmv_dirty_ = false;
    pending_fmv_rgba_.clear();
}

bool VulkanRenderer::create_pipelines(std::string* error_out) {
    // 1. Test scene pipeline (renders into offscreen_format_)
    VkPushConstantRange scene_pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants)};
    VkPipelineLayoutCreateInfo slci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    slci.pushConstantRangeCount = 1;
    slci.pPushConstantRanges = &scene_pcr;
    if (vkCreatePipelineLayout(device_, &slci, nullptr, &scene_pipe_layout_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating scene pipeline layout.";
        return false;
    }

    VkShaderModule scene_vs = create_shader_module(device_, shaders::test_scene_vert);
    VkShaderModule scene_fs = create_shader_module(device_, shaders::test_scene_frag);

    auto build_fullscreen_pipe = [&](VkShaderModule vs, VkShaderModule fs, VkFormat color_fmt,
                                     VkPipelineLayout layout, VkPipeline* out_pipe) -> bool {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;

        VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        prci.colorAttachmentCount = 1;
        prci.pColorAttachmentFormats = &color_fmt;

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.pNext = &prci;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &dyn;
        gpci.layout = layout;

        return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, out_pipe) == VK_SUCCESS;
    };

    const bool scene_ok = build_fullscreen_pipe(scene_vs, scene_fs, offscreen_format_, scene_pipe_layout_, &scene_pipeline_);
    vkDestroyShaderModule(device_, scene_vs, nullptr);
    vkDestroyShaderModule(device_, scene_fs, nullptr);
    if (!scene_ok) {
        if (error_out) *error_out = "Failed creating test scene graphics pipeline.";
        return false;
    }

    // 2. Present / Built-in CRT pipeline (renders into swapchain_format_)
    VkSamplerCreateInfo smci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smci.magFilter = VK_FILTER_LINEAR;
    smci.minFilter = VK_FILTER_LINEAR;
    smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &smci, nullptr, &linear_sampler_);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &present_desc_layout_);

    VkDescriptorPoolSize pool_sz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_sz;
    vkCreateDescriptorPool(device_, &dpci, nullptr, &present_desc_pool_);

    VkDescriptorSetLayout pres_layouts[2] = {present_desc_layout_, present_desc_layout_};
    VkDescriptorSet pres_sets[2]{};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = present_desc_pool_;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = pres_layouts;
    vkAllocateDescriptorSets(device_, &dsai, pres_sets);
    present_desc_set_ = pres_sets[0];
    present_desc_set_fsr_ = pres_sets[1];

    VkDescriptorImageInfo dii{};
    dii.sampler = linear_sampler_;
    dii.imageView = offscreen_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = present_desc_set_;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

    VkPushConstantRange present_pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPushConstants)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &present_desc_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &present_pcr;
    vkCreatePipelineLayout(device_, &plci, nullptr, &present_pipe_layout_);

    VkShaderModule pres_vs = create_shader_module(device_, shaders::present_crt_vert);
    VkShaderModule pres_fs = create_shader_module(device_, shaders::present_crt_frag);
    const bool pres_ok = build_fullscreen_pipe(pres_vs, pres_fs, swapchain_format_, present_pipe_layout_, &present_pipeline_);
    vkDestroyShaderModule(device_, pres_vs, nullptr);
    vkDestroyShaderModule(device_, pres_fs, nullptr);
    if (!pres_ok) {
        if (error_out) *error_out = "Failed creating present/CRT graphics pipeline.";
        return false;
    }

    // 3. AMD FSR 1.0 (EASU + RCAS) pipelines
    VkDescriptorPoolSize fsr_psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo fsr_dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    fsr_dpci.maxSets = 2;
    fsr_dpci.poolSizeCount = 1;
    fsr_dpci.pPoolSizes = &fsr_psz;
    vkCreateDescriptorPool(device_, &fsr_dpci, nullptr, &fsr_desc_pool_);

    VkDescriptorSetLayout fsr_layouts[2] = {present_desc_layout_, present_desc_layout_};
    VkDescriptorSet fsr_sets[2]{};
    VkDescriptorSetAllocateInfo fsr_dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    fsr_dsai.descriptorPool = fsr_desc_pool_;
    fsr_dsai.descriptorSetCount = 2;
    fsr_dsai.pSetLayouts = fsr_layouts;
    vkAllocateDescriptorSets(device_, &fsr_dsai, fsr_sets);
    fsr_easu_desc_set_ = fsr_sets[0];
    fsr_rcas_desc_set_ = fsr_sets[1];

    VkPushConstantRange fsr_pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FsrPushConstants)};
    VkPipelineLayoutCreateInfo fsr_plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fsr_plci.setLayoutCount = 1;
    fsr_plci.pSetLayouts = &present_desc_layout_;
    fsr_plci.pushConstantRangeCount = 1;
    fsr_plci.pPushConstantRanges = &fsr_pcr;
    vkCreatePipelineLayout(device_, &fsr_plci, nullptr, &fsr_pipe_layout_);

    VkShaderModule fsr_vs = create_shader_module(device_, shaders::present_crt_vert);
    VkShaderModule easu_fs = create_shader_module(device_, shaders::fsr_easu_frag);
    VkShaderModule rcas_fs = create_shader_module(device_, shaders::fsr_rcas_frag);
    const bool easu_ok = build_fullscreen_pipe(fsr_vs, easu_fs, offscreen_format_, fsr_pipe_layout_, &fsr_easu_pipeline_);
    const bool rcas_ok = build_fullscreen_pipe(fsr_vs, rcas_fs, offscreen_format_, fsr_pipe_layout_, &fsr_rcas_pipeline_);
    vkDestroyShaderModule(device_, fsr_vs, nullptr);
    vkDestroyShaderModule(device_, easu_fs, nullptr);
    vkDestroyShaderModule(device_, rcas_fs, nullptr);
    if (!easu_ok || !rcas_ok) {
        if (error_out) *error_out = "Failed creating AMD FSR 1.0 pipelines.";
        return false;
    }

    return true;
}

void VulkanRenderer::destroy_pipelines() {
    if (fsr_rcas_pipeline_) vkDestroyPipeline(device_, fsr_rcas_pipeline_, nullptr);
    if (fsr_easu_pipeline_) vkDestroyPipeline(device_, fsr_easu_pipeline_, nullptr);
    if (fsr_pipe_layout_) vkDestroyPipelineLayout(device_, fsr_pipe_layout_, nullptr);
    if (fsr_desc_pool_) vkDestroyDescriptorPool(device_, fsr_desc_pool_, nullptr);
    fsr_rcas_pipeline_ = VK_NULL_HANDLE;
    fsr_easu_pipeline_ = VK_NULL_HANDLE;
    fsr_pipe_layout_ = VK_NULL_HANDLE;
    fsr_desc_pool_ = VK_NULL_HANDLE;
    fsr_easu_desc_set_ = VK_NULL_HANDLE;
    fsr_rcas_desc_set_ = VK_NULL_HANDLE;

    if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
    if (present_pipe_layout_) vkDestroyPipelineLayout(device_, present_pipe_layout_, nullptr);
    if (present_desc_pool_) vkDestroyDescriptorPool(device_, present_desc_pool_, nullptr);
    if (present_desc_layout_) vkDestroyDescriptorSetLayout(device_, present_desc_layout_, nullptr);
    if (linear_sampler_) vkDestroySampler(device_, linear_sampler_, nullptr);
    if (scene_pipeline_) vkDestroyPipeline(device_, scene_pipeline_, nullptr);
    if (scene_pipe_layout_) vkDestroyPipelineLayout(device_, scene_pipe_layout_, nullptr);
    present_pipeline_ = VK_NULL_HANDLE;
    present_pipe_layout_ = VK_NULL_HANDLE;
    present_desc_pool_ = VK_NULL_HANDLE;
    present_desc_layout_ = VK_NULL_HANDLE;
    present_desc_set_ = VK_NULL_HANDLE;
    present_desc_set_fsr_ = VK_NULL_HANDLE;
    linear_sampler_ = VK_NULL_HANDLE;
    scene_pipeline_ = VK_NULL_HANDLE;
    scene_pipe_layout_ = VK_NULL_HANDLE;
}

bool VulkanRenderer::init_imgui(SDL_Window* window, std::string* error_out) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        if (error_out) *error_out = "ImGui_ImplSDL3_InitForVulkan failed.";
        return false;
    }

    ImGui_ImplVulkan_LoadFunctions(
        VK_API_VERSION_1_3,
        [](const char* fn, void* ud) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr(static_cast<VkInstance>(ud), fn);
        },
        instance_);

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = instance_;
    init_info.PhysicalDevice = physical_device_;
    init_info.Device = device_;
    init_info.QueueFamily = graphics_family_;
    init_info.Queue = graphics_queue_;
    init_info.DescriptorPoolSize = 32;
    init_info.MinImageCount = static_cast<uint32_t>(std::max<size_t>(2, swapchain_images_.size()));
    init_info.ImageCount = static_cast<uint32_t>(std::max<size_t>(2, swapchain_images_.size()));
    init_info.UseDynamicRendering = true;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchain_format_;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        if (error_out) *error_out = "ImGui_ImplVulkan_Init failed.";
        return false;
    }

    imgui_initialized_ = true;
    return true;
}

void VulkanRenderer::shutdown_imgui() {
    if (!imgui_initialized_) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    imgui_initialized_ = false;
}

void VulkanRenderer::on_window_resized() {
    swapchain_dirty_ = true;
}

void VulkanRenderer::apply_video_config(const AppConfig& config) {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    if (config.vsync != active_vsync_) {
        swapchain_dirty_ = true;
    }
    const uint32_t prev_w = offscreen_width_;
    const uint32_t prev_h = offscreen_height_;
    update_offscreen_dimensions(config);
    if (prev_w != offscreen_width_ || prev_h != offscreen_height_) {
        create_offscreen_target(offscreen_width_, offscreen_height_, nullptr);
    }
}

void VulkanRenderer::reload_librashader(const AppConfig& config) {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device_);
    if (!config.librashader_dll_path.empty() &&
        config.librashader_dll_path != librashader_.loaded_library_path()) {
        librashader_.try_load_library(config.librashader_dll_path);
    }
    if (config.shader_backend == ShaderBackendMode::Librashader &&
        !config.librashader_preset_path.empty() &&
        librashader_.is_library_loaded()) {
        librashader_.load_preset(
            config.librashader_preset_path,
            instance_,
            physical_device_,
            device_,
            graphics_queue_,
            kFramesInFlight,
            config.librashader_params);
    } else {
        librashader_.unload_preset();
    }
}

void VulkanRenderer::record_image_barrier(VkCommandBuffer cmd,
                                          VkImage image,
                                          VkImageLayout old_layout,
                                          VkImageLayout new_layout,
                                          VkPipelineStageFlags2 src_stage,
                                          VkAccessFlags2 src_access,
                                          VkPipelineStageFlags2 dst_stage,
                                          VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanRenderer::render_frame(const AppConfig& config,
                                  const PadState& pad1,
                                  bool rom_mounted,
                                  double elapsed_seconds,
                                  bool draw_overlay) {
    const uint64_t now_ns = SDL_GetTicksNS();
    const float dt_ms = static_cast<float>(now_ns - last_frame_ticks_ns_) / 1'000'000.0f;
    last_frame_ticks_ns_ = now_ns;
    if (dt_ms > 0.0f && dt_ms < 1000.0f) {
        frame_times_ms_.push_back(dt_ms);
        if (frame_times_ms_.size() > 120) {
            frame_times_ms_.pop_front();
        }
        const float sum = std::accumulate(frame_times_ms_.begin(), frame_times_ms_.end(), 0.0f);
        diag_.avg_frame_ms = sum / static_cast<float>(frame_times_ms_.size());
        diag_.current_fps = diag_.avg_frame_ms > 0.001f ? (1000.0f / diag_.avg_frame_ms) : 0.0f;
        diag_.frame_time_history.assign(frame_times_ms_.begin(), frame_times_ms_.end());
    }

    if (swapchain_dirty_ || swapchain_ == VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        if (!create_swapchain(config.vsync, nullptr) || swapchain_ == VK_NULL_HANDLE) {
            return;
        }
        update_offscreen_dimensions(config);
        create_offscreen_target(offscreen_width_, offscreen_height_, nullptr);
        create_fsr_targets(swapchain_extent_.width, swapchain_extent_.height, nullptr);
    }

    vkWaitForFences(device_, 1, &in_flight_[current_frame_], VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult acq = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX, image_available_[current_frame_], VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_dirty_ = true;
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        return;
    }

    vkResetFences(device_, 1, &in_flight_[current_frame_]);
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    // 1. Render either active Sofdec FMV frame OR synthetic GX scene into offscreen_image_
    float aspect = resolve_aspect_ratio(
        config.aspect_ratio, swapchain_extent_.width, swapchain_extent_.height);

    bool drew_fmv = false;
    if (has_fmv_frame_ && pending_fmv_width_ > 0 && pending_fmv_height_ > 0 &&
        ensure_fmv_resources(pending_fmv_width_, pending_fmv_height_)) {
        if (pending_fmv_dirty_ && fmv_staging_mapped_[current_frame_]) {
            const size_t copy_bytes = std::min(
                pending_fmv_rgba_.size(),
                static_cast<size_t>(fmv_width_) * fmv_height_ * 4);
            std::memcpy(fmv_staging_mapped_[current_frame_], pending_fmv_rgba_.data(), copy_bytes);

            record_image_barrier(cmd, fmv_image_,
                                 fmv_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {fmv_width_, fmv_height_, 1};
            vkCmdCopyBufferToImage(cmd, fmv_staging_buf_[current_frame_], fmv_image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            record_image_barrier(cmd, fmv_image_,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            fmv_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            pending_fmv_dirty_ = false;
        }

        if (fmv_layout_ == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            record_image_barrier(cmd, offscreen_image_,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<int32_t>(fmv_width_), static_cast<int32_t>(fmv_height_), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {static_cast<int32_t>(offscreen_width_), static_cast<int32_t>(offscreen_height_), 1};
            vkCmdBlitImage(cmd, fmv_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           offscreen_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            record_image_barrier(cmd, offscreen_image_,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

            // Preserve native FMV aspect ratio (pillarboxed 4:3 for standard GameCube .sfd)
            aspect = static_cast<float>(fmv_width_) / static_cast<float>(fmv_height_);
            drew_fmv = true;
        }
    }

    if (!drew_fmv) {
        if (gx_backend_.has_scene()) {
            gx_backend_.record_frame_to_offscreen(
                cmd,
                current_frame_,
                offscreen_image_,
                offscreen_view_,
                offscreen_depth_image_,
                offscreen_depth_view_,
                offscreen_width_,
                offscreen_height_,
                aspect,
                elapsed_seconds,
                pad1);
        } else {
            record_image_barrier(cmd, offscreen_image_,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo off_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            off_att.imageView = offscreen_view_;
            off_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            off_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            off_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            off_att.clearValue.color = {{0.02f, 0.02f, 0.04f, 1.0f}};

            VkRenderingInfo off_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            off_ri.renderArea = {{0, 0}, {offscreen_width_, offscreen_height_}};
            off_ri.layerCount = 1;
            off_ri.colorAttachmentCount = 1;
            off_ri.pColorAttachments = &off_att;

            vkCmdBeginRendering(cmd, &off_ri);
            VkViewport ovp{0.0f, 0.0f, static_cast<float>(offscreen_width_), static_cast<float>(offscreen_height_), 0.0f, 1.0f};
            VkRect2D osc{{0, 0}, {offscreen_width_, offscreen_height_}};
            vkCmdSetViewport(cmd, 0, 1, &ovp);
            vkCmdSetScissor(cmd, 0, 1, &osc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline_);
            ScenePushConstants spc{};
            spc.time_sec = static_cast<float>(elapsed_seconds);
            spc.aspect_ratio = aspect;
            spc.hud_edge_anchored = (config.hud_mode == HudLayoutMode::EdgeAnchored) ? 1.0f : 0.0f;
            spc.rom_mounted = rom_mounted ? 1.0f : 0.0f;
            spc.stick_x = pad1.norm_stick_x;
            spc.stick_y = pad1.norm_stick_y;
            spc.trigger_l = pad1.norm_trigger_l;
            spc.trigger_r = pad1.norm_trigger_r;
            spc.res_x = static_cast<float>(offscreen_width_);
            spc.res_y = static_cast<float>(offscreen_height_);
            vkCmdPushConstants(cmd, scene_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(spc), &spc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            record_image_barrier(cmd, offscreen_image_,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        }
    }

    // 1b. Optional AMD FSR 1.0 (EASU + RCAS) pass
    bool using_fsr = false;
    if (config.fsr_enabled && fsr_easu_pipeline_ != VK_NULL_HANDLE &&
        fsr_rcas_pipeline_ != VK_NULL_HANDLE && fsr_width_ > 0 && fsr_height_ > 0) {
        FsrPushConstants fpc{};
        fpc.src_width = static_cast<float>(offscreen_width_);
        fpc.src_height = static_cast<float>(offscreen_height_);
        fpc.dst_width = static_cast<float>(fsr_width_);
        fpc.dst_height = static_cast<float>(fsr_height_);
        fpc.sharpness = config.fsr_sharpness;

        VkViewport fvp{0.0f, 0.0f, fpc.dst_width, fpc.dst_height, 0.0f, 1.0f};
        VkRect2D fsc{{0, 0}, {fsr_width_, fsr_height_}};

        // EASU: offscreen_view_ -> fsr_easu_view_
        record_image_barrier(cmd, fsr_easu_image_,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo easu_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        easu_att.imageView = fsr_easu_view_;
        easu_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        easu_att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        easu_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo easu_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        easu_ri.renderArea = fsc;
        easu_ri.layerCount = 1;
        easu_ri.colorAttachmentCount = 1;
        easu_ri.pColorAttachments = &easu_att;
        vkCmdBeginRendering(cmd, &easu_ri);
        vkCmdSetViewport(cmd, 0, 1, &fvp);
        vkCmdSetScissor(cmd, 0, 1, &fsc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_easu_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_pipe_layout_,
                                0, 1, &fsr_easu_desc_set_, 0, nullptr);
        vkCmdPushConstants(cmd, fsr_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(fpc), &fpc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        record_image_barrier(cmd, fsr_easu_image_,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        // RCAS: fsr_easu_view_ -> fsr_rcas_view_
        record_image_barrier(cmd, fsr_rcas_image_,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo rcas_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        rcas_att.imageView = fsr_rcas_view_;
        rcas_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rcas_att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        rcas_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rcas_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rcas_ri.renderArea = fsc;
        rcas_ri.layerCount = 1;
        rcas_ri.colorAttachmentCount = 1;
        rcas_ri.pColorAttachments = &rcas_att;
        vkCmdBeginRendering(cmd, &rcas_ri);
        vkCmdSetViewport(cmd, 0, 1, &fvp);
        vkCmdSetScissor(cmd, 0, 1, &fsc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_rcas_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_pipe_layout_,
                                0, 1, &fsr_rcas_desc_set_, 0, nullptr);
        vkCmdPushConstants(cmd, fsr_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(fpc), &fpc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        record_image_barrier(cmd, fsr_rcas_image_,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        using_fsr = true;
    }
    diag_.using_fsr = using_fsr;

    const VkImage post_src_img = using_fsr ? fsr_rcas_image_ : offscreen_image_;
    const uint32_t post_src_w = using_fsr ? fsr_width_ : offscreen_width_;
    const uint32_t post_src_h = using_fsr ? fsr_height_ : offscreen_height_;
    const VkDescriptorSet post_desc_set = using_fsr ? present_desc_set_fsr_ : present_desc_set_;

    // 2. Prepare swapchain image & clear black margins
    VkImage swap_img = swapchain_images_[img_idx];
    VkImageView swap_view = swapchain_views_[img_idx];
    record_image_barrier(cmd, swap_img,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    bool used_librashader = false;
    if (config.shader_backend == ShaderBackendMode::Librashader && librashader_.has_active_preset()) {
        // Clear margins first before librashader renders into centered viewport
        VkRenderingAttachmentInfo clr_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        clr_att.imageView = swap_view;
        clr_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        clr_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        clr_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        clr_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo clr_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        clr_ri.renderArea = {{0, 0}, swapchain_extent_};
        clr_ri.layerCount = 1;
        clr_ri.colorAttachmentCount = 1;
        clr_ri.pColorAttachments = &clr_att;
        vkCmdBeginRendering(cmd, &clr_ri);
        vkCmdEndRendering(cmd);

        used_librashader = librashader_.render_frame(
            cmd,
            static_cast<size_t>(++total_frame_count_),
            post_src_img,
            offscreen_format_,
            post_src_w,
            post_src_h,
            swap_img,
            swapchain_format_,
            swapchain_extent_.width,
            swapchain_extent_.height,
            aspect,
            diag_.current_fps,
            static_cast<uint32_t>(std::max(1.0f, dt_ms)));
    }
    diag_.using_librashader = used_librashader;

    if (!used_librashader) {
        VkRenderingAttachmentInfo pres_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pres_att.imageView = swap_view;
        pres_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pres_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        pres_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        pres_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo pres_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        pres_ri.renderArea = {{0, 0}, swapchain_extent_};
        pres_ri.layerCount = 1;
        pres_ri.colorAttachmentCount = 1;
        pres_ri.pColorAttachments = &pres_att;
        vkCmdBeginRendering(cmd, &pres_ri);

        const float sw = static_cast<float>(swapchain_extent_.width);
        const float sh = static_cast<float>(swapchain_extent_.height);
        const float dst_aspect = sh > 0.0f ? (sw / sh) : 1.0f;
        VkViewport pvp{0.0f, 0.0f, sw, sh, 0.0f, 1.0f};
        if (dst_aspect > aspect) {
            pvp.width = sh * aspect;
            pvp.x = (sw - pvp.width) * 0.5f;
        } else {
            pvp.height = sw / std::max(aspect, 0.1f);
            pvp.y = (sh - pvp.height) * 0.5f;
        }
        VkRect2D psc{
            {static_cast<int32_t>(std::max(0.0f, pvp.x)), static_cast<int32_t>(std::max(0.0f, pvp.y))},
            {static_cast<uint32_t>(std::max(1.0f, pvp.width)), static_cast<uint32_t>(std::max(1.0f, pvp.height))}};
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        vkCmdSetScissor(cmd, 0, 1, &psc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipe_layout_,
                                0, 1, &post_desc_set, 0, nullptr);

        PresentPushConstants ppc{};
        ppc.crt_enabled = (config.shader_backend == ShaderBackendMode::BuiltinCrt) ? 1.0f : 0.0f;
        ppc.scanline_strength = config.builtin_crt.scanline_strength;
        ppc.mask_strength = config.builtin_crt.mask_strength;
        ppc.curvature = config.builtin_crt.curvature;
        ppc.vignette = config.builtin_crt.vignette;
        ppc.brightness = config.builtin_crt.brightness;
        ppc.src_width = static_cast<float>(offscreen_width_);
        ppc.src_height = static_cast<float>(offscreen_height_);
        ppc.dst_width = pvp.width;
        ppc.dst_height = pvp.height;
        vkCmdPushConstants(cmd, present_pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ppc), &ppc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    // 3. Dear ImGui overlay pass on top of swapchain image
    if (draw_overlay) {
        if (ImDrawData* draw_data = ImGui::GetDrawData()) {
            VkRenderingAttachmentInfo ui_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ui_att.imageView = swap_view;
            ui_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ui_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ui_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ui_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ui_ri.renderArea = {{0, 0}, swapchain_extent_};
            ui_ri.layerCount = 1;
            ui_ri.colorAttachmentCount = 1;
            ui_ri.pColorAttachments = &ui_att;
            vkCmdBeginRendering(cmd, &ui_ri);
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
            vkCmdEndRendering(cmd);
        }
    }

    record_image_barrier(cmd, swap_img,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_available_[current_frame_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_finished_[img_idx];
    vkQueueSubmit(graphics_queue_, 1, &si, in_flight_[current_frame_]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[img_idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &img_idx;
    VkResult pres = vkQueuePresentKHR(graphics_queue_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true;
    }

    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

} // namespace fzgx
