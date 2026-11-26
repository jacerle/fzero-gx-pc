#include "video/gx_vulkan.hpp"

#include "embedded_shaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <span>

namespace fzgx {

namespace {

constexpr uint32_t kMaxMaterialDescriptors = 2048;
constexpr uint32_t kMaxLiveMaterialDescriptors = 512;
constexpr VkDeviceSize kDynVboBytes = 4u * 1024u * 1024u;
constexpr VkDeviceSize kDynIboBytes = 2u * 1024u * 1024u;

struct alignas(16) GxCameraUboGpu {
    float mvp[16];
    float modelview[16];
    float normal_mat[16];
    float light_dir_intensity[4];
    float ambient_color[4];
    float mat_color[4];
    uint32_t draw_flags[4]; // x: has_vtx_color, y: env_map_uv1, z: debug_mode, w: is_tex_preview
};
static_assert(sizeof(GxCameraUboGpu) == 256);

struct alignas(16) GxTevUboGpu {
    uint32_t counts_and_alpha[4]; // x: num_stages, y: comp0|(comp1<<4)|(op<<8), z: fog_type, w: flags
    float alpha_refs_fog[4];      // x: ref0, y: ref1, z: fog_start_z, w: fog_end_z
    float fog_color[4];
    float tev_regs[4][4];
    float konst_regs[4][4];
    uint32_t swap_tables[4][4];
    uint32_t stage_color[16][4];
    uint32_t stage_alpha[16][4];
};
static_assert(sizeof(GxTevUboGpu) == 752);

VkDeviceSize align_up_dev(VkDeviceSize v, VkDeviceSize align) {
    if (align <= 1) return v;
    return ((v + align - 1) / align) * align;
}

void pack_tev_ubo(const GxTevConfig& cfg,
                  bool viewer_textures,
                  bool viewer_lighting,
                  bool viewer_fog,
                  GxTevUboGpu& out) {
    std::memset(&out, 0, sizeof(out));
    out.counts_and_alpha[0] = std::clamp<uint32_t>(cfg.num_stages, 1u, 16u);
    out.counts_and_alpha[1] = (static_cast<uint32_t>(cfg.alpha_comp0) & 0xFu) |
                              ((static_cast<uint32_t>(cfg.alpha_comp1) & 0xFu) << 4) |
                              ((static_cast<uint32_t>(cfg.alpha_op) & 0x3u) << 8);
    out.counts_and_alpha[2] = static_cast<uint32_t>(cfg.fog_type);
    uint32_t flags = 0;
    if (cfg.enable_textures && viewer_textures) flags |= 1u;
    if (cfg.enable_lighting && viewer_lighting) flags |= 2u;
    if (cfg.enable_fog && viewer_fog) flags |= 4u;
    out.counts_and_alpha[3] = flags;

    out.alpha_refs_fog[0] = cfg.alpha_ref0;
    out.alpha_refs_fog[1] = cfg.alpha_ref1;
    out.alpha_refs_fog[2] = cfg.fog_start_z;
    out.alpha_refs_fog[3] = cfg.fog_end_z;
    std::memcpy(out.fog_color, cfg.fog_color.data(), sizeof(out.fog_color));
    std::memcpy(out.tev_regs, cfg.tev_regs.data(), sizeof(out.tev_regs));
    std::memcpy(out.konst_regs, cfg.konst_regs.data(), sizeof(out.konst_regs));
    for (size_t i = 0; i < 4; ++i) {
        for (size_t k = 0; k < 4; ++k) {
            out.swap_tables[i][k] = cfg.swap_table[i][k];
        }
    }

    for (size_t s = 0; s < 16; ++s) {
        const auto& st = cfg.stages[s];
        out.stage_color[s][0] = static_cast<uint32_t>(st.color_in[0]) |
                                (static_cast<uint32_t>(st.color_in[1]) << 8) |
                                (static_cast<uint32_t>(st.color_in[2]) << 16) |
                                (static_cast<uint32_t>(st.color_in[3]) << 24);
        out.stage_color[s][1] = (static_cast<uint32_t>(st.color_op) & 1u) |
                                ((static_cast<uint32_t>(st.color_bias) & 3u) << 4) |
                                ((static_cast<uint32_t>(st.color_scale) & 3u) << 8) |
                                ((st.color_clamp ? 1u : 0u) << 12) |
                                ((static_cast<uint32_t>(st.color_out_reg) & 3u) << 16);
        out.stage_color[s][2] = static_cast<uint32_t>(st.tex_map) |
                                (static_cast<uint32_t>(st.tex_coord) << 8) |
                                (static_cast<uint32_t>(st.ras_chan) << 16) |
                                (static_cast<uint32_t>(st.kcolor_sel) << 24);
        out.stage_color[s][3] = static_cast<uint32_t>(st.ras_swap) |
                                (static_cast<uint32_t>(st.tex_swap) << 8);

        out.stage_alpha[s][0] = static_cast<uint32_t>(st.alpha_in[0]) |
                                (static_cast<uint32_t>(st.alpha_in[1]) << 8) |
                                (static_cast<uint32_t>(st.alpha_in[2]) << 16) |
                                (static_cast<uint32_t>(st.alpha_in[3]) << 24);
        out.stage_alpha[s][1] = (static_cast<uint32_t>(st.alpha_op) & 1u) |
                                ((static_cast<uint32_t>(st.alpha_bias) & 3u) << 4) |
                                ((static_cast<uint32_t>(st.alpha_scale) & 3u) << 8) |
                                ((st.alpha_clamp ? 1u : 0u) << 12) |
                                ((static_cast<uint32_t>(st.alpha_out_reg) & 3u) << 16);
        out.stage_alpha[s][2] = static_cast<uint32_t>(st.kalpha_sel);
        out.stage_alpha[s][3] = 0;
    }
}

struct Mat4 {
    float m[16]{}; // Column-major
};

Mat4 mat4_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

Mat4 mat4_perspective_vulkan(float fovy_rad, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovy_rad * 0.5f);
    Mat4 r{};
    r.m[0] = f / std::max(aspect, 0.05f);
    r.m[5] = -f;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (znear * zfar) / (znear - zfar);
    return r;
}

Mat4 mat4_look_at(const std::array<float, 3>& eye,
                  const std::array<float, 3>& center,
                  const std::array<float, 3>& up) {
    float fx = center[0] - eye[0];
    float fy = center[1] - eye[1];
    float fz = center[2] - eye[2];
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-6f) {
        fx /= flen; fy /= flen; fz /= flen;
    } else {
        fz = -1.0f;
    }

    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    const float slen = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-6f) {
        sx /= slen; sy /= slen; sz /= slen;
    } else {
        sx = 1.0f; sy = sz = 0.0f;
    }

    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;

    Mat4 r = mat4_identity();
    r.m[0] = sx;  r.m[4] = sy;  r.m[8]  = sz;
    r.m[1] = ux;  r.m[5] = uy;  r.m[9]  = uz;
    r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
    r.m[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    r.m[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    r.m[14] = (fx * eye[0] + fy * eye[1] + fz * eye[2]);
    return r;
}

VkShaderModule create_shader_mod(VkDevice device, std::span<const uint32_t> code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size_bytes();
    ci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkSamplerAddressMode to_vk_address_mode(GxWrapMode w) {
    switch (w) {
    case GxWrapMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case GxWrapMode::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkCompareOp to_vk_compare(GxCompare cmp) {
    switch (cmp) {
    case GxCompare::Never: return VK_COMPARE_OP_NEVER;
    case GxCompare::Less: return VK_COMPARE_OP_LESS;
    case GxCompare::Equal: return VK_COMPARE_OP_EQUAL;
    case GxCompare::LEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GxCompare::Greater: return VK_COMPARE_OP_GREATER;
    case GxCompare::NEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case GxCompare::GEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case GxCompare::Always:
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

uint32_t make_pipeline_key(GxCullMode cull,
                           GxBlendMode blend,
                           bool depth_test,
                           bool depth_write,
                           GxCompare depth_func,
                           bool wireframe) {
    return (static_cast<uint32_t>(cull) & 0x3u) |
           ((static_cast<uint32_t>(blend) & 0x3u) << 2) |
           ((depth_test ? 1u : 0u) << 4) |
           ((depth_write ? 1u : 0u) << 5) |
           ((static_cast<uint32_t>(depth_func) & 0x7u) << 6) |
           ((wireframe ? 1u : 0u) << 9);
}

uint32_t make_sampler_key(GxWrapMode ws, GxWrapMode wt, bool nearest) {
    return (static_cast<uint32_t>(ws) & 0x3u) |
           ((static_cast<uint32_t>(wt) & 0x3u) << 2) |
           ((nearest ? 1u : 0u) << 4);
}

void cmd_image_barrier(VkCommandBuffer cmd,
                       VkImage image,
                       VkImageAspectFlags aspect,
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
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void build_rot_trans_3x4(float rx, float ry, float tx, float ty, float tz, float out[12]) {
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    // R_y * R_x
    out[0] = cy;      out[1] = sy * sx; out[2]  = sy * cx; out[3]  = tx;
    out[4] = 0.0f;    out[5] = cx;      out[6]  = -sx;     out[7]  = ty;
    out[8] = -sy;     out[9] = cy * sx; out[10] = cy * cx; out[11] = tz;
}

} // namespace

GxVulkanBackend::~GxVulkanBackend() {
    shutdown();
}

uint32_t GxVulkanBackend::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool GxVulkanBackend::execute_oneshot(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &cbai, &cmd) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    return true;
}

bool GxVulkanBackend::create_buffer(VkDeviceSize size,
                                    VkBufferUsageFlags usage,
                                    VkMemoryPropertyFlags props,
                                    GpuBuffer& out_buf,
                                    const void* initial_data,
                                    bool keep_mapped) {
    destroy_buffer(out_buf);
    if (size == 0) return false;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bci, nullptr, &out_buf.buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, out_buf.buffer, &req);
    const uint32_t mem_idx = find_memory_type(req.memoryTypeBits, props);
    if (mem_idx == UINT32_MAX) {
        destroy_buffer(out_buf);
        return false;
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_idx;
    if (vkAllocateMemory(device_, &mai, nullptr, &out_buf.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, out_buf.buffer, out_buf.memory, 0) != VK_SUCCESS) {
        destroy_buffer(out_buf);
        return false;
    }

    out_buf.size = size;
    if ((initial_data != nullptr || keep_mapped) && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, out_buf.memory, 0, size, 0, &mapped) != VK_SUCCESS) {
            destroy_buffer(out_buf);
            return false;
        }
        if (initial_data != nullptr) {
            std::memcpy(mapped, initial_data, static_cast<size_t>(size));
        }
        if (keep_mapped) {
            out_buf.mapped = mapped;
        } else {
            vkUnmapMemory(device_, out_buf.memory);
        }
    }
    return true;
}

void GxVulkanBackend::destroy_buffer(GpuBuffer& buf) {
    if (buf.mapped && buf.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, buf.memory);
        buf.mapped = nullptr;
    }
    if (buf.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
    }
    if (buf.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
    }
    buf.size = 0;
}

bool GxVulkanBackend::upload_rgba_texture(uint32_t width,
                                          uint32_t height,
                                          const uint8_t* rgba,
                                          GpuTexture& out_tex) {
    destroy_texture(out_tex);
    if (!rgba || width == 0 || height == 0) {
        return false;
    }

    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(width) * height * 4u;
    GpuBuffer staging{};
    if (!create_buffer(byte_count,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging,
                       rgba)) {
        return false;
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &out_tex.image) != VK_SUCCESS) {
        destroy_buffer(staging);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, out_tex.image, &req);
    const uint32_t mem_idx = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        destroy_buffer(staging);
        destroy_texture(out_tex);
        return false;
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_idx;
    if (vkAllocateMemory(device_, &mai, nullptr, &out_tex.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, out_tex.image, out_tex.memory, 0) != VK_SUCCESS) {
        destroy_buffer(staging);
        destroy_texture(out_tex);
        return false;
    }

    const bool copy_ok = execute_oneshot([&](VkCommandBuffer cmd) {
        cmd_image_barrier(cmd, out_tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, out_tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        cmd_image_barrier(cmd, out_tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    });

    destroy_buffer(staging);
    if (!copy_ok) {
        destroy_texture(out_tex);
        return false;
    }

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = out_tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &ivci, nullptr, &out_tex.view) != VK_SUCCESS) {
        destroy_texture(out_tex);
        return false;
    }

    out_tex.width = width;
    out_tex.height = height;
    out_tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out_tex.valid = true;
    return true;
}

bool GxVulkanBackend::ensure_efb_texture(uint32_t efb_id, uint32_t width, uint32_t height) {
    width = std::clamp(width, 16u, 2048u);
    height = std::clamp(height, 16u, 2048u);
    auto& tex = efb_textures_[efb_id];
    if (tex.valid && tex.width == width && tex.height == height) {
        return true;
    }
    vkDeviceWaitIdle(device_);
    destroy_texture(tex);
    live_mat_cache_.clear();
    if (live_mat_desc_pool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device_, live_mat_desc_pool_, 0);
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &tex.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, tex.image, &req);
    const uint32_t mem_idx = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        destroy_texture(tex);
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_idx;
    if (vkAllocateMemory(device_, &mai, nullptr, &tex.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, tex.image, tex.memory, 0) != VK_SUCCESS) {
        destroy_texture(tex);
        return false;
    }

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &ivci, nullptr, &tex.view) != VK_SUCCESS) {
        destroy_texture(tex);
        return false;
    }

    tex.width = width;
    tex.height = height;
    tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex.valid = true;
    return true;
}

