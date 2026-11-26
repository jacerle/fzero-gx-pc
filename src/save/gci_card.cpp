#include "save/gci_card.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

namespace fzgx {

namespace {

inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[1] = static_cast<uint8_t>(v & 0xFFU);
}

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<uint8_t>(v & 0xFFU);
}

std::string sanitize_filename(std::string s) {
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            c = '_';
        }
    }
    return s;
}

} // namespace

bool GciCardManager::parse_gci_header(const uint8_t header[64], GciFileMeta& out_meta) {
    out_meta.game_code = std::string(reinterpret_cast<const char*>(header + 0x00), 4);
    out_meta.company = std::string(reinterpret_cast<const char*>(header + 0x04), 2);

    size_t name_len = 0;
    while (name_len < 32 && header[0x08 + name_len] != '\0') {
        ++name_len;
    }
    out_meta.file_name = std::string(reinterpret_cast<const char*>(header + 0x08), name_len);
    out_meta.mod_time = read_be32(header + 0x28);
    out_meta.block_count = read_be16(header + 0x38);
    out_meta.payload_size = static_cast<uint32_t>(out_meta.block_count) * static_cast<uint32_t>(kBlockSize);
    return !out_meta.file_name.empty();
}

void GciCardManager::build_gci_header(const GciFileMeta& meta, uint8_t out_header[64]) {
    std::memset(out_header, 0, 64);
    std::memcpy(out_header + 0x00, meta.game_code.data(), std::min<size_t>(meta.game_code.size(), 4));
    std::memcpy(out_header + 0x04, meta.company.data(), std::min<size_t>(meta.company.size(), 2));
    out_header[0x07] = 0x01; // Banner format default
    std::memcpy(out_header + 0x08, meta.file_name.data(), std::min<size_t>(meta.file_name.size(), 32));
    write_be32(out_header + 0x28, meta.mod_time);
    write_be16(out_header + 0x38, meta.block_count);
    write_be16(out_header + 0x3A, 0xFFFF); // Unused padding
}

bool GciCardManager::mount_slot(int slot, const std::filesystem::path& dir, std::string* error_out) {
    if (slot < 0 || slot > 1) {
        if (error_out) {
            *error_out = "Invalid Memory Card slot (expected 0 or 1).";
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error_out) {
            *error_out = "Failed creating GCI directory: " + ec.message();
        }
        return false;
    }
    slot_dirs_[slot] = dir;
    return refresh_slot(slot, error_out);
}

bool GciCardManager::refresh_slot(int slot, std::string* error_out) {
    if (slot < 0 || slot > 1 || slot_dirs_[slot].empty()) {
        return false;
    }
    slot_files_[slot].clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(slot_dirs_[slot], ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext != ".gci") {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
        if (!in || static_cast<size_t>(in.tellg()) < kGciHeaderSize) {
            continue;
        }
        const size_t total_size = static_cast<size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        uint8_t hdr[kGciHeaderSize]{};
        in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        GciFileMeta meta{};
        if (parse_gci_header(hdr, meta)) {
            meta.host_path = entry.path();
            if (meta.payload_size == 0) {
                meta.payload_size = static_cast<uint32_t>(total_size - kGciHeaderSize);
                meta.block_count = static_cast<uint16_t>((meta.payload_size + kBlockSize - 1) / kBlockSize);
            }
            slot_files_[slot].push_back(std::move(meta));
        }
    }
    if (ec && error_out) {
        *error_out = "Failed scanning GCI folder: " + ec.message();
        return false;
    }
    std::sort(slot_files_[slot].begin(), slot_files_[slot].end(),
              [](const GciFileMeta& a, const GciFileMeta& b) { return a.file_name < b.file_name; });
    return true;
}

const std::filesystem::path& GciCardManager::slot_dir(int slot) const {
    if (slot < 0 || slot > 1) {
        return empty_path_;
    }
    return slot_dirs_[slot];
}

const std::vector<GciFileMeta>& GciCardManager::files(int slot) const {
    if (slot < 0 || slot > 1) {
        return empty_files_;
    }
    return slot_files_[slot];
}

bool GciCardManager::read_gci_payload(int slot, const std::string& file_name, std::vector<uint8_t>& out_payload) const {
    if (slot < 0 || slot > 1) {
        return false;
    }
    for (const auto& f : slot_files_[slot]) {
        if (f.file_name == file_name) {
            std::ifstream in(f.host_path, std::ios::binary | std::ios::ate);
            if (!in || static_cast<size_t>(in.tellg()) < kGciHeaderSize) {
                return false;
            }
            const size_t payload_bytes = static_cast<size_t>(in.tellg()) - kGciHeaderSize;
            in.seekg(static_cast<std::streamoff>(kGciHeaderSize), std::ios::beg);
            out_payload.resize(payload_bytes);
            if (payload_bytes > 0) {
                in.read(reinterpret_cast<char*>(out_payload.data()), static_cast<std::streamsize>(payload_bytes));
            }
            return static_cast<bool>(in);
        }
    }
    return false;
}

bool GciCardManager::write_gci_file(int slot,
                                    const std::string& game_code,
                                    const std::string& company,
                                    const std::string& file_name,
                                    const std::vector<uint8_t>& payload,
                                    std::string* error_out) {
    if (slot < 0 || slot > 1 || slot_dirs_[slot].empty()) {
        if (error_out) {
            *error_out = "Memory Card slot is not mounted.";
        }
        return false;
    }

    const uint16_t blocks = static_cast<uint16_t>(std::max<size_t>(1, (payload.size() + kBlockSize - 1) / kBlockSize));
    const size_t padded_size = static_cast<size_t>(blocks) * kBlockSize;

    GciFileMeta meta{};
    meta.game_code = game_code;
    meta.company = company;
    meta.file_name = file_name;
    meta.block_count = blocks;
    meta.payload_size = static_cast<uint32_t>(padded_size);
    // GameCube epoch starts 2000-01-01 00:00:00 UTC (946684800 Unix seconds).
    const auto now = static_cast<int64_t>(std::time(nullptr));
    meta.mod_time = now > 946684800 ? static_cast<uint32_t>(now - 946684800) : 0;

    uint8_t hdr[kGciHeaderSize]{};
    build_gci_header(meta, hdr);

    const std::string safe_name =
        sanitize_filename(game_code) + "-" + sanitize_filename(file_name) + ".gci";
    const auto out_path = slot_dirs_[slot] / safe_name;

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        if (error_out) {
            *error_out = "Failed opening GCI file for writing: " + out_path.string();
        }
        return false;
    }
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    if (!payload.empty()) {
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    if (padded_size > payload.size()) {
        std::vector<uint8_t> pad(padded_size - payload.size(), 0);
        out.write(reinterpret_cast<const char*>(pad.data()), static_cast<std::streamsize>(pad.size()));
    }
    out.flush();
    return refresh_slot(slot, error_out);
}

bool GciCardManager::delete_gci_file(int slot, const std::string& file_name, std::string* error_out) {
    if (slot < 0 || slot > 1) {
        return false;
    }
    for (const auto& f : slot_files_[slot]) {
        if (f.file_name == file_name) {
            std::error_code ec;
            std::filesystem::remove(f.host_path, ec);
            if (ec) {
                if (error_out) {
                    *error_out = "Failed deleting GCI file: " + ec.message();
                }
                return false;
            }
            return refresh_slot(slot, error_out);
        }
    }
    return false;
}

} // namespace fzgx
