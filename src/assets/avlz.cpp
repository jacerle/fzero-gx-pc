#include "assets/avlz.hpp"
#include "assets/byte_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace fzgx {

namespace {

constexpr uint32_t kRingSize = 0x1000u;
constexpr uint32_t kRingStart = 0xFEEu;
constexpr uint32_t kMinMatch = 3u;
constexpr uint32_t kMaxUncompressedBytes = 256u * 1024u * 1024u;

bool has_lz_extension(const std::string& s) {
    if (s.size() < 3) return false;
    return std::tolower(static_cast<unsigned char>(s[s.size() - 3])) == '.' &&
           std::tolower(static_cast<unsigned char>(s[s.size() - 2])) == 'l' &&
           std::tolower(static_cast<unsigned char>(s[s.size() - 1])) == 'z';
}

} // namespace

bool is_avlz_buffer(std::span<const uint8_t> data) {
    if (data.size() < 8) {
        return false;
    }
    const uint32_t comp_size = ByteReader::peek_le32(data, 0);
    const uint32_t uncomp_size = ByteReader::peek_le32(data, 4);
    if (comp_size < 2 || uncomp_size == 0 || uncomp_size > kMaxUncompressedBytes) {
        return false;
    }
    if (static_cast<uint64_t>(comp_size) + 8ull > data.size()) {
        return false;
    }
    // Heuristic: padded to 32 bytes or exact size; avoid mistaking U8/GCMF/TPL big-endian headers
    const uint32_t be_magic = ByteReader::peek_be32(data, 0);
    if (be_magic == 0x55AA382Du || be_magic == 0x47434D46u || be_magic == 0x0020AF30u) {
        return false;
    }
    return data.size() - (static_cast<size_t>(comp_size) + 8) <= 64;
}

bool decompress_avlz(std::span<const uint8_t> src,
                     std::vector<uint8_t>& out,
                     std::string* error_out) {
    out.clear();
    if (src.size() < 8) {
        if (error_out) *error_out = "AV-LZSS buffer too small (< 8 bytes).";
        return false;
    }

    const uint32_t comp_size = ByteReader::peek_le32(src, 0);
    const uint32_t uncomp_size = ByteReader::peek_le32(src, 4);
    if (uncomp_size > kMaxUncompressedBytes) {
        if (error_out) *error_out = "AV-LZSS uncompressed size exceeds 256 MiB limit.";
        return false;
    }
    const uint64_t end_u64 = static_cast<uint64_t>(comp_size) + 8ull;
    if (end_u64 > src.size()) {
        if (error_out) {
            *error_out = "AV-LZSS truncated: header requires " + std::to_string(end_u64) +
                         " bytes, have " + std::to_string(src.size()) + ".";
        }
        return false;
    }

    const size_t end = static_cast<size_t>(end_u64);
    std::array<uint8_t, kRingSize> ring{};
    uint32_t pos = kRingStart;
    out.reserve(uncomp_size);

    size_t cursor = 8;
    uint32_t flags = 0;

    while (true) {
        flags >>= 1;
        if ((flags & 0x100u) == 0) {
            if (cursor >= end) {
                break;
            }
            flags = static_cast<uint32_t>(src[cursor++]) | 0xFF00u;
        }

        if ((flags & 1u) != 0) {
            if (cursor >= end) {
                break;
            }
            const uint8_t b = src[cursor++];
            out.push_back(b);
            ring[pos] = b;
            pos = (pos + 1u) & 0xFFFu;
        } else {
            if (cursor + 1 >= end) {
                break;
            }
            const uint8_t b0 = src[cursor++];
            const uint8_t b1 = src[cursor++];
            const uint32_t off = static_cast<uint32_t>(b0) | ((static_cast<uint32_t>(b1) << 4) & 0xF00u);
            const uint32_t n = (static_cast<uint32_t>(b1) & 0xFu) + kMinMatch;
            for (uint32_t k = 0; k < n; ++k) {
                const uint8_t b = ring[(off + k) & 0xFFFu];
                out.push_back(b);
                ring[pos] = b;
                pos = (pos + 1u) & 0xFFFu;
            }
        }
    }

    if (out.size() != uncomp_size) {
        if (error_out) {
            *error_out = "AV-LZSS size mismatch: header declared " + std::to_string(uncomp_size) +
                         " bytes, decoded " + std::to_string(out.size()) + ".";
        }
        out.clear();
        return false;
    }
    return true;
}

bool maybe_decompress_avlz(std::span<const uint8_t> src,
                           const std::string& name_hint,
                           std::vector<uint8_t>& out,
                           std::string* error_out) {
    if (has_lz_extension(name_hint) || is_avlz_buffer(src)) {
        return decompress_avlz(src, out, error_out);
    }
    out.assign(src.begin(), src.end());
    return true;
}

} // namespace fzgx