void GxVulkanBackend::destroy_texture(GpuTexture& tex) {
    if (tex.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, tex.view, nullptr);
        tex.view = VK_NULL_HANDLE;
    }
    if (tex.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, tex.image, nullptr);
        tex.image = VK_NULL_HANDLE;
    }
    if (tex.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, tex.memory, nullptr);
        tex.memory = VK_NULL_HANDLE;
    }
    tex.width = 0;
    tex.height = 0;
    tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex.valid = false;
}

VkSampler GxVulkanBackend::get_or_create_sampler(GxWrapMode wrap_s, GxWrapMode wrap_t, bool nearest) {
    const uint32_t key = make_sampler_key(wrap_s, wrap_t, nearest);
    auto it = sampler_cache_.find(key);
    if (it != sampler_cache_.end()) {
        return it->second;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = to_vk_address_mode(wrap_s);
    sci.addressModeV = to_vk_address_mode(wrap_t);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;

    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(device_, &sci, nullptr, &smp);
    sampler_cache_[key] = smp;
    return smp;
}

VkPipeline GxVulkanBackend::get_or_create_pipeline(GxCullMode cull,
                                                   GxBlendMode blend,
                                                   bool depth_test,
                                                   bool depth_write,
                                                   GxCompare depth_func,
                                                   bool wireframe) {
    const bool use_wire = wireframe && supports_wireframe_;
    const uint32_t key = make_pipeline_key(cull, blend, depth_test, depth_write, depth_func, use_wire);
    auto it = pipeline_cache_.find(key);
    if (it != pipeline_cache_.end()) {
        return it->second;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_shader_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbind{};
    vbind.binding = 0;
    vbind.stride = sizeof(GxVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 5> vattrs{};
    vattrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GxVertex, pos))};
    vattrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GxVertex, normal))};
    vattrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(GxVertex, color))};
    vattrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GxVertex, uv0))};
    vattrs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(GxVertex, uv1))};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(vattrs.size());
    vi.pVertexAttributeDescriptions = vattrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = use_wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    switch (cull) {
    case GxCullMode::Back: rs.cullMode = VK_CULL_MODE_BACK_BIT; break;
    case GxCullMode::Front: rs.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    default: rs.cullMode = VK_CULL_MODE_NONE; break;
    }

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (depth_write && (blend == GxBlendMode::Opaque || blend == GxBlendMode::Cutout))
                              ? VK_TRUE
                              : VK_FALSE;
    ds.depthCompareOp = to_vk_compare(depth_func);

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend == GxBlendMode::AlphaBlend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend == GxBlendMode::Additive) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &color_format_;
    prci.depthAttachmentFormat = depth_format_;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.pNext = &prci;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout_;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    pipeline_cache_[key] = pipe;
    return pipe;
}

