#define VK_NO_PROTOTYPES
#include "video/librashader_bridge.hpp"

#define LIBRA_RUNTIME_VULKAN
#include "librashader_ld.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fzgx {

namespace {

std::string consume_libra_error(const libra_instance_t& api, libra_error_t err) {
    if (!err) {
        return {};
    }
    char* msg = nullptr;
    std::string out = "librashader error";
    if (api.error_write && api.error_write(err, &msg) == 0 && msg != nullptr) {
        out = msg;
        if (api.error_free_string) {
            api.error_free_string(&msg);
        }
    }
    if (api.error_free) {
        api.error_free(&err);
    }
    return out;
}

_LIBRASHADER_IMPL_HANDLE open_libra_handle(const std::filesystem::path& path) {
#if defined(_WIN32)
    if (path.empty()) {
        return LoadLibraryW(L"librashader.dll");
    }
    return LoadLibraryW(path.wstring().c_str());
#else
    if (path.empty()) {
#if defined(__APPLE__)
        return dlopen("librashader.dylib", RTLD_LAZY);
#else
        return dlopen("librashader.so", RTLD_LAZY);
#endif
    }
    return dlopen(path.string().c_str(), RTLD_LAZY);
#endif
}

void close_libra_handle(_LIBRASHADER_IMPL_HANDLE h) {
    if (!h) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(h);
#else
    dlclose(h);
#endif
}

libra_instance_t load_instance_from_handle(_LIBRASHADER_IMPL_HANDLE h) {
    libra_instance_t instance = __librashader_make_null_instance();
    if (!h) {
        return instance;
    }
    _LIBRASHADER_ASSIGN(h, instance, instance_abi_version);
    _LIBRASHADER_ASSIGN(h, instance, instance_api_version);
    if (instance.instance_abi_version() != LIBRASHADER_CURRENT_ABI) {
        return __librashader_make_null_instance();
    }

    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_create);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_free);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_core_name);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_content_dir);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_param);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_runtime);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_core_rotation);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_user_rotation);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_screen_orientation);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_allow_rotation);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_view_aspect_orientation);
    _LIBRASHADER_ASSIGN(h, instance, preset_ctx_set_core_aspect_orientation);

    _LIBRASHADER_ASSIGN(h, instance, preset_create);
    _LIBRASHADER_ASSIGN(h, instance, preset_create_with_context);
    _LIBRASHADER_ASSIGN(h, instance, preset_create_with_options);
    _LIBRASHADER_ASSIGN(h, instance, preset_color_space);
    _LIBRASHADER_ASSIGN(h, instance, preset_free);
    _LIBRASHADER_ASSIGN(h, instance, preset_set_param);
    _LIBRASHADER_ASSIGN(h, instance, preset_get_param);
    _LIBRASHADER_ASSIGN(h, instance, preset_print);
    _LIBRASHADER_ASSIGN(h, instance, preset_get_runtime_params);
    _LIBRASHADER_ASSIGN(h, instance, preset_free_runtime_params);

    _LIBRASHADER_ASSIGN(h, instance, error_errno);
    _LIBRASHADER_ASSIGN(h, instance, error_print);
    _LIBRASHADER_ASSIGN(h, instance, error_free);
    _LIBRASHADER_ASSIGN(h, instance, error_write);
    _LIBRASHADER_ASSIGN(h, instance, error_free_string);

    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_create);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_create_deferred);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_frame);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_free);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_get_param);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_set_param);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_get_active_pass_count);
    _LIBRASHADER_ASSIGN(h, instance, vk_filter_chain_set_active_pass_count);

    instance.instance_loaded = true;
    return instance;
}

} // namespace

struct LibrashaderBridge::Impl {
    _LIBRASHADER_IMPL_HANDLE handle{};
    libra_instance_t api{__librashader_make_null_instance()};
    libra_vk_filter_chain_t vk_chain{nullptr};
};

LibrashaderBridge::~LibrashaderBridge() {
    unload_preset();
    if (impl_) {
        close_libra_handle(impl_->handle);
        delete impl_;
        impl_ = nullptr;
    }
}

