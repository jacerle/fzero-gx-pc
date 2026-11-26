#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fzgx {

struct GciFileMeta {
    std::filesystem::path host_path;
    std::string game_code;   // e.g. "GFZE"
    std::string company;     // e.g. "8P"
    std::string file_name;   // up to 32 chars
    uint32_t mod_time{0};    // GameCube epoch seconds
    uint16_t block_count{0}; // 8 KiB blocks
    uint32_t payload_size{0};
};

class GciCardManager {
public:
    static constexpr size_t kGciHeaderSize = 64;
    static constexpr size_t kBlockSize = 8192;

    bool mount_slot(int slot, const std::filesystem::path& dir, std::string* error_out = nullptr);
    bool refresh_slot(int slot, std::string* error_out = nullptr);

    const std::filesystem::path& slot_dir(int slot) const;
    const std::vector<GciFileMeta>& files(int slot) const;

    bool read_gci_payload(int slot, const std::string& file_name, std::vector<uint8_t>& out_payload) const;
    bool write_gci_file(int slot,
                        const std::string& game_code,
                        const std::string& company,
                        const std::string& file_name,
                        const std::vector<uint8_t>& payload,
                        std::string* error_out = nullptr);
    bool delete_gci_file(int slot, const std::string& file_name, std::string* error_out = nullptr);

    static bool parse_gci_header(const uint8_t header[64], GciFileMeta& out_meta);
    static void build_gci_header(const GciFileMeta& meta, uint8_t out_header[64]);

private:
    std::filesystem::path slot_dirs_[2];
    std::vector<GciFileMeta> slot_files_[2];
    std::filesystem::path empty_path_;
    std::vector<GciFileMeta> empty_files_;
};

} // namespace fzgx
