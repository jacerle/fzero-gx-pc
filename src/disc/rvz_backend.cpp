#include "disc/rvz_backend.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>

namespace fzgx {

namespace {

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | static_cast<uint64_t>(read_be32(p + 4));
}

const char* compression_label(uint32_t c) {
    switch (c) {
    case 0: return "None";
    case 1: return "Purge";
    case 2: return "Bzip2";
    case 3: return "LZMA";
    case 4: return "LZMA2";
    case 5: return "Zstandard";
    default: return "Unknown";
    }
}

} // namespace

void RvzPrng::seed(const uint8_t seed_bytes[68]) {
    for (size_t i = 0; i < 17; ++i) {
        buffer_[i] = read_be32(seed_bytes + i * 4);
    }
    for (size_t i = 17; i < kBufferWords; ++i) {
        buffer_[i] = (buffer_[i - 17] << 23) ^ (buffer_[i - 16] >> 9) ^ buffer_[i - 1];
    }
    for (int round = 0; round < 4; ++round) {
        advance_buffer();
    }
    word_index_ = 0;
    byte_in_word_ = 4;
}

void RvzPrng::advance_buffer() {
    for (size_t i = 0; i < 32; ++i) {
        buffer_[i] ^= buffer_[i + kBufferWords - 32];
    }
    for (size_t i = 32; i < kBufferWords; ++i) {
        buffer_[i] ^= buffer_[i - 32];
    }
}

uint8_t RvzPrng::next_byte() {
    if (byte_in_word_ >= 4) {
        if (word_index_ >= kBufferWords) {
            advance_buffer();
            word_index_ = 0;
        }
        current_word_ = buffer_[word_index_++];
        byte_in_word_ = 0;
    }
    const int idx = byte_in_word_++;
    switch (idx) {
    case 0: return static_cast<uint8_t>((current_word_ >> 24) & 0xFFU);
    case 1: return static_cast<uint8_t>((current_word_ >> 18) & 0xFFU); // Spec: shift 18, not 16
    case 2: return static_cast<uint8_t>((current_word_ >> 8) & 0xFFU);
    default: return static_cast<uint8_t>(current_word_ & 0xFFU);
    }
}

void RvzPrng::skip_bytes(uint64_t count) {
    while (count > 0 && byte_in_word_ < 4) {
        (void)next_byte();
        --count;
    }
    while (count >= 4) {
        if (word_index_ >= kBufferWords) {
            advance_buffer();
            word_index_ = 0;
        }
        const size_t avail_words = kBufferWords - word_index_;
        const size_t skip_words = static_cast<size_t>(std::min<uint64_t>(avail_words, count / 4));
        word_index_ += skip_words;
        count -= static_cast<uint64_t>(skip_words) * 4;
    }
    while (count > 0) {
        (void)next_byte();
        --count;
    }
}

void RvzPrng::generate_bytes(uint8_t* dst, size_t count) {
    size_t pos = 0;
    while (pos < count && byte_in_word_ < 4) {
        dst[pos++] = next_byte();
    }
    while (pos + 4 <= count) {
        if (word_index_ >= kBufferWords) {
            advance_buffer();
            word_index_ = 0;
        }
        const uint32_t w = buffer_[word_index_++];
        dst[pos + 0] = static_cast<uint8_t>((w >> 24) & 0xFFU);
        dst[pos + 1] = static_cast<uint8_t>((w >> 18) & 0xFFU);
        dst[pos + 2] = static_cast<uint8_t>((w >> 8) & 0xFFU);
        dst[pos + 3] = static_cast<uint8_t>(w & 0xFFU);
        pos += 4;
    }
    while (pos < count) {
        dst[pos++] = next_byte();
    }
}

