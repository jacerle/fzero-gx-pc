#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fzgx {

struct GciFileMeta {
    std::filesystem::path host_path;
    // Raw 64-byte big-endian GameCube directory entry (CARDDir), exactly as stored in the file.
    std::array<uint8_t, 64> header{};
    std::string game_code;   // e.g. "GFZE"
    std::string company;     // e.g. "8P"
    std::string file_name;   // up to 32 chars
    uint32_t mod_time{0};    // GameCube epoch seconds
    uint16_t block_count{0}; // 8 KiB blocks
    uint32_t payload_size{0};
};

// Dolphin-compatible GCI folder memory cards (slots 0/1). A .gci file is the 64-byte directory
// entry followed by the file's 8 KiB blocks.
//
// The *_entry methods address a file like the GameCube directory does: by the game code (0x00),
// maker code (0x04) and NUL-padded file name (0x08) of a 64-byte directory entry `key`.
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

    const GciFileMeta* find_entry(int slot, const uint8_t key[kGciHeaderSize]) const;
    // Creates a new file from a raw directory entry; the payload is zero-padded to the entry's
    // block count (0x38). Fails if a file with the same identity exists or the payload is larger.
    bool create_entry(int slot,
                      const uint8_t header[kGciHeaderSize],
                      const std::vector<uint8_t>& payload,
                      std::string* error_out = nullptr);
    // Replaces the directory entry of the file identified by `key`. When the new entry changes
    // the identity (rename), the host file is renamed to match.
    bool update_entry_header(int slot,
                             const uint8_t key[kGciHeaderSize],
                             const uint8_t header[kGciHeaderSize],
                             std::string* error_out = nullptr);
    // Reads/writes payload bytes [offset, offset + size); the range must lie inside the file.
    bool read_entry_data(int slot, const uint8_t key[kGciHeaderSize], size_t offset, uint8_t* out, size_t size) const;
    bool write_entry_data(int slot,
                          const uint8_t key[kGciHeaderSize],
                          size_t offset,
                          const uint8_t* data,
                          size_t size,
                          std::string* error_out = nullptr);
    bool delete_entry(int slot, const uint8_t key[kGciHeaderSize], std::string* error_out = nullptr);

    static bool parse_gci_header(const uint8_t header[64], GciFileMeta& out_meta);
    static void build_gci_header(const GciFileMeta& meta, uint8_t out_header[64]);

private:
    // Host path for a file with `header`'s identity: its current path, or a new unused
    // Dolphin-style "<maker>-<game>-<name>.gci" name in the slot folder.
    std::filesystem::path host_path_for(int slot, const uint8_t header[kGciHeaderSize]) const;
    bool write_file(int slot,
                    const std::filesystem::path& path,
                    const uint8_t header[kGciHeaderSize],
                    const std::vector<uint8_t>& payload,
                    std::string* error_out);

    std::filesystem::path slot_dirs_[2];
    std::vector<GciFileMeta> slot_files_[2];
    std::filesystem::path empty_path_;
    std::vector<GciFileMeta> empty_files_;
};

} // namespace fzgx
