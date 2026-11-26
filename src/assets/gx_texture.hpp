#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fzgx {

enum class GxTexFmt : uint8_t {
    I4 = 0x0,
    I8 = 0x1,
    IA4 = 0x2,
    IA8 = 0x3,
    RGB565 = 0x4,
    RGB5A3 = 0x5,
    RGBA8 = 0x6,
    C4 = 0x8,
    C8 = 0x9,
    C14X2 = 0xA,
    CMPR = 0xE,
    Unknown = 0xFF,
};

const char* gx_tex_fmt_name(GxTexFmt fmt);

// Returns expected byte size of mip 0 for standard GameCube tiled texture formats.
size_t gx_texture_mip0_bytes(uint32_t width, uint32_t height, GxTexFmt fmt);

// Decodes GameCube tiled texture (mip 0) into tightly packed RGBA8 (width * height * 4 bytes).
bool decode_gx_texture(std::span<const uint8_t> src,
                       uint32_t width,
                       uint32_t height,
                       GxTexFmt fmt,
                       std::vector<uint8_t>& out_rgba8,
                       std::string* error_out = nullptr);

struct TplTexture {
    bool valid{false};
    uint16_t width{0};
    uint16_t height{0};
    uint16_t mip_levels{1};
    GxTexFmt format{GxTexFmt::Unknown};
    uint32_t raw_flags{0};
    std::vector<uint8_t> rgba8; // width * height * 4
};

class TplArchive {
public:
    static constexpr uint32_t kNintendoTplMagic = 0x0020AF30u;

    bool load(std::span<const uint8_t> raw_bytes,
              bool auto_decompress_lz = true,
              std::string* error_out = nullptr);
    void clear();

    bool is_loaded() const { return !textures_.empty() || loaded_; }
    bool is_nintendo_tpl() const { return is_nintendo_tpl_; }
    const std::vector<TplTexture>& textures() const { return textures_; }
    size_t valid_texture_count() const;

private:
    bool load_av_tpl(std::span<const uint8_t> data, std::string* error_out);
    bool load_nintendo_tpl(std::span<const uint8_t> data, std::string* error_out);

    std::vector<TplTexture> textures_;
    bool is_nintendo_tpl_{false};
    bool loaded_{false};
};

} // namespace fzgx
