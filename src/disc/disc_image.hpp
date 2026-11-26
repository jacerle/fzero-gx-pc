#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fzgx {

struct DiscHeaderInfo {
    std::string game_id;        // e.g. "GFZE01"
    std::string game_code;      // e.g. "GFZE"
    std::string maker_code;     // e.g. "8P"
    uint8_t disc_number{0};
    uint8_t game_version{0};
    uint32_t magic{0};          // GameCube magic 0xC2339F3D
    std::string internal_title; // From 0x0020
    uint32_t dol_offset{0};
    uint32_t fst_offset{0};
    uint32_t fst_size{0};
    uint64_t file_size_bytes{0};
    uint64_t logical_iso_size{0};
    uint32_t chunk_size{0};
    std::string container_format; // "ISO/GCM", "RVZ", "WIA"
    std::string compression_name{"None"};
    bool is_valid_gc_magic{false};
    bool is_fzero_gx_usa{false};
    bool is_supported_for_mount{false};
};

struct FstEntry {
    std::string path;   // Disc path, e.g. "bgm/01_mute.adx"
    uint64_t offset{0}; // Logical byte offset within disc image
    uint64_t size{0};
    bool is_dir{false};
};

class IDiscBackend {
public:
    virtual ~IDiscBackend() = default;
    virtual bool open(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out) = 0;
    virtual bool read_at(uint64_t offset, void* dst, size_t size) = 0;
    virtual uint64_t logical_size() const = 0;
    virtual const char* format_name() const = 0;
};

void parse_gc_boot_header(const uint8_t hdr[0x440], DiscHeaderInfo& info);

class DiscImage {
public:
    DiscImage() = default;

    bool mount(const std::filesystem::path& rom_path,
               const std::filesystem::path& mod_override_dir = {},
               std::string* error_out = nullptr);
    void unmount();

    bool is_mounted() const { return mounted_; }
    const DiscHeaderInfo& header() const { return header_; }
    const std::filesystem::path& rom_path() const { return rom_path_; }
    const std::filesystem::path& mod_override_dir() const { return mod_override_dir_; }

    size_t file_count() const { return file_list_.size(); }
    const std::vector<FstEntry>& files() const { return file_list_; }

    bool exists(const std::string& fst_path) const;
    bool find_entry(const std::string& fst_path, FstEntry& out_entry) const;
    bool read_file(const std::string& fst_path, std::vector<uint8_t>& out_bytes, bool* out_from_mod = nullptr);
    bool read_dol(std::vector<uint8_t>& out_bytes);

    // Computes SHA-1 over the logical uncompressed GameCube disc stream (works for both ISO and RVZ).
    bool compute_logical_sha1(
        std::string& out_hex,
        const std::function<bool(uint64_t done, uint64_t total)>& progress_cb = {});

    static bool inspect_file(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out = nullptr);
    static bool compute_sha1(
        const std::filesystem::path& path,
        std::string& out_hex,
        const std::function<bool(uint64_t done, uint64_t total)>& progress_cb = {});

private:
    bool parse_fst(std::string* error_out);

    std::filesystem::path rom_path_;
    std::filesystem::path mod_override_dir_;
    std::unique_ptr<IDiscBackend> backend_;
    DiscHeaderInfo header_{};
    bool mounted_{false};
    std::vector<FstEntry> file_list_;
    std::unordered_map<std::string, size_t> path_index_;
};

inline constexpr uint32_t kGameCubeMagic = 0xC2339F3Du;

} // namespace fzgx
