#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzgx {

struct U8Entry {
    std::string path;   // Normalized path inside archive, e.g. "car/blue_falcon.gma"
    uint32_t offset{0}; // Byte offset from start of decompressed archive
    uint32_t size{0};
    bool is_dir{false};
};

class U8Archive {
public:
    static constexpr uint32_t kMagic = 0x55AA382Du;

    bool load(std::span<const uint8_t> raw_bytes,
              bool auto_decompress_lz = true,
              std::string* error_out = nullptr);
    void clear();

    bool is_loaded() const { return !entries_.empty(); }
    const std::vector<U8Entry>& entries() const { return entries_; }
    size_t file_count() const { return file_count_; }

    const U8Entry* find(const std::string& path) const;
    std::span<const uint8_t> entry_bytes(const U8Entry& entry) const;
    bool read_file(const std::string& path, std::vector<uint8_t>& out) const;

private:
    std::vector<uint8_t> buffer_;
    std::vector<U8Entry> entries_;
    std::unordered_map<std::string, size_t> index_by_path_;
    size_t file_count_{0};
};

} // namespace fzgx
