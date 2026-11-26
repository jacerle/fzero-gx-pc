#include "save/gci_card.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string_view>
#include <utility>

namespace fzgx {

namespace {

// Directory entry (CARDDir) field offsets used here.
constexpr size_t kDentryCompany = 0x04;
constexpr size_t kDentryBannerFormat = 0x07;
constexpr size_t kDentryFileName = 0x08;
constexpr size_t kDentryFileNameMax = 32;
constexpr size_t kDentryTime = 0x28;
constexpr size_t kDentryLength = 0x38;
constexpr size_t kDentryPadding1 = 0x3A;

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
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return s;
}

std::string_view dentry_name(const uint8_t* header) {
    size_t len = 0;
    while (len < kDentryFileNameMax && header[kDentryFileName + len] != '\0') {
        ++len;
    }
    return {reinterpret_cast<const char*>(header + kDentryFileName), len};
}

// Same file on a card: game code, maker code and file name match.
bool same_identity(const uint8_t* a, const uint8_t* b) {
    return std::memcmp(a, b, kDentryCompany + 2) == 0 && dentry_name(a) == dentry_name(b);
}

void set_error(std::string* error_out, std::string message) {
    if (error_out) {
        *error_out = std::move(message);
    }
}

} // namespace

bool GciCardManager::parse_gci_header(const uint8_t header[64], GciFileMeta& out_meta) {
    std::memcpy(out_meta.header.data(), header, kGciHeaderSize);
    out_meta.game_code = std::string(reinterpret_cast<const char*>(header + 0x00), 4);
    out_meta.company = std::string(reinterpret_cast<const char*>(header + kDentryCompany), 2);
    out_meta.file_name = std::string(dentry_name(header));
    out_meta.mod_time = read_be32(header + kDentryTime);
    out_meta.block_count = read_be16(header + kDentryLength);
    out_meta.payload_size = static_cast<uint32_t>(out_meta.block_count) * static_cast<uint32_t>(kBlockSize);
    return !out_meta.file_name.empty();
}

void GciCardManager::build_gci_header(const GciFileMeta& meta, uint8_t out_header[64]) {
    std::memset(out_header, 0, 64);
    std::memcpy(out_header + 0x00, meta.game_code.data(), std::min<size_t>(meta.game_code.size(), 4));
    std::memcpy(out_header + kDentryCompany, meta.company.data(), std::min<size_t>(meta.company.size(), 2));
    out_header[kDentryBannerFormat] = 0x01; // Banner format default
    std::memcpy(out_header + kDentryFileName, meta.file_name.data(),
                std::min<size_t>(meta.file_name.size(), kDentryFileNameMax));
    write_be32(out_header + kDentryTime, meta.mod_time);
    write_be16(out_header + kDentryLength, meta.block_count);
    write_be16(out_header + kDentryPadding1, 0xFFFF); // Unused padding
}

bool GciCardManager::mount_slot(int slot, const std::filesystem::path& dir, std::string* error_out) {
    if (slot < 0 || slot > 1) {
        set_error(error_out, "Invalid Memory Card slot (expected 0 or 1).");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        set_error(error_out, "Failed creating GCI directory: " + ec.message());
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
                write_be16(meta.header.data() + kDentryLength, meta.block_count);
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

std::filesystem::path GciCardManager::host_path_for(int slot, const uint8_t header[kGciHeaderSize]) const {
    if (const GciFileMeta* existing = find_entry(slot, header)) {
        return existing->host_path;
    }
    const std::string base =
        sanitize_filename(std::string(reinterpret_cast<const char*>(header + kDentryCompany), 2)) + "-" +
        sanitize_filename(std::string(reinterpret_cast<const char*>(header), 4)) + "-" +
        sanitize_filename(std::string(dentry_name(header)));
    auto path = slot_dirs_[slot] / (base + ".gci");
    std::error_code ec;
    for (int n = 1; std::filesystem::exists(path, ec); ++n) {
        path = slot_dirs_[slot] / (base + "-" + std::to_string(n) + ".gci");
    }
    return path;
}

bool GciCardManager::write_file(int slot,
                                const std::filesystem::path& path,
                                const uint8_t header[kGciHeaderSize],
                                const std::vector<uint8_t>& payload,
                                std::string* error_out) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error_out, "Failed opening GCI file for writing: " + path.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(header), static_cast<std::streamsize>(kGciHeaderSize));
    if (!payload.empty()) {
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    out.flush();
    if (!out) {
        set_error(error_out, "Failed writing GCI file: " + path.string());
        return false;
    }
    out.close();
    return refresh_slot(slot, error_out);
}

bool GciCardManager::write_gci_file(int slot,
                                    const std::string& game_code,
                                    const std::string& company,
                                    const std::string& file_name,
                                    const std::vector<uint8_t>& payload,
                                    std::string* error_out) {
    if (slot < 0 || slot > 1 || slot_dirs_[slot].empty()) {
        set_error(error_out, "Memory Card slot is not mounted.");
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

    std::vector<uint8_t> padded = payload;
    padded.resize(padded_size, 0);
    return write_file(slot, host_path_for(slot, hdr), hdr, padded, error_out);
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
                set_error(error_out, "Failed deleting GCI file: " + ec.message());
                return false;
            }
            return refresh_slot(slot, error_out);
        }
    }
    return false;
}

