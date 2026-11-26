#pragma once

#include "disc/disc_image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzgx {

// Lagged Fibonacci PRNG used by Dolphin RVZ packing for lossless GameCube junk/padding data.
class RvzPrng {
public:
    static constexpr size_t kBufferWords = 521;

    void seed(const uint8_t seed_bytes[68]);
    void skip_bytes(uint64_t count);
    void generate_bytes(uint8_t* dst, size_t count);

private:
    void advance_buffer();
    uint8_t next_byte();

    std::array<uint32_t, kBufferWords> buffer_{};
    size_t word_index_{kBufferWords};
    uint32_t current_word_{0};
    int byte_in_word_{4};
};

// Streaming GameCube RVZ / WIA disc backend implementing Dolphin's WiaAndRvz specification.
class RvzDiscBackend final : public IDiscBackend {
public:
    RvzDiscBackend() = default;
    ~RvzDiscBackend() override = default;

    bool open(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out) override;
    bool read_at(uint64_t offset, void* dst, size_t size) override;
    uint64_t logical_size() const override { return iso_file_size_; }
    const char* format_name() const override { return is_rvz_ ? "RVZ" : "WIA"; }

    static bool unpack_rvz_data(const uint8_t* src,
                                size_t src_size,
                                uint64_t chunk_disc_offset,
                                uint8_t* dst,
                                size_t expected_dst_size,
                                std::string* error_out = nullptr);

private:
    struct RawDataEntry {
        uint64_t aligned_off{0};
        uint64_t aligned_size{0};
        uint32_t group_index{0};
        uint32_t n_groups{0};
    };

    struct GroupEntry {
        uint64_t file_offset{0};
        uint32_t stored_size{0};
        bool compressed{false};
        uint32_t rvz_packed_size{0};
    };

    bool decompress_bytes(const uint8_t* src,
                          size_t src_size,
                          std::vector<uint8_t>& dst,
                          size_t expected_size,
                          std::string* error_out) const;
    const std::vector<uint8_t>* get_chunk_locked(uint32_t group_idx, uint64_t chunk_disc_offset, size_t expected_size);

    std::ifstream file_;
    std::mutex mutex_;
    bool is_rvz_{true};
    uint32_t compression_{0};
    int32_t compr_level_{0};
    uint32_t chunk_size_{0};
    uint64_t iso_file_size_{0};
    std::array<uint8_t, 0x80> dhead_{};
    std::vector<RawDataEntry> raw_entries_;
    std::vector<GroupEntry> groups_;

    // 32-entry LRU chunk cache
    static constexpr size_t kMaxCachedChunks = 32;
    std::list<uint32_t> lru_order_;
    std::unordered_map<uint32_t, std::pair<std::vector<uint8_t>, std::list<uint32_t>::iterator>> chunk_cache_;
};

} // namespace fzgx