bool RvzDiscBackend::unpack_rvz_data(const uint8_t* src,
                                     size_t src_size,
                                     uint64_t chunk_disc_offset,
                                     uint8_t* dst,
                                     size_t expected_dst_size,
                                     std::string* error_out) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    uint64_t cur_offset = chunk_disc_offset;

    while (in_pos < src_size && out_pos < expected_dst_size) {
        if (in_pos + 4 > src_size) {
            if (error_out) *error_out = "Truncated RVZ packed block length.";
            return false;
        }
        const uint32_t tag = read_be32(src + in_pos);
        in_pos += 4;
        const bool is_prng = (tag & 0x80000000u) != 0;
        const size_t run_len = static_cast<size_t>(tag & 0x7FFFFFFFu);

        if (out_pos + run_len > expected_dst_size) {
            if (error_out) *error_out = "RVZ packed block exceeds chunk bounds.";
            return false;
        }

        if (!is_prng) {
            if (in_pos + run_len > src_size) {
                if (error_out) *error_out = "Truncated RVZ raw run.";
                return false;
            }
            if (run_len > 0) {
                std::memcpy(dst + out_pos, src + in_pos, run_len);
                in_pos += run_len;
                out_pos += run_len;
                cur_offset += run_len;
            }
        } else {
            if (in_pos + 68 > src_size) {
                if (error_out) *error_out = "Truncated RVZ PRNG seed.";
                return false;
            }
            RvzPrng prng;
            prng.seed(src + in_pos);
            in_pos += 68;
            prng.skip_bytes(cur_offset % 0x8000ULL);
            prng.generate_bytes(dst + out_pos, run_len);
            out_pos += run_len;
            cur_offset += run_len;
        }
    }

    if (out_pos < expected_dst_size) {
        std::memset(dst + out_pos, 0, expected_dst_size - out_pos);
    }
    return true;
}

bool RvzDiscBackend::decompress_bytes(const uint8_t* src,
                                      size_t src_size,
                                      std::vector<uint8_t>& dst,
                                      size_t expected_size,
                                      std::string* error_out) const {
    dst.resize(expected_size);
    if (expected_size == 0) {
        return true;
    }
    if (compression_ == 0) {
        if (src_size < expected_size) {
            if (error_out) *error_out = "Uncompressed RVZ block is shorter than expected.";
            return false;
        }
        std::memcpy(dst.data(), src, expected_size);
        return true;
    }
    if (compression_ != 5) {
        if (error_out) {
            *error_out = std::string("Unsupported WIA/RVZ compression method: ") +
                         compression_label(compression_) + " (only None and Zstandard are supported).";
        }
        return false;
    }

    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) {
        if (error_out) *error_out = "Failed to create ZSTD_DCtx.";
        return false;
    }

    ZSTD_inBuffer in{src, src_size, 0};
    ZSTD_outBuffer out{dst.data(), expected_size, 0};
    while (in.pos < in.size && out.pos < out.size) {
        const size_t ret = ZSTD_decompressStream(dctx, &out, &in);
        if (ZSTD_isError(ret)) {
            if (error_out) {
                *error_out = std::string("Zstd decompression error: ") + ZSTD_getErrorName(ret);
            }
            ZSTD_freeDCtx(dctx);
            return false;
        }
        if (ret == 0) {
            break;
        }
    }
    ZSTD_freeDCtx(dctx);

    if (out.pos != expected_size) {
        dst.resize(out.pos);
    }
    return true;
}