bool LibrashaderBridge::try_load_library(const std::filesystem::path& explicit_path) {
    unload_preset();
    if (!impl_) {
        impl_ = new Impl();
    } else if (impl_->handle) {
        close_libra_handle(impl_->handle);
        impl_->handle = nullptr;
        impl_->api = __librashader_make_null_instance();
    }
    library_loaded_ = false;
    loaded_lib_path_.clear();

    std::vector<std::filesystem::path> candidates;
    if (!explicit_path.empty()) {
        candidates.push_back(explicit_path);
    }
#if defined(_WIN32)
    const char* lib_name = "librashader.dll";
#elif defined(__APPLE__)
    const char* lib_name = "librashader.dylib";
#else
    const char* lib_name = "librashader.so";
#endif
    candidates.push_back(std::filesystem::current_path() / lib_name);
    candidates.push_back(std::filesystem::current_path() / "lib" / lib_name);
    candidates.push_back(std::filesystem::path{}); // System / LD_LIBRARY_PATH / PATH search

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && !std::filesystem::exists(candidate)) {
            continue;
        }
        _LIBRASHADER_IMPL_HANDLE h = open_libra_handle(candidate);
        if (!h) {
            continue;
        }
        libra_instance_t api = load_instance_from_handle(h);
        if (api.instance_loaded) {
            impl_->handle = h;
            impl_->api = api;
            library_loaded_ = true;
            loaded_lib_path_ = candidate.empty() ? std::string(lib_name) : candidate.string();
            status_message_ = "Loaded " + loaded_lib_path_ +
                              " (API v" + std::to_string(api.instance_api_version()) + ")";
            return true;
        }
        close_libra_handle(h);
    }

    status_message_ = std::string(lib_name) +
                      " not found (optional). Place it beside the executable or set its path in Shaders tab.";
    return false;
}

void LibrashaderBridge::unload_preset() {
    if (impl_ && impl_->vk_chain && impl_->api.instance_loaded && impl_->api.vk_filter_chain_free) {
        libra_error_t err = impl_->api.vk_filter_chain_free(&impl_->vk_chain);
        if (err) {
            consume_libra_error(impl_->api, err);
        }
        impl_->vk_chain = nullptr;
    }
    chain_ = nullptr;
    active_preset_path_.clear();
    params_.clear();
}

bool LibrashaderBridge::load_preset(const std::filesystem::path& preset_path,
                                    VkInstance instance,
                                    VkPhysicalDevice physical_device,
                                    VkDevice device,
                                    VkQueue queue,
                                    uint32_t frames_in_flight,
                                    const std::map<std::string, float>& saved_params) {
    unload_preset();
    if (!library_loaded_ || !impl_ || !impl_->api.instance_loaded) {
        status_message_ = "Cannot load preset: librashader shared library is not loaded.";
        return false;
    }
    if (preset_path.empty() || !std::filesystem::exists(preset_path)) {
        status_message_ = "Preset file not found: " + preset_path.string();
        return false;
    }

    libra_preset_ctx_t ctx = nullptr;
    if (impl_->api.preset_ctx_create) {
        libra_error_t err = impl_->api.preset_ctx_create(&ctx);
        if (err) {
            status_message_ = "preset_ctx_create failed: " + consume_libra_error(impl_->api, err);
            return false;
        }
        if (impl_->api.preset_ctx_set_runtime) {
            impl_->api.preset_ctx_set_runtime(&ctx, LIBRA_PRESET_CTX_RUNTIME_VULKAN);
        }
        if (impl_->api.preset_ctx_set_core_name) {
            impl_->api.preset_ctx_set_core_name(&ctx, "fzero-gx-pc");
        }
    }

    libra_preset_opt_t preset_opt{};
    preset_opt.version = LIBRASHADER_CURRENT_VERSION;
    preset_opt.original_aspect_uniforms = true;
    preset_opt.frametime_uniforms = true;
    preset_opt.sensor_uniforms = false;

    libra_shader_preset_t preset = nullptr;
    libra_error_t err = impl_->api.preset_create_with_options(
        preset_path.string().c_str(),
        ctx ? &ctx : nullptr,
        &preset_opt,
        &preset);
    if (ctx && impl_->api.preset_ctx_free) {
        impl_->api.preset_ctx_free(&ctx);
    }
    if (err || !preset) {
        status_message_ = "Failed to parse preset: " + consume_libra_error(impl_->api, err);
        return false;
    }

    // Read runtime parameters before filter_chain_create consumes preset.
    if (impl_->api.preset_get_runtime_params && impl_->api.preset_free_runtime_params) {
        libra_preset_param_list_t list{};
        if (auto p_err = impl_->api.preset_get_runtime_params(&preset, &list); !p_err) {
            params_.reserve(static_cast<size_t>(list.length));
            for (uint64_t i = 0; i < list.length; ++i) {
                const auto& p = list.parameters[i];
                LibrashaderParam out{};
                out.name = p.name ? p.name : "";
                out.description = p.description ? p.description : out.name;
                out.initial = p.initial;
                out.minimum = p.minimum;
                out.maximum = p.maximum;
                out.step = p.step > 0.0f ? p.step : 0.01f;
                auto it = saved_params.find(out.name);
                out.current = (it != saved_params.end()) ? it->second : out.initial;
                params_.push_back(std::move(out));
            }
            impl_->api.preset_free_runtime_params(list);
        } else {
            consume_libra_error(impl_->api, p_err);
        }
    }

    libra_device_vk_t vk_dev{};
    vk_dev.physical_device = physical_device;
    vk_dev.instance = instance;
    vk_dev.device = device;
    vk_dev.queue = queue;
    vk_dev.entry = vkGetInstanceProcAddr;

    filter_chain_vk_opt_t chain_opt{};
    chain_opt.version = LIBRASHADER_CURRENT_VERSION;
    chain_opt.frames_in_flight = frames_in_flight;
    chain_opt.force_no_mipmaps = false;
    chain_opt.use_dynamic_rendering = true;
    chain_opt.disable_cache = false;

    err = impl_->api.vk_filter_chain_create(&preset, vk_dev, &chain_opt, &impl_->vk_chain);
    if (err || !impl_->vk_chain) {
        params_.clear();
        status_message_ = "Failed to build Vulkan filter chain: " + consume_libra_error(impl_->api, err);
        return false;
    }

    for (const auto& p : params_) {
        if (p.current != p.initial) {
            set_param(p.name, p.current);
        }
    }

    chain_ = impl_->vk_chain;
    active_preset_path_ = preset_path.string();
    status_message_ = "Active preset: " + preset_path.filename().string() +
                      " (" + std::to_string(params_.size()) + " parameters)";
    return true;
}

