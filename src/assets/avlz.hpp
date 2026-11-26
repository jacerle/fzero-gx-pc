#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fzgx {

// Amusement Vision LZSS (.lz) header check:
// 8-byte little-endian header: [comp_size, uncomp_size] where comp_size + 8 <= data.size().
bool is_avlz_buffer(std::span<const uint8_t> data);

// Decompresses Amusement Vision .lz (4 KiB ring buffer, start=0xFEE, match=3..18).
bool decompress_avlz(std::span<const uint8_t> src,
                     std::vector<uint8_t>& out,
                     std::string* error_out = nullptr);

// If `src` has an `.lz` extension hint or looks like an AV-LZSS stream, decompresses into `out`;
// otherwise copies `src` into `out`.
bool maybe_decompress_avlz(std::span<const uint8_t> src,
                           const std::string& name_hint,
                           std::vector<uint8_t>& out,
                           std::string* error_out = nullptr);

} // namespace fzgx