bool RvzDiscBackend::open(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out) {
    std::lock_guard lock(mutex_);
    file_.close();
    raw_entries_.clear();
    groups_.clear();
    lru_order_.clear();
    chunk_cache_.clear();

    file_.open(path, std::ios::binary | std::ios::ate);
    if (!file_) {
        if (error_out) *error_out = "Could not open RVZ/WIA file: " + path.string();
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_.tellg());
    if (file_size < 0x48 + 0xD4) {
        if (error_out) *error_out = "File too small for WIA/RVZ header.";
        return false;
    }

    uint8_t file_head[0x48]{};
    file_.seekg(0, std::ios::beg);
    file_.read(reinterpret_cast<char*>(file_head), sizeof(file_head));
    if (!file_) {
        if (error_out) *error_out = "Failed reading WIA/RVZ file header.";
        return false;
    }

    if (std::memcmp(file_head, "RVZ\x01", 4) == 0) {
        is_rvz_ = true;
    } else if (std::memcmp(file_head, "WIA\x01", 4) == 0) {
        is_rvz_ = false;
    } else {
        if (error_out) *error_out = "Invalid WIA/RVZ magic.";
        return false;
    }

    const uint32_t disc_struct_size = read_be32(file_head + 0x0C);
    iso_file_size_ = read_be64(file_head + 0x24);
    if (disc_struct_size < 0xD4 || 0x48 + disc_struct_size > file_size) {
        if (error_out) *error_out = "Invalid RVZ/WIA disc struct size.";
        return false;
    }

    std::vector<uint8_t> disc_buf(disc_struct_size);
    file_.seekg(0x48, std::ios::beg);
    file_.read(reinterpret_cast<char*>(disc_buf.data()), static_cast<std::streamsize>(disc_buf.size()));
    if (!file_) {
        if (error_out) *error_out = "Failed reading RVZ/WIA disc metadata.";
        return false;
    }

    const uint32_t disc_type = read_be32(disc_buf.data() + 0x00);
    compression_ = read_be32(disc_buf.data() + 0x04);
    compr_level_ = static_cast<int32_t>(read_be32(disc_buf.data() + 0x08));
    chunk_size_ = read_be32(disc_buf.data() + 0x0C);
    std::memcpy(dhead_.data(), disc_buf.data() + 0x10, 0x80);

    const uint32_t n_part = read_be32(disc_buf.data() + 0x90);
    const uint32_t n_raw_data = read_be32(disc_buf.data() + 0xB4);
    const uint64_t raw_data_off = read_be64(disc_buf.data() + 0xB8);
    const uint32_t raw_data_comp_size = read_be32(disc_buf.data() + 0xC0);
    const uint32_t n_groups = read_be32(disc_buf.data() + 0xC4);
    const uint64_t group_off = read_be64(disc_buf.data() + 0xC8);
    const uint32_t group_comp_size = read_be32(disc_buf.data() + 0xD0);

    out_info = {};
    out_info.file_size_bytes = file_size;
    out_info.logical_iso_size = iso_file_size_;
    out_info.chunk_size = chunk_size_;
    out_info.container_format = is_rvz_ ? "RVZ" : "WIA";
    out_info.compression_name = compression_label(compression_);

    if (disc_type != 1 || n_part != 0) {
        if (error_out) {
            *error_out = "This RVZ/WIA image is not a GameCube disc (expected disc_type=1).";
        }
        return false;
    }
    if (chunk_size_ < 0x8000 || n_raw_data == 0 || n_groups == 0) {
        if (error_out) *error_out = "Invalid chunk size or empty RVZ/WIA tables.";
        return false;
    }
    if (compression_ != 0 && compression_ != 5) {
        if (error_out) {
            *error_out = std::string("Unsupported compression in ") + out_info.container_format + ": " +
                         out_info.compression_name + " (expected Zstandard or None).";
        }
        return false;
    }

    // Read & decompress wia_raw_data_t table (24 bytes per entry)
    if (raw_data_off + raw_data_comp_size > file_size) {
        if (error_out) *error_out = "RVZ raw_data table extends past EOF.";
        return false;
    }
    std::vector<uint8_t> raw_comp(raw_data_comp_size);
    file_.seekg(static_cast<std::streamoff>(raw_data_off), std::ios::beg);
    file_.read(reinterpret_cast<char*>(raw_comp.data()), static_cast<std::streamsize>(raw_comp.size()));
    std::vector<uint8_t> raw_table;
    if (!decompress_bytes(raw_comp.data(), raw_comp.size(), raw_table, static_cast<size_t>(n_raw_data) * 24, error_out)) {
        return false;
    }

    raw_entries_.resize(n_raw_data);
    for (uint32_t i = 0; i < n_raw_data; ++i) {
        const uint8_t* p = raw_table.data() + static_cast<size_t>(i) * 24;
        const uint64_t r_off = read_be64(p + 0x00);
        const uint64_t r_sz = read_be64(p + 0x08);
        const uint64_t aligned_off = r_off & ~0x7FFFULL;
        const uint64_t aligned_sz = r_sz + (r_off - aligned_off);
        raw_entries_[i].aligned_off = aligned_off;
        raw_entries_[i].aligned_size = aligned_sz;
        raw_entries_[i].group_index = read_be32(p + 0x10);
        raw_entries_[i].n_groups = read_be32(p + 0x14);
    }

    // Read & decompress group table (12 bytes for RVZ, 8 bytes for WIA)
    if (group_off + group_comp_size > file_size) {
        if (error_out) *error_out = "RVZ group table extends past EOF.";
        return false;
    }
    const size_t group_stride = is_rvz_ ? 12 : 8;
    std::vector<uint8_t> grp_comp(group_comp_size);
    file_.seekg(static_cast<std::streamoff>(group_off), std::ios::beg);
    file_.read(reinterpret_cast<char*>(grp_comp.data()), static_cast<std::streamsize>(grp_comp.size()));
    std::vector<uint8_t> grp_table;
    if (!decompress_bytes(grp_comp.data(), grp_comp.size(), grp_table, static_cast<size_t>(n_groups) * group_stride, error_out)) {
        return false;
    }

    groups_.resize(n_groups);
    for (uint32_t i = 0; i < n_groups; ++i) {
        const uint8_t* p = grp_table.data() + static_cast<size_t>(i) * group_stride;
        groups_[i].file_offset = static_cast<uint64_t>(read_be32(p + 0x00)) * 4ULL;
        const uint32_t ds = read_be32(p + 0x04);
        if (is_rvz_) {
            groups_[i].compressed = (ds & 0x80000000u) != 0;
            groups_[i].stored_size = ds & 0x7FFFFFFFu;
            groups_[i].rvz_packed_size = read_be32(p + 0x08);
        } else {
            groups_[i].compressed = (compression_ != 0);
            groups_[i].stored_size = ds;
            groups_[i].rvz_packed_size = 0;
        }
    }

    // Read logical 0x440 boot header to populate GameID, magic, title, DOL & FST offsets
    uint8_t boot_hdr[0x440]{};
    // Release lock temporarily via scoped read helper? Better: read_at locks mutex_, so call internal?
    // Wait: we hold mutex_ right now! Let's populate boot_hdr directly after unlocking or via helper.
    // Let's copy dhead_ for [0..0x80) and read [0x80..0x440) after building the object.
    // Actually we can just unlock before calling read_at!
    (void)boot_hdr;
    // Mark ready and read boot header via read_at outside the lock:
    out_info.is_supported_for_mount = true;
    // Temporarily return; read_at below handles locking cleanly.
    // Let's release lock first by scoping it!
    // (Done by structuring open() with a small inner lock or unlocking cleanly — let's do an inner helper or unique_lock.)
    // Wait: std::lock_guard doesn't unlock early. Let's read [0x80..0x440) cleanly right here using get_chunk_locked!
    std::memcpy(boot_hdr, dhead_.data(), 0x80);
    if (!raw_entries_.empty() && raw_entries_[0].aligned_off <= 0x80 &&
        raw_entries_[0].aligned_off + raw_entries_[0].aligned_size >= 0x440) {
        const uint32_t grp = raw_entries_[0].group_index;
        const size_t chunk_bytes = static_cast<size_t>(std::min<uint64_t>(chunk_size_, raw_entries_[0].aligned_size));
        if (const auto* chunk = get_chunk_locked(grp, raw_entries_[0].aligned_off, chunk_bytes)) {
            const size_t src_off = static_cast<size_t>(0x80 - raw_entries_[0].aligned_off);
            if (src_off + (0x440 - 0x80) <= chunk->size()) {
                std::memcpy(boot_hdr + 0x80, chunk->data() + src_off, 0x440 - 0x80);
            }
        }
    }

    parse_gc_boot_header(boot_hdr, out_info);
    out_info.is_supported_for_mount = out_info.is_valid_gc_magic;
    if (!out_info.is_valid_gc_magic && error_out) {
        *error_out = "Invalid GameCube disc magic inside RVZ/WIA image.";
    }
    return out_info.is_valid_gc_magic;
}