const GciFileMeta* GciCardManager::find_entry(int slot, const uint8_t key[kGciHeaderSize]) const {
    if (slot < 0 || slot > 1 || !key) {
        return nullptr;
    }
    for (const auto& f : slot_files_[slot]) {
        if (same_identity(f.header.data(), key)) {
            return &f;
        }
    }
    return nullptr;
}

bool GciCardManager::create_entry(int slot,
                                  const uint8_t header[kGciHeaderSize],
                                  const std::vector<uint8_t>& payload,
                                  std::string* error_out) {
    if (slot < 0 || slot > 1 || slot_dirs_[slot].empty()) {
        set_error(error_out, "Memory Card slot is not mounted.");
        return false;
    }
    if (dentry_name(header).empty()) {
        set_error(error_out, "GCI file name is empty.");
        return false;
    }
    if (find_entry(slot, header)) {
        set_error(error_out, "GCI file already exists: " + std::string(dentry_name(header)));
        return false;
    }
    const size_t payload_size = static_cast<size_t>(read_be16(header + kDentryLength)) * kBlockSize;
    if (payload_size == 0 || payload.size() > payload_size) {
        set_error(error_out, "GCI payload does not fit the directory entry's block count.");
        return false;
    }
    std::vector<uint8_t> padded = payload;
    padded.resize(payload_size, 0);
    return write_file(slot, host_path_for(slot, header), header, padded, error_out);
}

bool GciCardManager::update_entry_header(int slot,
                                         const uint8_t key[kGciHeaderSize],
                                         const uint8_t header[kGciHeaderSize],
                                         std::string* error_out) {
    const GciFileMeta* meta = find_entry(slot, key);
    if (!meta) {
        set_error(error_out, "GCI file not found.");
        return false;
    }
    const bool renamed = !same_identity(key, header);
    if (renamed) {
        if (dentry_name(header).empty()) {
            set_error(error_out, "GCI file name is empty.");
            return false;
        }
        if (find_entry(slot, header)) {
            set_error(error_out, "GCI file already exists: " + std::string(dentry_name(header)));
            return false;
        }
    }
    const std::filesystem::path old_path = meta->host_path;
    {
        std::fstream io(old_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!io) {
            set_error(error_out, "Failed opening GCI file for update: " + old_path.string());
            return false;
        }
        io.write(reinterpret_cast<const char*>(header), static_cast<std::streamsize>(kGciHeaderSize));
        io.flush();
        if (!io) {
            set_error(error_out, "Failed writing GCI header: " + old_path.string());
            return false;
        }
    }
    if (renamed) {
        const std::filesystem::path new_path = host_path_for(slot, header);
        std::error_code ec;
        std::filesystem::rename(old_path, new_path, ec);
        if (ec) {
            set_error(error_out, "Failed renaming GCI file: " + ec.message());
            refresh_slot(slot);
            return false;
        }
    }
    return refresh_slot(slot, error_out);
}

bool GciCardManager::read_entry_data(int slot,
                                     const uint8_t key[kGciHeaderSize],
                                     size_t offset,
                                     uint8_t* out,
                                     size_t size) const {
    const GciFileMeta* meta = find_entry(slot, key);
    if (!meta || (!out && size != 0)) {
        return false;
    }
    std::ifstream in(meta->host_path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const auto file_size = static_cast<size_t>(in.tellg());
    if (file_size < kGciHeaderSize || offset > file_size - kGciHeaderSize ||
        size > file_size - kGciHeaderSize - offset) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    in.seekg(static_cast<std::streamoff>(kGciHeaderSize + offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

bool GciCardManager::write_entry_data(int slot,
                                      const uint8_t key[kGciHeaderSize],
                                      size_t offset,
                                      const uint8_t* data,
                                      size_t size,
                                      std::string* error_out) {
    const GciFileMeta* meta = find_entry(slot, key);
    if (!meta) {
        set_error(error_out, "GCI file not found.");
        return false;
    }
    if (!data && size != 0) {
        set_error(error_out, "No data to write.");
        return false;
    }
    std::fstream io(meta->host_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
    if (!io) {
        set_error(error_out, "Failed opening GCI file for writing: " + meta->host_path.string());
        return false;
    }
    const auto file_size = static_cast<size_t>(io.tellp());
    if (file_size < kGciHeaderSize || offset > file_size - kGciHeaderSize ||
        size > file_size - kGciHeaderSize - offset) {
        set_error(error_out, "GCI write range exceeds the file.");
        return false;
    }
    if (size == 0) {
        return true;
    }
    io.seekp(static_cast<std::streamoff>(kGciHeaderSize + offset), std::ios::beg);
    io.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    io.flush();
    if (!io) {
        set_error(error_out, "Failed writing GCI file: " + meta->host_path.string());
        return false;
    }
    return true;
}

bool GciCardManager::delete_entry(int slot, const uint8_t key[kGciHeaderSize], std::string* error_out) {
    const GciFileMeta* meta = find_entry(slot, key);
    if (!meta) {
        set_error(error_out, "GCI file not found.");
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(meta->host_path, ec);
    if (ec) {
        set_error(error_out, "Failed deleting GCI file: " + ec.message());
        return false;
    }
    return refresh_slot(slot, error_out);
}

} // namespace fzgx