VkDescriptorSet GxVulkanBackend::allocate_material_descriptor(const std::array<VkImageView, 4>& views,
                                                              const std::array<VkSampler, 4>& samplers) {
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = mat_desc_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &mat_desc_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &dsai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo imgs[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        imgs[i].sampler = samplers[i];
        imgs[i].imageView = views[i];
        imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 4;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = imgs;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return set;
}

bool GxVulkanBackend::initialize(VkPhysicalDevice phys,
                                 VkDevice device,
                                 uint32_t graphics_family,
                                 VkQueue graphics_queue,
                                 VkCommandPool cmd_pool,
                                 VkFormat color_format,
                                 VkFormat depth_format,
                                 bool supports_wireframe,
                                 std::string* error_out) {
    physical_device_ = phys;
    device_ = device;
    graphics_family_ = graphics_family;
    graphics_queue_ = graphics_queue;
    cmd_pool_ = cmd_pool;
    color_format_ = color_format;
    depth_format_ = depth_format;
    supports_wireframe_ = supports_wireframe;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    min_ubo_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    cam_stride_ = align_up_dev(sizeof(GxCameraUboGpu), min_ubo_align_);
    tev_stride_ = align_up_dev(sizeof(GxTevUboGpu), min_ubo_align_);

    vert_shader_ = create_shader_mod(device_, shaders::gx_3d_vert);
    frag_shader_ = create_shader_mod(device_, shaders::gx_3d_frag);
    if (!vert_shader_ || !frag_shader_) {
        if (error_out) *error_out = "Failed creating GX 3D SPIR-V shader modules.";
        return false;
    }

    // Set 0: Dynamic Camera UBO (binding 0) + Dynamic TEV UBO (binding 1)
    VkDescriptorSetLayoutBinding ubo_b[2]{};
    ubo_b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    ubo_b[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ubo_dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ubo_dsl.bindingCount = 2;
    ubo_dsl.pBindings = ubo_b;
    if (vkCreateDescriptorSetLayout(device_, &ubo_dsl, nullptr, &ubo_desc_layout_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating GX dynamic UBO descriptor layout.";
        return false;
    }

    // Set 1: 4 texture slots (u_tex[4])
    VkDescriptorSetLayoutBinding mat_b{};
    mat_b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo mat_dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    mat_dsl.bindingCount = 1;
    mat_dsl.pBindings = &mat_b;
    if (vkCreateDescriptorSetLayout(device_, &mat_dsl, nullptr, &mat_desc_layout_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating GX material descriptor layout.";
        return false;
    }

    VkDescriptorPoolSize ubo_ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFramesInFlight * 2};
    VkDescriptorPoolCreateInfo ubo_dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ubo_dp.maxSets = kFramesInFlight;
    ubo_dp.poolSizeCount = 1;
    ubo_dp.pPoolSizes = &ubo_ps;
    vkCreateDescriptorPool(device_, &ubo_dp, nullptr, &ubo_desc_pool_);

    VkDescriptorSetLayout ubo_layouts[kFramesInFlight] = {ubo_desc_layout_, ubo_desc_layout_};
    VkDescriptorSetAllocateInfo ubo_ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ubo_ai.descriptorPool = ubo_desc_pool_;
    ubo_ai.descriptorSetCount = kFramesInFlight;
    ubo_ai.pSetLayouts = ubo_layouts;
    vkAllocateDescriptorSets(device_, &ubo_ai, ubo_desc_sets_.data());

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!create_buffer(cam_stride_ * kMaxDrawsPerFrame,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           cam_ubos_[i], nullptr, true) ||
            !create_buffer(tev_stride_ * kMaxDrawsPerFrame,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           tev_ubos_[i], nullptr, true) ||
            !create_buffer(kDynVboBytes,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           dyn_vbos_[i], nullptr, true) ||
            !create_buffer(kDynIboBytes,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           dyn_ibos_[i], nullptr, true)) {
            if (error_out) *error_out = "Failed creating GX dynamic uniform/vertex buffers.";
            return false;
        }

        VkDescriptorBufferInfo dbis[2]{};
        dbis[0] = {cam_ubos_[i].buffer, 0, sizeof(GxCameraUboGpu)};
        dbis[1] = {tev_ubos_[i].buffer, 0, sizeof(GxTevUboGpu)};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = ubo_desc_sets_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[b].pBufferInfo = &dbis[b];
        }
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    // Material descriptor pools (static scene + live per-EFB/texture cache)
    VkDescriptorPoolSize mat_ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterialDescriptors * 4};
    VkDescriptorPoolCreateInfo mat_dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    mat_dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    mat_dp.maxSets = kMaxMaterialDescriptors;
    mat_dp.poolSizeCount = 1;
    mat_dp.pPoolSizes = &mat_ps;
    vkCreateDescriptorPool(device_, &mat_dp, nullptr, &mat_desc_pool_);

    VkDescriptorPoolSize lmat_ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxLiveMaterialDescriptors * 4};
    VkDescriptorPoolCreateInfo lmat_dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    lmat_dp.maxSets = kMaxLiveMaterialDescriptors;
    lmat_dp.poolSizeCount = 1;
    lmat_dp.pPoolSizes = &lmat_ps;
    vkCreateDescriptorPool(device_, &lmat_dp, nullptr, &live_mat_desc_pool_);

    VkDescriptorSetLayout set_layouts[2] = {ubo_desc_layout_, mat_desc_layout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = set_layouts;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        if (error_out) *error_out = "Failed creating GX 3D pipeline layout.";
        return false;
    }

    const uint8_t white_px[4] = {255, 255, 255, 255};
    if (!upload_rgba_texture(1, 1, white_px, white_fallback_)) {
        if (error_out) *error_out = "Failed creating 1x1 white fallback texture.";
        return false;
    }
    // 16x16 procedural F-Zero cyan/dark grid texture for Live GX demo
    std::vector<uint8_t> chk(64 * 64 * 4, 0);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const bool border = (x < 3 || y < 3 || x >= 61 || y >= 61);
            const bool grid = ((x / 16 + y / 16) & 1u) == 0;
            const size_t idx = (static_cast<size_t>(y) * 64 + x) * 4u;
            if (border) {
                chk[idx + 0] = 40; chk[idx + 1] = 220; chk[idx + 2] = 255; chk[idx + 3] = 255;
            } else if (grid) {
                chk[idx + 0] = 32; chk[idx + 1] = 68; chk[idx + 2] = 135; chk[idx + 3] = 255;
            } else {
                chk[idx + 0] = 18; chk[idx + 1] = 26; chk[idx + 2] = 52; chk[idx + 3] = 255;
            }
        }
    }
    upload_rgba_texture(64, 64, chk.data(), checker_fallback_);
    return true;
}