bool LibrashaderBridge::set_param(const std::string& name, float value) {
    if (!impl_ || !impl_->vk_chain || !impl_->api.vk_filter_chain_set_param) {
        return false;
    }
    libra_error_t err = impl_->api.vk_filter_chain_set_param(&impl_->vk_chain, name.c_str(), value);
    if (err) {
        status_message_ = "Failed setting parameter '" + name + "': " + consume_libra_error(impl_->api, err);
        return false;
    }
    for (auto& p : params_) {
        if (p.name == name) {
            p.current = value;
            break;
        }
    }
    return true;
}

bool LibrashaderBridge::render_frame(VkCommandBuffer cmd,
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
                                     uint32_t frametime_delta_ms) {
    if (!impl_ || !impl_->vk_chain || !impl_->api.vk_filter_chain_frame) {
        return false;
    }

    libra_image_vk_t src{};
    src.handle = src_image;
    src.format = src_format;
    src.width = src_width;
    src.height = src_height;

    libra_image_vk_t dst{};
    dst.handle = dst_image;
    dst.format = dst_format;
    dst.width = dst_width;
    dst.height = dst_height;

    // Aspect-preserving centered viewport inside dst_image
    const float dst_aspect = dst_height > 0 ? static_cast<float>(dst_width) / static_cast<float>(dst_height) : 1.0f;
    libra_viewport_t vp{};
    if (dst_aspect > aspect_ratio) {
        vp.height = dst_height;
        vp.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(dst_height) * aspect_ratio));
        vp.x = static_cast<float>((dst_width - vp.width) / 2);
        vp.y = 0.0f;
    } else {
        vp.width = dst_width;
        vp.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(dst_width) / std::max(aspect_ratio, 0.1f)));
        vp.x = 0.0f;
        vp.y = static_cast<float>((dst_height - vp.height) / 2);
    }

    frame_vk_opt_t opt{};
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.clear_history = (frame_count <= 1);
    opt.frame_direction = 1;
    opt.rotation = 0;
    opt.total_subframes = 1;
    opt.current_subframe = 1;
    opt.aspect_ratio = aspect_ratio;
    opt.frames_per_second = fps > 0.0f ? fps : 60.0f;
    opt.frametime_delta = frametime_delta_ms;
    opt.color_space = LIBRA_COLOR_SPACE_SDR;
    opt.brightness_nits = 200.0f;

    libra_error_t err = impl_->api.vk_filter_chain_frame(
        &impl_->vk_chain, cmd, frame_count, src, dst, &vp, nullptr, &opt);
    if (err) {
        status_message_ = "Filter chain frame error: " + consume_libra_error(impl_->api, err);
        return false;
    }
    return true;
}

} // namespace fzgx
