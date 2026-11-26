#include "assets/gx_texture.hpp"
#include "assets/avlz.hpp"
#include "assets/byte_reader.hpp"

#include <algorithm>
#include <array>

namespace fzgx {

namespace {

inline uint8_t expand_5_to_8(uint32_t v) {
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

inline uint8_t expand_6_to_8(uint32_t v) {
    return static_cast<uint8_t>((v << 2) | (v >> 4));
}

inline uint8_t expand_3_to_8(uint32_t v) {
    return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1));
}

inline void decode_rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = expand_5_to_8((c >> 11) & 0x1Fu);
    g = expand_6_to_8((c >> 5) & 0x3Fu);
    b = expand_5_to_8(c & 0x1Fu);
}

inline void put_px(std::vector<uint8_t>& out, uint32_t w, uint32_t h,
                   uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= w || y >= h) return;
    const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
    out[idx + 0] = r;
    out[idx + 1] = g;
    out[idx + 2] = b;
    out[idx + 3] = a;
}

uint32_t align_up(uint32_t v, uint32_t align) {
    return ((v + align - 1u) / align) * align;
}

GxTexFmt parse_gx_tex_fmt(uint32_t raw) {
    switch (raw & 0x1Fu) {
    case 0x0: return GxTexFmt::I4;
    case 0x1: return GxTexFmt::I8;
    case 0x2: return GxTexFmt::IA4;
    case 0x3: return GxTexFmt::IA8;
    case 0x4: return GxTexFmt::RGB565;
    case 0x5: return GxTexFmt::RGB5A3;
    case 0x6: return GxTexFmt::RGBA8;
    case 0x8: return GxTexFmt::C4;
    case 0x9: return GxTexFmt::C8;
    case 0xA: return GxTexFmt::C14X2;
    case 0xE: return GxTexFmt::CMPR;
    default: return GxTexFmt::Unknown;
    }
}

void decode_cmpr_subblock(const uint8_t* blk, uint32_t w, uint32_t h,
                          uint32_t bx, uint32_t by, std::vector<uint8_t>& out) {
    const uint16_t c0 = static_cast<uint16_t>((static_cast<uint16_t>(blk[0]) << 8) | blk[1]);
    const uint16_t c1 = static_cast<uint16_t>((static_cast<uint16_t>(blk[2]) << 8) | blk[3]);

    std::array<std::array<uint8_t, 4>, 4> pal{};
    decode_rgb565(c0, pal[0][0], pal[0][1], pal[0][2]);
    pal[0][3] = 255;
    decode_rgb565(c1, pal[1][0], pal[1][1], pal[1][2]);
    pal[1][3] = 255;

    if (c0 > c1) {
        for (int c = 0; c < 3; ++c) {
            pal[2][c] = static_cast<uint8_t>((2 * pal[0][c] + pal[1][c]) / 3);
            pal[3][c] = static_cast<uint8_t>((pal[0][c] + 2 * pal[1][c]) / 3);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int c = 0; c < 3; ++c) {
            pal[2][c] = static_cast<uint8_t>((pal[0][c] + pal[1][c]) / 2);
        }
        pal[2][3] = 255;
        pal[3] = {0, 0, 0, 0};
    }

    for (uint32_t row = 0; row < 4; ++row) {
        const uint8_t bits = blk[4 + row];
        for (uint32_t col = 0; col < 4; ++col) {
            // GameCube CMPR uses MSB-first 2-bit indices
            const uint32_t code = (bits >> (6u - col * 2u)) & 3u;
            const auto& p = pal[code];
            put_px(out, w, h, bx + col, by + row, p[0], p[1], p[2], p[3]);
        }
    }
}

} // namespace