const std::vector<uint8_t>* RvzDiscBackend::get_chunk_locked(uint32_t group_idx,
                                                             uint64_t chunk_disc_offset,
                                                             size_t expected_size) {
    if (group_idx >= groups_.size()) {
        return nullptr;
    }

    if (auto it = chunk_cache_.find(group_idx); it != chunk_cache_.end()) {
        lru_order_.splice(lru_order_.begin(), lru_order_, it->second.second);
        return &it->second.first;
    }

    const GroupEntry& grp = groups_[group_idx];
    std::vector<uint8_t> decoded(expected_size, 0);

    if (grp.stored_size > 0) {
        std::vector<uint8_t> raw_stored(grp.stored_size);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(grp.file_offset), std::ios::beg);
        file_.read(reinterpret_cast<char*>(raw_stored.data()), static_cast<std::streamsize>(grp.stored_size));
        if (!file_) {
            return nullptr;
        }

        std::vector<uint8_t> decompressed;
        const uint8_t* post_comp_ptr = raw_stored.data();
        size_t post_comp_size = raw_stored.size();

        if (grp.compressed && compression_ != 0) {
            const size_t target_sz = grp.rvz_packed_size != 0 ? grp.rvz_packed_size : expected_size;
            if (!decompress_bytes(raw_stored.data(), raw_stored.size(), decompressed, target_sz, nullptr)) {
                return nullptr;
            }
            post_comp_ptr = decompressed.data();
            post_comp_size = decompressed.size();
        }

        if (grp.rvz_packed_size != 0) {
            if (!unpack_rvz_data(post_comp_ptr, post_comp_size, chunk_disc_offset,
                                 decoded.data(), expected_size, nullptr)) {
                return nullptr;
            }
        } else {
            const size_t copy_n = std::min(expected_size, post_comp_size);
            std::memcpy(decoded.data(), post_comp_ptr, copy_n);
        }
    }

    if (chunk_cache_.size() >= kMaxCachedChunks && !lru_order_.empty()) {
        const uint32_t evict_idx = lru_order_.back();
        lru_order_.pop_back();
        chunk_cache_.erase(evict_idx);
    }

    lru_order_.push_front(group_idx);
    auto [inserted, _] = chunk_cache_.emplace(
        group_idx, std::make_pair(std::move(decoded), lru_order_.begin()));
    return &inserted->second.first;
}