void GxVulkanBackend::destroy_scene_resources() {
    if (device_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_);

    for (auto& m : scene_models_) {
        for (auto& sm : m.submeshes) {
            destroy_buffer(sm.vbo);
            destroy_buffer(sm.ibo);
        }
    }
    scene_models_.clear();

    for (auto& t : scene_textures_) {
        destroy_texture(t);
    }
    scene_textures_.clear();

    for (auto& [id, t] : efb_textures_) {
        destroy_texture(t);
    }
    efb_textures_.clear();
    live_mat_cache_.clear();

    if (mat_desc_pool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device_, mat_desc_pool_, 0);
    }
    if (live_mat_desc_pool_ != VK_NULL_HANDLE) {
        vkResetDescriptorPool(device_, live_mat_desc_pool_, 0);
    }

    model_summaries_.clear();
    texture_summaries_.clear();
    scene_label_.clear();
    total_submeshes_ = 0;
    total_triangles_ = 0;
    last_efb_copies_ = 0;
    last_max_tev_stages_ = 1;
    has_scene_ = false;
    is_tpl_preview_ = false;
    live_demo_active_ = false;
    viewer_.preview_tpl_index = -1;
}

void GxVulkanBackend::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_scene_resources();
    destroy_texture(white_fallback_);
    destroy_texture(checker_fallback_);

    for (auto& [k, pipe] : pipeline_cache_) {
        if (pipe != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipe, nullptr);
    }
    pipeline_cache_.clear();

    for (auto& [k, smp] : sampler_cache_) {
        if (smp != VK_NULL_HANDLE) vkDestroySampler(device_, smp, nullptr);
    }
    sampler_cache_.clear();

    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        destroy_buffer(cam_ubos_[i]);
        destroy_buffer(tev_ubos_[i]);
        destroy_buffer(dyn_vbos_[i]);
        destroy_buffer(dyn_ibos_[i]);
    }
    if (live_mat_desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, live_mat_desc_pool_, nullptr);
        live_mat_desc_pool_ = VK_NULL_HANDLE;
    }
    if (mat_desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, mat_desc_pool_, nullptr);
        mat_desc_pool_ = VK_NULL_HANDLE;
    }
    if (ubo_desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, ubo_desc_pool_, nullptr);
        ubo_desc_pool_ = VK_NULL_HANDLE;
    }
    if (mat_desc_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, mat_desc_layout_, nullptr);
        mat_desc_layout_ = VK_NULL_HANDLE;
    }
    if (ubo_desc_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, ubo_desc_layout_, nullptr);
        ubo_desc_layout_ = VK_NULL_HANDLE;
    }
    if (vert_shader_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vert_shader_, nullptr);
        vert_shader_ = VK_NULL_HANDLE;
    }
    if (frag_shader_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, frag_shader_, nullptr);
        frag_shader_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void GxVulkanBackend::clear_scene() {
    external_live_active_ = false;
    destroy_scene_resources();
}

void GxVulkanBackend::enable_live_demo_scene() {
    destroy_scene_resources();
    scene_label_ = "Live GX / TEV / EFB Copy Test";
    live_demo_active_ = true;
    viewer_.auto_rotate = true;
    viewer_.yaw_deg = 25.0f;
    viewer_.pitch_deg = 16.0f;
    viewer_.distance = 9.0f;
    viewer_.target = {0.0f, 0.0f, 0.0f};
}

bool GxVulkanBackend::upload_gma_scene(const GmaArchive& gma,
                                       const TplArchive* tpl,
                                       const std::string& scene_label,
                                       std::string* error_out) {
    destroy_scene_resources();
    if (!gma.is_loaded() || gma.valid_model_count() == 0) {
        if (error_out) *error_out = "GMA archive contains no valid 3D models.";
        return false;
    }

    if (tpl && tpl->is_loaded()) {
        const auto& tlist = tpl->textures();
        scene_textures_.resize(tlist.size());
        texture_summaries_.resize(tlist.size());
        for (size_t i = 0; i < tlist.size(); ++i) {
            const auto& t = tlist[i];
            texture_summaries_[i] = {
                static_cast<uint32_t>(i),
                t.valid,
                t.width,
                t.height,
                gx_tex_fmt_name(t.format),
            };
            if (t.valid && !t.rgba8.empty()) {
                upload_rgba_texture(t.width, t.height, t.rgba8.data(), scene_textures_[i]);
            }
        }
    }

    VkSampler default_smp = get_or_create_sampler(GxWrapMode::Clamp, GxWrapMode::Clamp, false);

    const auto& models = gma.models();
    scene_models_.resize(models.size());
    model_summaries_.resize(models.size());

    for (size_t mi = 0; mi < models.size(); ++mi) {
        const GmaModel& src_m = models[mi];
        GpuModel& dst_m = scene_models_[mi];
        dst_m.name = src_m.name;
        dst_m.valid = src_m.valid;
        dst_m.bound_center = src_m.bound_center;
        dst_m.bound_radius = src_m.bound_radius;

        uint32_t model_tris = 0;
        if (src_m.valid) {
            for (const auto& src_sm : src_m.submeshes) {
                if (src_sm.vertices.empty() || src_sm.indices.empty()) continue;

                GpuSubMesh dst_sm{};
                const VkDeviceSize vbytes = static_cast<VkDeviceSize>(src_sm.vertices.size()) * sizeof(GxVertex);
                const VkDeviceSize ibytes = static_cast<VkDeviceSize>(src_sm.indices.size()) * sizeof(uint32_t);
                if (!create_buffer(vbytes,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   dst_sm.vbo,
                                   src_sm.vertices.data()) ||
                    !create_buffer(ibytes,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   dst_sm.ibo,
                                   src_sm.indices.data())) {
                    destroy_buffer(dst_sm.vbo);
                    destroy_buffer(dst_sm.ibo);
                    continue;
                }

                dst_sm.index_count = static_cast<uint32_t>(src_sm.indices.size());
                dst_sm.material_rgba = src_sm.material_rgba;
                dst_sm.tev = build_gma_submesh_tev(src_sm, src_m.layers);
                last_max_tev_stages_ = std::max(last_max_tev_stages_, dst_sm.tev.num_stages);
                dst_sm.is_transparent = src_sm.is_transparent;
                dst_sm.double_sided = src_sm.double_sided;
                dst_sm.unlit = src_sm.unlit;
                dst_sm.has_vertex_color = src_sm.has_vertex_color;
                dst_sm.blend_mode = src_sm.blend_mode;

                std::array<VkImageView, 4> views{
                    white_fallback_.view, white_fallback_.view, white_fallback_.view, white_fallback_.view};
                std::array<VkSampler, 4> smps{
                    default_smp, default_smp, default_smp, default_smp};

                if (src_sm.primary_tpl_index >= 0 &&
                    static_cast<size_t>(src_sm.primary_tpl_index) < scene_textures_.size() &&
                    scene_textures_[static_cast<size_t>(src_sm.primary_tpl_index)].valid) {
                    views[0] = scene_textures_[static_cast<size_t>(src_sm.primary_tpl_index)].view;
                    smps[0] = get_or_create_sampler(src_sm.wrap_s, src_sm.wrap_t, src_sm.nearest_filter);
                }

                if (src_sm.secondary_tpl_index >= 0 &&
                    static_cast<size_t>(src_sm.secondary_tpl_index) < scene_textures_.size() &&
                    scene_textures_[static_cast<size_t>(src_sm.secondary_tpl_index)].valid) {
                    views[1] = scene_textures_[static_cast<size_t>(src_sm.secondary_tpl_index)].view;
                    for (const auto& l : src_m.layers) {
                        if (static_cast<int32_t>(l.tpl_index) == src_sm.secondary_tpl_index) {
                            dst_sm.env_map_uv1 = l.is_env_map;
                            break;
                        }
                    }
                }

                dst_sm.mat_desc_set = allocate_material_descriptor(views, smps);
                model_tris += dst_sm.index_count / 3u;
                ++total_submeshes_;
                dst_m.submeshes.push_back(std::move(dst_sm));
            }
        }

        total_triangles_ += model_tris;
        model_summaries_[mi] = {
            src_m.name,
            src_m.valid && !dst_m.submeshes.empty(),
            static_cast<uint32_t>(dst_m.submeshes.size()),
            model_tris,
            src_m.bound_radius,
        };
    }

    if (total_submeshes_ == 0) {
        destroy_scene_resources();
        if (error_out) *error_out = "No renderable submeshes produced from GMA archive.";
        return false;
    }

    scene_label_ = scene_label;
    has_scene_ = true;
    is_tpl_preview_ = false;
    viewer_.selected_model_index = -1;
    viewer_.auto_rotate = true;
    auto_frame_camera();
    return true;
}