const char* gx_tex_fmt_name(GxTexFmt fmt) {
    switch (fmt) {
    case GxTexFmt::I4: return "I4";
    case GxTexFmt::I8: return "I8";
    case GxTexFmt::IA4: return "IA4";
    case GxTexFmt::IA8: return "IA8";
    case GxTexFmt::RGB565: return "RGB565";
    case GxTexFmt::RGB5A3: return "RGB5A3";
    case GxTexFmt::RGBA8: return "RGBA8";
    case GxTexFmt::C4: return "C4";
    case GxTexFmt::C8: return "C8";
    case GxTexFmt::C14X2: return "C14X2";
    case GxTexFmt::CMPR: return "CMPR";
    default: return "Unknown";
    }
}

size_t gx_texture_mip0_bytes(uint32_t width, uint32_t height, GxTexFmt fmt) {
    if (width == 0 || height == 0) return 0;
    switch (fmt) {
    case GxTexFmt::I4:
    case GxTexFmt::C4:
    case GxTexFmt::CMPR:
        return (static_cast<size_t>(align_up(width, 8)) * align_up(height, 8)) / 2u;
    case GxTexFmt::I8:
    case GxTexFmt::IA4:
    case GxTexFmt::C8:
        return static_cast<size_t>(align_up(width, 8)) * align_up(height, 4);
    case GxTexFmt::IA8:
    case GxTexFmt::RGB565:
    case GxTexFmt::RGB5A3:
    case GxTexFmt::C14X2:
        return static_cast<size_t>(align_up(width, 4)) * align_up(height, 4) * 2u;
    case GxTexFmt::RGBA8:
        return static_cast<size_t>(align_up(width, 4)) * align_up(height, 4) * 4u;
    default:
        return 0;
    }
}