bool RvzDiscBackend::read_at(uint64_t offset, void* dst, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!dst || offset + size > iso_file_size_) {
        return false;
    }

    std::lock_guard lock(mutex_);
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t remaining = size;
    uint64_t cur = offset;

    if (cur < 0x80) {
        const size_t copy_n = static_cast<size_t>(std::min<uint64_t>(0x80 - cur, remaining));
        std::memcpy(out, dhead_.data() + cur, copy_n);
        out += copy_n;
        cur += copy_n;
        remaining -= copy_n;
    }

    while (remaining > 0) {
        const RawDataEntry* match = nullptr;
        for (const auto& entry : raw_entries_) {
            if (cur >= entry.aligned_off && cur < entry.aligned_off + entry.aligned_size) {
                match = &entry;
                break;
            }
        }
        if (!match) {
            // Gap outside raw_data entries (scrubbed zero padding)
            uint64_t next_start = iso_file_size_;
            for (const auto& entry : raw_entries_) {
                if (entry.aligned_off > cur && entry.aligned_off < next_start) {
                    next_start = entry.aligned_off;
                }
            }
            const size_t zero_n = static_cast<size_t>(std::min<uint64_t>(next_start - cur, remaining));
            std::memset(out, 0, zero_n);
            out += zero_n;
            cur += zero_n;
            remaining -= zero_n;
            continue;
        }

        const uint64_t local_off = cur - match->aligned_off;
        const uint32_t chunk_local_idx = static_cast<uint32_t>(local_off / chunk_size_);
        if (chunk_local_idx >= match->n_groups) {
            return false;
        }
        const uint32_t group_idx = match->group_index + chunk_local_idx;
        const uint64_t chunk_start = match->aligned_off + static_cast<uint64_t>(chunk_local_idx) * chunk_size_;
        const size_t expected_chunk_sz = static_cast<size_t>(
            std::min<uint64_t>(chunk_size_, match->aligned_off + match->aligned_size - chunk_start));

        const auto* chunk = get_chunk_locked(group_idx, chunk_start, expected_chunk_sz);
        if (!chunk) {
            return false;
        }

        const size_t in_chunk_off = static_cast<size_t>(cur - chunk_start);
        if (in_chunk_off >= chunk->size()) {
            return false;
        }
        const size_t copy_n = std::min(remaining, chunk->size() - in_chunk_off);
        std::memcpy(out, chunk->data() + in_chunk_off, copy_n);
        out += copy_n;
        cur += copy_n;
        remaining -= copy_n;
    }

    return true;
}

} // namespace fzgx