bool GxVulkanBackend::upload_tpl_preview(const TplArchive& tpl,
                                         int tex_index,
                                         const std::string& label,
                                         std::string* error_out) {
    destroy_scene_resources();
    if (!tpl.is_loaded() || tpl.valid_texture_count() == 0) {
        if (error_out) *error_out = "TPL contains no valid textures.";
        return false;
    }

    const auto& tlist = tpl.textures();
    scene_textures_.resize(tlist.size());
    texture_summaries_.resize(tlist.size());
    int first_valid = -1;

    for (size_t i = 0; i < tlist.size(); ++i) {
        const auto& t = tlist[i];
        texture_summaries_[i] = {
            static_cast<uint32_t>(i), t.valid, t.width, t.height, gx_tex_fmt_name(t.format)};
        if (t.valid && !t.rgba8.empty()) {
            upload_rgba_texture(t.width, t.height, t.rgba8.data(), scene_textures_[i]);
            if (first_valid < 0) first_valid = static_cast<int>(i);
        }
    }

    if (first_valid < 0) {
        destroy_scene_resources();
        if (error_out) *error_out = "Failed uploading TPL textures.";
        return false;
    }

    const int chosen = (tex_index >= 0 && static_cast<size_t>(tex_index) < scene_textures_.size() &&
                        scene_textures_[static_cast<size_t>(tex_index)].valid)
                           ? tex_index
                           : first_valid;

    const auto& gt = scene_textures_[static_cast<size_t>(chosen)];
    const float w_aspect = gt.height > 0 ? (static_cast<float>(gt.width) / static_cast<float>(gt.height)) : 1.0f;
    const float hx = std::min(1.8f, w_aspect);
    const float hy = hx / std::max(w_aspect, 0.1f);

    std::vector<GxVertex> verts(4);
    verts[0].pos[0] = -hx; verts[0].pos[1] = hy;  verts[0].uv0[0] = 0.0f; verts[0].uv0[1] = 0.0f;
    verts[1].pos[0] = hx;  verts[1].pos[1] = hy;  verts[1].uv0[0] = 1.0f; verts[1].uv0[1] = 0.0f;
    verts[2].pos[0] = hx;  verts[2].pos[1] = -hy; verts[2].uv0[0] = 1.0f; verts[2].uv0[1] = 1.0f;
    verts[3].pos[0] = -hx; verts[3].pos[1] = -hy; verts[3].uv0[0] = 0.0f; verts[3].uv0[1] = 1.0f;
    const std::vector<uint32_t> idxs = {0, 1, 2, 0, 2, 3};

    GpuModel gm{};
    gm.name = "TPL[" + std::to_string(chosen) + "] (" + std::to_string(gt.width) + "x" + std::to_string(gt.height) + ")";
    gm.valid = true;
    gm.bound_radius = std::max(hx, hy) * 1.3f;

    GpuSubMesh sm{};
    create_buffer(verts.size() * sizeof(GxVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sm.vbo, verts.data());
    create_buffer(idxs.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sm.ibo, idxs.data());
    sm.index_count = 6;
    sm.tev = make_default_tev_config(true, false);
    sm.double_sided = true;
    sm.unlit = true;

    VkSampler smp = get_or_create_sampler(GxWrapMode::Clamp, GxWrapMode::Clamp, true);
    sm.mat_desc_set = allocate_material_descriptor(
        {gt.view, white_fallback_.view, white_fallback_.view, white_fallback_.view},
        {smp, smp, smp, smp});
    gm.submeshes.push_back(std::move(sm));
    scene_models_.push_back(std::move(gm));

    model_summaries_.push_back({scene_models_[0].name, true, 1, 2, scene_models_[0].bound_radius});
    total_submeshes_ = 1;
    total_triangles_ = 2;
    scene_label_ = label;
    has_scene_ = true;
    is_tpl_preview_ = true;
    viewer_.preview_tpl_index = chosen;
    viewer_.selected_model_index = 0;
    viewer_.auto_rotate = false;
    viewer_.yaw_deg = 0.0f;
    viewer_.pitch_deg = 0.0f;
    viewer_.target = {0.0f, 0.0f, 0.0f};
    viewer_.distance = 3.2f;
    return true;
}

void GxVulkanBackend::auto_frame_camera() {
    if (scene_models_.empty()) return;

    float cx = 0.0f, cy = 0.0f, cz = 0.0f, r = 1.0f;
    if (viewer_.selected_model_index >= 0 &&
        static_cast<size_t>(viewer_.selected_model_index) < scene_models_.size() &&
        scene_models_[static_cast<size_t>(viewer_.selected_model_index)].valid) {
        const auto& m = scene_models_[static_cast<size_t>(viewer_.selected_model_index)];
        cx = m.bound_center[0]; cy = m.bound_center[1]; cz = m.bound_center[2];
        r = std::max(0.25f, m.bound_radius);
    } else {
        float min_p[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float max_p[3] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        bool any = false;
        for (const auto& m : scene_models_) {
            if (!m.valid || m.submeshes.empty()) continue;
            const float mr = std::max(0.1f, m.bound_radius);
            for (int k = 0; k < 3; ++k) {
                min_p[k] = std::min(min_p[k], m.bound_center[k] - mr);
                max_p[k] = std::max(max_p[k], m.bound_center[k] + mr);
            }
            any = true;
        }
        if (any) {
            cx = 0.5f * (min_p[0] + max_p[0]);
            cy = 0.5f * (min_p[1] + max_p[1]);
            cz = 0.5f * (min_p[2] + max_p[2]);
            const float dx = max_p[0] - cx, dy = max_p[1] - cy, dz = max_p[2] - cz;
            r = std::max(0.25f, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }

    viewer_.target = {cx, cy, cz};
    const float half_fov = (viewer_.fov_y_deg * 0.5f) * (3.14159265f / 180.0f);
    viewer_.distance = std::max(0.5f, (r / std::max(0.15f, std::sin(half_fov))) * 1.15f);
}

void GxVulkanBackend::update_camera(double dt_s,
                                    const PadState& pad1,
                                    float orbit_dx,
                                    float orbit_dy,
                                    float pan_dx,
                                    float pan_dy,
                                    float wheel_delta,
                                    float move_forward,
                                    float move_right,
                                    float move_up) {
    const float dt = static_cast<float>(std::clamp(dt_s, 0.0, 0.1));
    if (viewer_.auto_rotate && !is_tpl_preview_) {
        viewer_.yaw_deg += 22.0f * dt;
    }
    viewer_.yaw_deg += orbit_dx * 0.35f + pad1.norm_substick_x * 110.0f * dt;
    viewer_.pitch_deg += orbit_dy * 0.35f - pad1.norm_substick_y * 85.0f * dt;
    viewer_.pitch_deg = std::clamp(viewer_.pitch_deg, -89.0f, 89.0f);

    const float zoom_trig = (pad1.norm_trigger_l - pad1.norm_trigger_r) * 1.4f * dt;
    const float zoom_scale = std::exp(-wheel_delta * 0.12f + zoom_trig);
    viewer_.distance = std::clamp(viewer_.distance * zoom_scale, 0.25f, 50000.0f);

    const float yaw_rad = viewer_.yaw_deg * (3.14159265f / 180.0f);
    const float fwd_x = -std::sin(yaw_rad), fwd_z = -std::cos(yaw_rad);
    const float right_x = std::cos(yaw_rad), right_z = -std::sin(yaw_rad);
    const float speed = std::max(0.5f, viewer_.distance * 0.85f) * dt;
    const float pan_scale = std::max(0.001f, viewer_.distance * 0.0018f);

    const float net_fwd = move_forward + pad1.norm_stick_y;
    const float net_right = move_right + pad1.norm_stick_x;
    viewer_.target[0] += (fwd_x * net_fwd + right_x * net_right) * speed - right_x * pan_dx * pan_scale;
    viewer_.target[1] += move_up * speed + pan_dy * pan_scale;
    viewer_.target[2] += (fwd_z * net_fwd + right_z * net_right) * speed - right_z * pan_dx * pan_scale;
}

void GxVulkanBackend::populate_live_demo_frame(uint32_t width,
                                               uint32_t height,
                                               float aspect,
                                               double elapsed_seconds,
                                               const PadState& pad1) {
    auto& gx = global_gx_state();
    gx.reset_state();
    gx.begin_frame(width, height, aspect, true);

    const auto proj = gx_make_perspective_mtx(viewer_.fov_y_deg, 4.0f / 3.0f, 0.2f, 200.0f);
    gx.set_projection(proj.data(), false);
    gx.set_fog(viewer_.fog_enabled ? GxFogType::PerspLin : GxFogType::None,
               8.0f, 45.0f, 0.2f, 200.0f,
               viewer_.background_color[0], viewer_.background_color[1], viewer_.background_color[2], 1.0f);

    // Build view-space rotation from camera orbit
    const float yaw = viewer_.yaw_deg * (3.14159265f / 180.0f);
    const float pitch = viewer_.pitch_deg * (3.14159265f / 180.0f);
    float mv[12]{};
    build_rot_trans_3x4(pitch, -yaw, -viewer_.target[0], -viewer_.target[1], -viewer_.distance, mv);
    gx.load_pos_mtx(0, mv);
    gx.set_current_mtx(0);

    // 1. Draw F-Zero style neon cyber grid floor + spinning 3D ring using 2-stage TEV (Tex0 * Ras + Konst glow)
    gx.bind_texture(0, -2); // checker_fallback_
    GxTevConfig floor_tev = make_default_tev_config(true, true);
    floor_tev.num_stages = 2;
    floor_tev.konst_regs[0] = {0.06f, 0.22f, 0.45f, 1.0f};
    floor_tev.stages[1].kcolor_sel = 12;
    floor_tev.stages[1].color_in = {
        static_cast<uint8_t>(GxTevColorArg::CPrev),
        static_cast<uint8_t>(GxTevColorArg::Zero),
        static_cast<uint8_t>(GxTevColorArg::Zero),
        static_cast<uint8_t>(GxTevColorArg::Konst),
    };
    floor_tev.stages[1].alpha_in = {
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::Zero),
        static_cast<uint8_t>(GxTevAlphaArg::APrev),
    };
    gx.set_tev_num_stages(2);
    gx.set_tev_stage(0, floor_tev.stages[0]);
    gx.set_tev_stage(1, floor_tev.stages[1]);
    gx.set_tev_konst(0, 0.06f, 0.22f, 0.45f, 1.0f);
    gx.set_cull_mode(GxCullMode::None);

    gx.begin_primitive(0x80u, true, false); // GX_QUADS
    gx.push_vertex({{-8.0f, -1.6f, -8.0f}, {0.0f, 1.0f, 0.0f}, {0.8f, 0.9f, 1.0f, 1.0f}, {0.0f, 0.0f}, {}});
    gx.push_vertex({{ 8.0f, -1.6f, -8.0f}, {0.0f, 1.0f, 0.0f}, {0.8f, 0.9f, 1.0f, 1.0f}, {8.0f, 0.0f}, {}});
    gx.push_vertex({{ 8.0f, -1.6f,  8.0f}, {0.0f, 1.0f, 0.0f}, {0.8f, 0.9f, 1.0f, 1.0f}, {8.0f, 8.0f}, {}});
    gx.push_vertex({{-8.0f, -1.6f,  8.0f}, {0.0f, 1.0f, 0.0f}, {0.8f, 0.9f, 1.0f, 1.0f}, {0.0f, 8.0f}, {}});
    gx.end_primitive();

    // 2. Spinning 3D F-Zero prism craft in center
    const float t = static_cast<float>(elapsed_seconds);
    float craft_mv[12]{};
    build_rot_trans_3x4(0.15f + pad1.norm_stick_y * 0.25f, t * 1.1f,
                        pad1.norm_stick_x * 1.5f, 0.35f + 0.15f * std::sin(t * 2.5f), -viewer_.distance * 0.82f,
                        craft_mv);
    gx.load_pos_mtx(1, craft_mv);
    gx.set_current_mtx(1);
    gx.set_tev_num_stages(1);
    gx.set_tev_stage(0, make_default_tev_config(false, true).stages[0]);
    gx.begin_primitive(0x90u, true, false); // GX_TRIANGLES
    const GxVertex nose{{0.0f, 0.25f, 2.1f}, {0.0f, 0.4f, 0.9f}, {0.15f, 0.78f, 1.0f, 1.0f}, {}, {}};
    const GxVertex lw{{-1.35f, -0.15f, -1.4f}, {-0.6f, 0.6f, -0.3f}, {0.10f, 0.32f, 0.95f, 1.0f}, {}, {}};
    const GxVertex rw{{ 1.35f, -0.15f, -1.4f}, { 0.6f, 0.6f, -0.3f}, {0.10f, 0.32f, 0.95f, 1.0f}, {}, {}};
    const GxVertex top{{0.0f, 0.65f, -0.8f}, {0.0f, 1.0f, 0.0f}, {0.95f, 0.85f, 0.20f, 1.0f}, {}, {}};
    gx.push_vertex(nose); gx.push_vertex(lw); gx.push_vertex(top);
    gx.push_vertex(nose); gx.push_vertex(top); gx.push_vertex(rw);
    gx.push_vertex(lw); gx.push_vertex(rw); gx.push_vertex(top);
    gx.push_vertex(nose); gx.push_vertex(rw); gx.push_vertex(lw);
    gx.end_primitive();

    // 3. Mid-frame EFB Copy-to-Texture (captures current 3D scene into EFB slot 0x10000!)
    GxEfbCopyCommand efb{};
    efb.target_efb_id = GxStateProcessor::kEfbTexIdBase;
    efb.src_w = width;
    efb.src_h = height;
    efb.dst_w = std::max(64u, width / 2u);
    efb.dst_h = std::max(64u, height / 2u);
    efb.clear_efb = false;
    gx.request_efb_copy(efb);

    // 4. Draw a floating translucent EFB-feedback hologram screen sampling the captured EFB copy!
    gx.load_pos_mtx(0, mv);
    gx.set_current_mtx(0);
    gx.bind_texture(0, static_cast<int32_t>(GxStateProcessor::kEfbTexIdBase));
    gx.set_tev_num_stages(1);
    gx.set_tev_stage(0, make_default_tev_config(true, false).stages[0]);
    gx.set_blend_mode(GxBlendMode::AlphaBlend);
    gx.begin_primitive(0x80u, true, false);
    gx.push_vertex({{-2.1f, 0.6f, -3.2f}, {0.0f, 0.0f, 1.0f}, {0.75f, 0.95f, 1.0f, 0.85f}, {0.0f, 0.0f}, {}});
    gx.push_vertex({{ 2.1f, 0.6f, -3.2f}, {0.0f, 0.0f, 1.0f}, {0.75f, 0.95f, 1.0f, 0.85f}, {1.0f, 0.0f}, {}});
    gx.push_vertex({{ 2.1f, 3.0f, -3.2f}, {0.0f, 0.0f, 1.0f}, {0.75f, 0.95f, 1.0f, 0.85f}, {1.0f, 1.0f}, {}});
    gx.push_vertex({{-2.1f, 3.0f, -3.2f}, {0.0f, 0.0f, 1.0f}, {0.75f, 0.95f, 1.0f, 0.85f}, {0.0f, 1.0f}, {}});
    gx.end_primitive();

    gx.end_frame();
}

void GxVulkanBackend::record_live_frame_commands(VkCommandBuffer cmd,
                                                 uint32_t fi,
                                                 VkImage offscreen_image,
                                                 VkImageView offscreen_view,
                                                 VkImage,
                                                 VkImageView offscreen_depth_view,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 const GxFrameCommands& frame) {
    last_efb_copies_ = frame.efb_copy_count;
    last_max_tev_stages_ = frame.max_tev_stages;

    // Upload dynamic vertices and indices for this frame
    if (!frame.vertices.empty() && !frame.indices.empty()) {
        const VkDeviceSize vb_bytes = std::min(kDynVboBytes, static_cast<VkDeviceSize>(frame.vertices.size() * sizeof(GxVertex)));
        const VkDeviceSize ib_bytes = std::min(kDynIboBytes, static_cast<VkDeviceSize>(frame.indices.size() * sizeof(uint32_t)));
        if (dyn_vbos_[fi].mapped) std::memcpy(dyn_vbos_[fi].mapped, frame.vertices.data(), static_cast<size_t>(vb_bytes));
        if (dyn_ibos_[fi].mapped) std::memcpy(dyn_ibos_[fi].mapped, frame.indices.data(), static_cast<size_t>(ib_bytes));
    }

    auto begin_pass = [&](bool clear_color, bool clear_depth, const std::array<float, 4>& clear_rgba) {
        VkRenderingAttachmentInfo color_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color_att.imageView = offscreen_view;
        color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_att.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.clearValue.color = {{clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]}};

        VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth_att.imageView = offscreen_depth_view;
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_att.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width, height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color_att;
        ri.pDepthAttachment = &depth_att;
        vkCmdBeginRendering(cmd, &ri);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    const std::array<float, 4> bg_clr{
        viewer_.background_color[0], viewer_.background_color[1], viewer_.background_color[2], 1.0f};
    bool pass_open = false;
    begin_pass(true, true, bg_clr);
    pass_open = true;

    VkSampler default_smp = get_or_create_sampler(GxWrapMode::Repeat, GxWrapMode::Repeat, false);
    uint32_t draw_idx = 0;

    for (const auto& item : frame.items) {
        if (item.type == GxCommandItem::Type::EfbCopy) {
            if (pass_open) {
                vkCmdEndRendering(cmd);
                pass_open = false;
            }
            const auto& ec = item.efb_copy;
            if (ensure_efb_texture(ec.target_efb_id, ec.dst_w, ec.dst_h)) {
                auto& dst_tex = efb_textures_[ec.target_efb_id];
                cmd_image_barrier(cmd, offscreen_image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                cmd_image_barrier(cmd, dst_tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  dst_tex.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[0] = {static_cast<int32_t>(ec.src_x), static_cast<int32_t>(ec.src_y), 0};
                blit.srcOffsets[1] = {
                    static_cast<int32_t>(std::min(width, ec.src_x + ec.src_w)),
                    static_cast<int32_t>(std::min(height, ec.src_y + ec.src_h)),
                    1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {static_cast<int32_t>(dst_tex.width), static_cast<int32_t>(dst_tex.height), 1};
                vkCmdBlitImage(cmd, offscreen_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst_tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);

                cmd_image_barrier(cmd, dst_tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                dst_tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                cmd_image_barrier(cmd, offscreen_image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            }
            begin_pass(ec.clear_efb, ec.clear_efb, ec.clear_rgba);
            pass_open = true;
            continue;
        }

        if (draw_idx >= kMaxDrawsPerFrame || frame.vertices.empty() || frame.indices.empty()) {
            continue;
        }
        const auto& d = item.draw;
        const GxCullMode cull = viewer_.backface_culling_enabled ? d.cull : GxCullMode::None;
        VkPipeline pipe = get_or_create_pipeline(cull, d.blend, d.depth_test, d.depth_write, d.depth_func, viewer_.wireframe);
        if (pipe == VK_NULL_HANDLE) continue;

        const uint32_t cam_off = static_cast<uint32_t>(draw_idx * cam_stride_);
        const uint32_t tev_off = static_cast<uint32_t>(draw_idx * tev_stride_);
        if (cam_ubos_[fi].mapped) {
            GxCameraUboGpu cam{};
            std::memcpy(cam.mvp, d.mvp.data(), sizeof(cam.mvp));
            std::memcpy(cam.modelview, d.modelview.data(), sizeof(cam.modelview));
            std::memcpy(cam.normal_mat, d.normal_mat.data(), sizeof(cam.normal_mat));
            std::memcpy(cam.light_dir_intensity, d.light_dir_intensity.data(), sizeof(cam.light_dir_intensity));
            std::memcpy(cam.ambient_color, d.ambient_color.data(), sizeof(cam.ambient_color));
            std::memcpy(cam.mat_color, d.mat_color.data(), sizeof(cam.mat_color));
            cam.draw_flags[0] = d.has_vertex_color ? 1u : 0u;
            cam.draw_flags[1] = d.env_map_uv1 ? 1u : 0u;
            cam.draw_flags[2] = static_cast<uint32_t>(viewer_.shading_mode);
            cam.draw_flags[3] = 0u;
            std::memcpy(static_cast<uint8_t*>(cam_ubos_[fi].mapped) + cam_off, &cam, sizeof(cam));
        }
        if (tev_ubos_[fi].mapped) {
            GxTevUboGpu tev{};
            pack_tev_ubo(d.tev, viewer_.textures_enabled, viewer_.lighting_enabled, viewer_.fog_enabled, tev);
            std::memcpy(static_cast<uint8_t*>(tev_ubos_[fi].mapped) + tev_off, &tev, sizeof(tev));
        }

        // Resolve texture bindings for this draw
        const uint64_t mat_key =
            (static_cast<uint64_t>(static_cast<uint32_t>(d.tex_ids[0])) & 0xFFFFFFFFull) |
            (static_cast<uint64_t>(static_cast<uint32_t>(d.tex_ids[1])) << 32);
        VkDescriptorSet mat_set = VK_NULL_HANDLE;
        auto mit = live_mat_cache_.find(mat_key);
        if (mit != live_mat_cache_.end()) {
            mat_set = mit->second;
        } else {
            VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsai.descriptorPool = live_mat_desc_pool_;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &mat_desc_layout_;
            if (vkAllocateDescriptorSets(device_, &dsai, &mat_set) == VK_SUCCESS) {
                auto resolve_view = [&](int32_t id) -> VkImageView {
                    if (id == -2 && checker_fallback_.valid) return checker_fallback_.view;
                    if (id >= static_cast<int32_t>(GxStateProcessor::kEfbTexIdBase)) {
                        auto eit = efb_textures_.find(static_cast<uint32_t>(id));
                        if (eit != efb_textures_.end() && eit->second.valid) return eit->second.view;
                    }
                    if (id >= 0 && static_cast<size_t>(id) < scene_textures_.size() &&
                        scene_textures_[static_cast<size_t>(id)].valid) {
                        return scene_textures_[static_cast<size_t>(id)].view;
                    }
                    return white_fallback_.view;
                };
                VkDescriptorImageInfo imgs[4]{};
                for (uint32_t t = 0; t < 4; ++t) {
                    imgs[t] = {default_smp, resolve_view(d.tex_ids[t]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                }
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = mat_set;
                w.dstBinding = 0;
                w.descriptorCount = 4;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = imgs;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
                live_mat_cache_[mat_key] = mat_set;
            }
        }

        if (mat_set != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            const uint32_t dyn_offs[2] = {cam_off, tev_off};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    0, 1, &ubo_desc_sets_[fi], 2, dyn_offs);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    1, 1, &mat_set, 0, nullptr);
            VkDeviceSize zero_off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &dyn_vbos_[fi].buffer, &zero_off);
            vkCmdBindIndexBuffer(cmd, dyn_ibos_[fi].buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
            ++draw_idx;
        }
    }

    if (pass_open) {
        vkCmdEndRendering(cmd);
    }
}

void GxVulkanBackend::record_gma_frame_commands(VkCommandBuffer cmd,
                                                uint32_t fi,
                                                VkImage,
                                                VkImageView offscreen_view,
                                                VkImage,
                                                VkImageView offscreen_depth_view,
                                                uint32_t width,
                                                uint32_t height,
                                                float aspect_ratio) {
    last_efb_copies_ = 0;
    const float yaw_rad = viewer_.yaw_deg * (3.14159265f / 180.0f);
    const float pitch_rad = viewer_.pitch_deg * (3.14159265f / 180.0f);
    const float cp = std::cos(pitch_rad);
    const std::array<float, 3> eye{
        viewer_.target[0] + viewer_.distance * cp * std::sin(yaw_rad),
        viewer_.target[1] + viewer_.distance * std::sin(pitch_rad),
        viewer_.target[2] + viewer_.distance * cp * std::cos(yaw_rad),
    };

    const Mat4 view = mat4_look_at(eye, viewer_.target, {0.0f, 1.0f, 0.0f});
    const float znear = std::max(0.05f, viewer_.distance * 0.002f);
    const float zfar = std::max(1000.0f, viewer_.distance * 35.0f);
    const Mat4 proj = mat4_perspective_vulkan(
        viewer_.fov_y_deg * (3.14159265f / 180.0f), aspect_ratio, znear, zfar);
    const Mat4 mvp = mat4_mul(proj, view);

    VkRenderingAttachmentInfo off_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    off_att.imageView = offscreen_view;
    off_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    off_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    off_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    off_att.clearValue.color = {
        {viewer_.background_color[0], viewer_.background_color[1], viewer_.background_color[2], 1.0f}};

    VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_att.imageView = offscreen_depth_view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo off_ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    off_ri.renderArea = {{0, 0}, {width, height}};
    off_ri.layerCount = 1;
    off_ri.colorAttachmentCount = 1;
    off_ri.pColorAttachments = &off_att;
    off_ri.pDepthAttachment = &depth_att;
    vkCmdBeginRendering(cmd, &off_ri);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    uint32_t draw_idx = 0;
    auto draw_pass = [&](bool transparent_pass) {
        for (size_t mi = 0; mi < scene_models_.size(); ++mi) {
            if (viewer_.selected_model_index >= 0 &&
                static_cast<size_t>(viewer_.selected_model_index) != mi) {
                continue;
            }
            const auto& m = scene_models_[mi];
            if (!m.valid) continue;

            for (const auto& sm : m.submeshes) {
                if (draw_idx >= kMaxDrawsPerFrame) return;
                if (sm.is_transparent != transparent_pass) continue;
                if (transparent_pass && !viewer_.transparent_pass_enabled) continue;
                if (sm.vbo.buffer == VK_NULL_HANDLE || sm.ibo.buffer == VK_NULL_HANDLE) continue;

                const GxCullMode cull = (!viewer_.backface_culling_enabled || sm.double_sided)
                                            ? GxCullMode::None
                                            : GxCullMode::Back;
                VkPipeline pipe = get_or_create_pipeline(
                    cull, sm.blend_mode, true, true, GxCompare::LEqual, viewer_.wireframe);
                if (pipe == VK_NULL_HANDLE) continue;

                const uint32_t cam_off = static_cast<uint32_t>(draw_idx * cam_stride_);
                const uint32_t tev_off = static_cast<uint32_t>(draw_idx * tev_stride_);

                if (cam_ubos_[fi].mapped) {
                    GxCameraUboGpu cam{};
                    std::memcpy(cam.mvp, mvp.m, sizeof(cam.mvp));
                    std::memcpy(cam.modelview, view.m, sizeof(cam.modelview));
                    std::memcpy(cam.normal_mat, view.m, sizeof(cam.normal_mat));
                    cam.light_dir_intensity[0] = 0.45f;
                    cam.light_dir_intensity[1] = 0.78f;
                    cam.light_dir_intensity[2] = 0.42f;
                    cam.light_dir_intensity[3] = viewer_.lighting_enabled ? 1.0f : 0.0f;
                    cam.ambient_color[0] = 0.42f;
                    cam.ambient_color[1] = 0.44f;
                    cam.ambient_color[2] = 0.50f;
                    cam.ambient_color[3] = 1.0f;
                    std::memcpy(cam.mat_color, sm.material_rgba.data(), sizeof(cam.mat_color));
                    cam.draw_flags[0] = sm.has_vertex_color ? 1u : 0u;
                    cam.draw_flags[1] = sm.env_map_uv1 ? 1u : 0u;
                    cam.draw_flags[2] = static_cast<uint32_t>(viewer_.shading_mode);
                    cam.draw_flags[3] = is_tpl_preview_ ? 1u : 0u;
                    std::memcpy(static_cast<uint8_t*>(cam_ubos_[fi].mapped) + cam_off, &cam, sizeof(cam));
                }

                if (tev_ubos_[fi].mapped) {
                    GxTevUboGpu tev{};
                    GxTevConfig cfg = sm.tev;
                    if (viewer_.fog_enabled && !is_tpl_preview_) {
                        cfg.enable_fog = false;
                    }
                    pack_tev_ubo(cfg, viewer_.textures_enabled,
                                 viewer_.lighting_enabled && !sm.unlit, viewer_.fog_enabled, tev);
                    std::memcpy(static_cast<uint8_t*>(tev_ubos_[fi].mapped) + tev_off, &tev, sizeof(tev));
                }

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                const uint32_t dyn_offs[2] = {cam_off, tev_off};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                        0, 1, &ubo_desc_sets_[fi], 2, dyn_offs);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                        1, 1, &sm.mat_desc_set, 0, nullptr);

                VkDeviceSize zero_off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &sm.vbo.buffer, &zero_off);
                vkCmdBindIndexBuffer(cmd, sm.ibo.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, sm.index_count, 1, 0, 0, 0);
                ++draw_idx;
            }
        }
    };

    draw_pass(false);
    draw_pass(true);
    vkCmdEndRendering(cmd);
}

void GxVulkanBackend::record_frame_to_offscreen(VkCommandBuffer cmd,
                                                uint32_t frame_index,
                                                VkImage offscreen_image,
                                                VkImageView offscreen_view,
                                                VkImage offscreen_depth_image,
                                                VkImageView offscreen_depth_view,
                                                uint32_t width,
                                                uint32_t height,
                                                float aspect_ratio,
                                                double elapsed_seconds,
                                                const PadState& pad1) {
    if (!has_scene() || width == 0 || height == 0) {
        return;
    }

    cmd_image_barrier(cmd, offscreen_image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    cmd_image_barrier(cmd, offscreen_depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const uint32_t fi = frame_index % kFramesInFlight;
    if (live_demo_active_ || external_live_active_) {
        if (live_demo_active_) {
            populate_live_demo_frame(width, height, aspect_ratio, elapsed_seconds, pad1);
        }
        record_live_frame_commands(
            cmd, fi, offscreen_image, offscreen_view, offscreen_depth_image, offscreen_depth_view,
            width, height, global_gx_state().completed_frame());
    } else {
        record_gma_frame_commands(
            cmd, fi, offscreen_image, offscreen_view, offscreen_depth_image, offscreen_depth_view,
            width, height, aspect_ratio);
    }

    cmd_image_barrier(cmd, offscreen_image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace fzgx