bool decode_gx_texture(std::span<const uint8_t> src,
                       uint32_t width,
                       uint32_t height,
                       GxTexFmt fmt,
                       std::vector<uint8_t>& out_rgba8,
                       std::string* error_out) {
    out_rgba8.clear();
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        if (error_out) *error_out = "Invalid GX texture dimensions.";
        return false;
    }
    const size_t needed = gx_texture_mip0_bytes(width, height, fmt);
    if (needed == 0) {
        if (error_out) *error_out = std::string("Unsupported GX texture format: ") + gx_tex_fmt_name(fmt);
        return false;
    }
    if (src.size() < needed) {
        if (error_out) {
            *error_out = "Truncated GX texture payload for format " + std::string(gx_tex_fmt_name(fmt)) +
                         " (need " + std::to_string(needed) + ", have " + std::to_string(src.size()) + ").";
        }
        return false;
    }

    out_rgba8.assign(static_cast<size_t>(width) * height * 4u, 0);

    switch (fmt) {
    case GxTexFmt::I4: {
        const uint32_t tw = align_up(width, 8);
        const uint32_t th = align_up(height, 8);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 8) {
            for (uint32_t tx = 0; tx < tw; tx += 8) {
                for (uint32_t y = 0; y < 8; ++y) {
                    for (uint32_t x = 0; x < 8; x += 2) {
                        const uint8_t byte = src[pos++];
                        const uint8_t i0 = static_cast<uint8_t>((byte >> 4) * 0x11u);
                        const uint8_t i1 = static_cast<uint8_t>((byte & 0xFu) * 0x11u);
                        put_px(out_rgba8, width, height, tx + x, ty + y, i0, i0, i0, 255);
                        put_px(out_rgba8, width, height, tx + x + 1, ty + y, i1, i1, i1, 255);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::I8: {
        const uint32_t tw = align_up(width, 8);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 8) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        const uint8_t v = src[pos++];
                        put_px(out_rgba8, width, height, tx + x, ty + y, v, v, v, 255);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::IA4: {
        const uint32_t tw = align_up(width, 8);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 8) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        const uint8_t byte = src[pos++];
                        const uint8_t a = static_cast<uint8_t>((byte >> 4) * 0x11u);
                        const uint8_t i = static_cast<uint8_t>((byte & 0xFu) * 0x11u);
                        put_px(out_rgba8, width, height, tx + x, ty + y, i, i, i, a);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::IA8: {
        const uint32_t tw = align_up(width, 4);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 4) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint8_t a = src[pos++];
                        const uint8_t i = src[pos++];
                        put_px(out_rgba8, width, height, tx + x, ty + y, i, i, i, a);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::RGB565: {
        const uint32_t tw = align_up(width, 4);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 4) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint16_t c = static_cast<uint16_t>((static_cast<uint16_t>(src[pos]) << 8) | src[pos + 1]);
                        pos += 2;
                        uint8_t r = 0, g = 0, b = 0;
                        decode_rgb565(c, r, g, b);
                        put_px(out_rgba8, width, height, tx + x, ty + y, r, g, b, 255);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::RGB5A3: {
        const uint32_t tw = align_up(width, 4);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 4) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint16_t c = static_cast<uint16_t>((static_cast<uint16_t>(src[pos]) << 8) | src[pos + 1]);
                        pos += 2;
                        uint8_t r = 0, g = 0, b = 0, a = 255;
                        if ((c & 0x8000u) != 0) {
                            r = expand_5_to_8((c >> 10) & 0x1Fu);
                            g = expand_5_to_8((c >> 5) & 0x1Fu);
                            b = expand_5_to_8(c & 0x1Fu);
                        } else {
                            a = expand_3_to_8((c >> 12) & 0x7u);
                            r = static_cast<uint8_t>(((c >> 8) & 0xFu) * 0x11u);
                            g = static_cast<uint8_t>(((c >> 4) & 0xFu) * 0x11u);
                            b = static_cast<uint8_t>((c & 0xFu) * 0x11u);
                        }
                        put_px(out_rgba8, width, height, tx + x, ty + y, r, g, b, a);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::RGBA8: {
        const uint32_t tw = align_up(width, 4);
        const uint32_t th = align_up(height, 4);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 4) {
            for (uint32_t tx = 0; tx < tw; tx += 4) {
                // First 32 bytes: 16 (A, R) pairs; next 32 bytes: 16 (G, B) pairs
                const uint8_t* ar = src.data() + pos;
                const uint8_t* gb = src.data() + pos + 32;
                pos += 64;
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        const size_t p = (y * 4u + x) * 2u;
                        put_px(out_rgba8, width, height, tx + x, ty + y,
                               ar[p + 1], gb[p + 0], gb[p + 1], ar[p + 0]);
                    }
                }
            }
        }
        return true;
    }
    case GxTexFmt::CMPR: {
        const uint32_t tw = align_up(width, 8);
        const uint32_t th = align_up(height, 8);
        size_t pos = 0;
        for (uint32_t ty = 0; ty < th; ty += 8) {
            for (uint32_t tx = 0; tx < tw; tx += 8) {
                decode_cmpr_subblock(src.data() + pos + 0, width, height, tx + 0, ty + 0, out_rgba8);
                decode_cmpr_subblock(src.data() + pos + 8, width, height, tx + 4, ty + 0, out_rgba8);
                decode_cmpr_subblock(src.data() + pos + 16, width, height, tx + 0, ty + 4, out_rgba8);
                decode_cmpr_subblock(src.data() + pos + 24, width, height, tx + 4, ty + 4, out_rgba8);
                pos += 32;
            }
        }
        return true;
    }
    default:
        if (error_out) {
            *error_out = std::string("Indexed palette GX texture format unsupported without TLUT: ") +
                         gx_tex_fmt_name(fmt);
        }
        out_rgba8.clear();
        return false;
    }
}

void TplArchive::clear() {
    textures_.clear();
    is_nintendo_tpl_ = false;
    loaded_ = false;
}

size_t TplArchive::valid_texture_count() const {
    return static_cast<size_t>(std::count_if(textures_.begin(), textures_.end(),
                                              [](const TplTexture& t) { return t.valid; }));
}

bool TplArchive::load(std::span<const uint8_t> raw_bytes,
                      bool auto_decompress_lz,
                      std::string* error_out) {
    clear();
    if (raw_bytes.size() < 4) {
        if (error_out) *error_out = "Empty or truncated TPL buffer.";
        return false;
    }

    std::vector<uint8_t> decompressed;
    std::span<const uint8_t> data = raw_bytes;
    if (auto_decompress_lz && ByteReader::peek_be32(raw_bytes, 0) != kNintendoTplMagic && is_avlz_buffer(raw_bytes)) {
        if (!decompress_avlz(raw_bytes, decompressed, error_out)) {
            return false;
        }
        data = decompressed;
    }

    if (data.size() < 4) {
        if (error_out) *error_out = "TPL buffer too small after decompression.";
        return false;
    }

    if (ByteReader::peek_be32(data, 0) == kNintendoTplMagic) {
        return load_nintendo_tpl(data, error_out);
    }
    return load_av_tpl(data, error_out);
}

bool TplArchive::load_av_tpl(std::span<const uint8_t> data, std::string* error_out) {
    const uint32_t count = ByteReader::peek_be32(data, 0);
    if (count > 16384u || 4ull + static_cast<uint64_t>(count) * 16ull > data.size()) {
        if (error_out) *error_out = "Invalid Amusement Vision TPL texture count or truncated header.";
        clear();
        return false;
    }

    textures_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry_off = 4u + static_cast<size_t>(i) * 16u;
        const uint32_t flags = ByteReader::peek_be32(data, entry_off + 0x00);
        const uint32_t data_off = ByteReader::peek_be32(data, entry_off + 0x04);
        const uint16_t width = ByteReader::peek_be16(data, entry_off + 0x08);
        const uint16_t height = ByteReader::peek_be16(data, entry_off + 0x0A);
        const uint16_t mips = ByteReader::peek_be16(data, entry_off + 0x0C);

        TplTexture& tex = textures_[i];
        tex.raw_flags = flags;
        tex.width = width;
        tex.height = height;
        tex.mip_levels = mips;
        tex.format = parse_gx_tex_fmt(flags);

        if ((flags & 0x100u) != 0 || width == 0 || height == 0 || data_off >= data.size()) {
            tex.valid = false;
            continue;
        }

        std::string dec_err;
        if (decode_gx_texture(data.subspan(data_off), width, height, tex.format, tex.rgba8, &dec_err)) {
            tex.valid = true;
        } else {
            tex.valid = false;
        }
    }

    is_nintendo_tpl_ = false;
    loaded_ = true;
    return true;
}

bool TplArchive::load_nintendo_tpl(std::span<const uint8_t> data, std::string* error_out) {
    if (data.size() < 12) {
        if (error_out) *error_out = "Truncated Nintendo TPL header.";
        return false;
    }
    const uint32_t count = ByteReader::peek_be32(data, 4);
    const uint32_t tbl_off = ByteReader::peek_be32(data, 8);
    if (count > 16384u || static_cast<uint64_t>(tbl_off) + static_cast<uint64_t>(count) * 8ull > data.size()) {
        if (error_out) *error_out = "Invalid Nintendo TPL offset table.";
        clear();
        return false;
    }

    textures_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t img_hdr_off = ByteReader::peek_be32(data, tbl_off + static_cast<size_t>(i) * 8u);
        if (img_hdr_off == 0 || static_cast<uint64_t>(img_hdr_off) + 36ull > data.size()) {
            continue;
        }
        const uint16_t height = ByteReader::peek_be16(data, img_hdr_off + 0x00);
        const uint16_t width = ByteReader::peek_be16(data, img_hdr_off + 0x02);
        const uint32_t fmt_raw = ByteReader::peek_be32(data, img_hdr_off + 0x04);
        const uint32_t data_off = ByteReader::peek_be32(data, img_hdr_off + 0x08);

        TplTexture& tex = textures_[i];
        tex.width = width;
        tex.height = height;
        tex.format = parse_gx_tex_fmt(fmt_raw);
        tex.raw_flags = fmt_raw;
        if (data_off < data.size() &&
            decode_gx_texture(data.subspan(data_off), width, height, tex.format, tex.rgba8)) {
            tex.valid = true;
        }
    }

    is_nintendo_tpl_ = true;
    loaded_ = true;
    return true;
}

} // namespace fzgx
