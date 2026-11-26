#include "disc/disc_image.hpp"
#include "disc/rvz_backend.hpp"
#include "disc/sha1.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fzgx {

namespace {

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint32_t read_be24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

std::string trim_cstr(const uint8_t* data, size_t max_len) {
    size_t len = 0;
    while (len < max_len && data[len] != '\0') {
        ++len;
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

std::string normalize_fst_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    std::string lower;
    lower.reserve(path.size());
    for (unsigned char c : path) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower;
}

class IsoDiscBackend final : public IDiscBackend {
public:
    bool open(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out) override {
        file_.open(path, std::ios::binary | std::ios::ate);
        if (!file_) {
            if (error_out) {
                *error_out = "Could not open disc image: " + path.string();
            }
            return false;
        }
        const auto sz = static_cast<uint64_t>(file_.tellg());
        if (sz < 0x440) {
            if (error_out) {
                *error_out = "File is too small to be a valid GameCube ISO/GCM image.";
            }
            return false;
        }
        uint8_t hdr[0x440]{};
        file_.seekg(0, std::ios::beg);
        file_.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        if (!file_) {
            if (error_out) {
                *error_out = "Failed reading GameCube boot header.";
            }
            return false;
        }

        size_bytes_ = sz;
        out_info = {};
        out_info.file_size_bytes = sz;
        out_info.logical_iso_size = sz;
        out_info.container_format = "ISO/GCM";
        out_info.compression_name = "None";
        parse_gc_boot_header(hdr, out_info);
        out_info.is_supported_for_mount = out_info.is_valid_gc_magic;
        if (!out_info.is_valid_gc_magic && error_out) {
            *error_out = "Invalid GameCube disc magic (expected 0xC2339F3D).";
        }
        return out_info.is_valid_gc_magic;
    }

    bool read_at(uint64_t offset, void* dst, size_t size) override {
        if (!file_.is_open()) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
        return static_cast<size_t>(file_.gcount()) == size;
    }

    uint64_t logical_size() const override { return size_bytes_; }
    const char* format_name() const override { return "ISO/GCM"; }

private:
    std::ifstream file_;
    uint64_t size_bytes_{0};
};

std::unique_ptr<IDiscBackend> create_backend_for_file(const std::filesystem::path& path) {
    std::ifstream probe(path, std::ios::binary);
    uint8_t magic[4]{};
    if (probe && probe.read(reinterpret_cast<char*>(magic), 4)) {
        if (std::memcmp(magic, "RVZ\x01", 4) == 0 || std::memcmp(magic, "WIA\x01", 4) == 0) {
            return std::make_unique<RvzDiscBackend>();
        }
    }
    return std::make_unique<IsoDiscBackend>();
}

} // namespace

void parse_gc_boot_header(const uint8_t hdr[0x440], DiscHeaderInfo& info) {
    info.game_code = trim_cstr(hdr + 0x00, 4);
    info.maker_code = trim_cstr(hdr + 0x04, 2);
    info.game_id = info.game_code + info.maker_code;
    info.disc_number = hdr[0x06];
    info.game_version = hdr[0x07];
    info.magic = read_be32(hdr + 0x1C);
    info.internal_title = trim_cstr(hdr + 0x20, 64);
    info.dol_offset = read_be32(hdr + 0x420);
    info.fst_offset = read_be32(hdr + 0x424);
    info.fst_size = read_be32(hdr + 0x428);
    info.is_valid_gc_magic = (info.magic == kGameCubeMagic);
    info.is_fzero_gx_usa = (info.is_valid_gc_magic && info.game_id == "GFZE01");
}

bool DiscImage::inspect_file(const std::filesystem::path& path, DiscHeaderInfo& out_info, std::string* error_out) {
    auto backend = create_backend_for_file(path);
    return backend->open(path, out_info, error_out);
}

bool DiscImage::compute_sha1(
    const std::filesystem::path& path,
    std::string& out_hex,
    const std::function<bool(uint64_t done, uint64_t total)>& progress_cb) {
    return Sha1::hash_file_hex(path, out_hex, progress_cb);
}

bool DiscImage::compute_logical_sha1(
    std::string& out_hex,
    const std::function<bool(uint64_t done, uint64_t total)>& progress_cb) {
    if (!mounted_ || !backend_) {
        return false;
    }
    const uint64_t total = backend_->logical_size();
    Sha1 sha;
    std::vector<uint8_t> buf(1024 * 1024);
    uint64_t offset = 0;
    while (offset < total) {
        const size_t to_read = static_cast<size_t>(std::min<uint64_t>(buf.size(), total - offset));
        if (!backend_->read_at(offset, buf.data(), to_read)) {
            return false;
        }
        sha.update(buf.data(), to_read);
        offset += to_read;
        if (progress_cb && !progress_cb(offset, total)) {
            return false;
        }
    }
    out_hex = sha.final_hex();
    return true;
}

bool DiscImage::mount(const std::filesystem::path& rom_path,
                      const std::filesystem::path& mod_override_dir,
                      std::string* error_out) {
    unmount();

    auto backend = create_backend_for_file(rom_path);
    DiscHeaderInfo info{};
    if (!backend->open(rom_path, info, error_out)) {
        header_ = info;
        return false;
    }

    rom_path_ = rom_path;
    mod_override_dir_ = mod_override_dir;
    backend_ = std::move(backend);
    header_ = info;

    if (!parse_fst(error_out)) {
        unmount();
        return false;
    }

    mounted_ = true;
    return true;
}

void DiscImage::unmount() {
    mounted_ = false;
    backend_.reset();
    file_list_.clear();
    path_index_.clear();
    files_by_offset_.clear();
    header_ = {};
}

bool DiscImage::parse_fst(std::string* error_out) {
    if (header_.fst_offset == 0 || header_.fst_size < 12) {
        if (error_out) {
            *error_out = "Disc FST offset/size is invalid.";
        }
        return false;
    }
    const uint64_t total_sz = backend_->logical_size();
    if (static_cast<uint64_t>(header_.fst_offset) + header_.fst_size > total_sz) {
        if (error_out) {
            *error_out = "Disc FST extends beyond end of image.";
        }
        return false;
    }

    std::vector<uint8_t> fst(header_.fst_size);
    if (!backend_->read_at(header_.fst_offset, fst.data(), fst.size())) {
        if (error_out) {
            *error_out = "Failed reading FST from disc image.";
        }
        return false;
    }

    const uint32_t root_count = read_be32(fst.data() + 8);
    if (root_count == 0 || static_cast<uint64_t>(root_count) * 12ULL > fst.size()) {
        if (error_out) {
            *error_out = "Corrupt GameCube FST entry count.";
        }
        return false;
    }

    const size_t string_table_offset = static_cast<size_t>(root_count) * 12;
    const auto read_name = [&](uint32_t name_off) -> std::string {
        const size_t pos = string_table_offset + name_off;
        if (pos >= fst.size()) {
            return {};
        }
        const char* start = reinterpret_cast<const char*>(fst.data() + pos);
        const size_t max_len = fst.size() - pos;
        size_t len = 0;
        while (len < max_len && start[len] != '\0') {
            ++len;
        }
        return std::string(start, len);
    };

    struct DirScope {
        uint32_t end_index;
        std::string prefix;
    };
    std::vector<DirScope> stack;
    stack.push_back({root_count, ""});

    for (uint32_t i = 1; i < root_count; ++i) {
        while (stack.size() > 1 && i >= stack.back().end_index) {
            stack.pop_back();
        }
        const uint8_t* entry = fst.data() + static_cast<size_t>(i) * 12;
        const bool is_dir = entry[0] != 0;
        const uint32_t name_off = read_be24(entry + 1);
        const uint32_t field_a = read_be32(entry + 4);
        const uint32_t field_b = read_be32(entry + 8);

        const std::string name = read_name(name_off);
        std::string full_path = stack.back().prefix.empty() ? name : (stack.back().prefix + "/" + name);

        if (is_dir) {
            const uint32_t dir_end = std::min(field_b, root_count);
            FstEntry dir_entry;
            dir_entry.path = full_path;
            dir_entry.is_dir = true;
            path_index_[normalize_fst_path(full_path)] = file_list_.size();
            file_list_.push_back(std::move(dir_entry));
            if (dir_end > i + 1) {
                stack.push_back({dir_end, full_path});
            }
        } else {
            FstEntry file_entry;
            file_entry.path = full_path;
            file_entry.offset = field_a;
            file_entry.size = field_b;
            file_entry.is_dir = false;
            path_index_[normalize_fst_path(full_path)] = file_list_.size();
            file_list_.push_back(std::move(file_entry));
        }
    }

    for (size_t i = 0; i < file_list_.size(); ++i) {
        if (!file_list_[i].is_dir) {
            files_by_offset_.push_back(i);
        }
    }
    std::stable_sort(files_by_offset_.begin(), files_by_offset_.end(), [&](size_t a, size_t b) {
        return file_list_[a].offset < file_list_[b].offset;
    });
    return true;
}

bool DiscImage::exists(const std::string& fst_path) const {
    const std::string norm = normalize_fst_path(fst_path);
    if (!mod_override_dir_.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(mod_override_dir_ / norm, ec)) {
            return true;
        }
    }
    return path_index_.find(norm) != path_index_.end();
}

bool DiscImage::find_entry(const std::string& fst_path, FstEntry& out_entry) const {
    const auto it = path_index_.find(normalize_fst_path(fst_path));
    if (it == path_index_.end()) {
        return false;
    }
    out_entry = file_list_[it->second];
    return true;
}

bool DiscImage::read_file(const std::string& fst_path, std::vector<uint8_t>& out_bytes, bool* out_from_mod) {
    const std::string norm = normalize_fst_path(fst_path);
    if (out_from_mod) {
        *out_from_mod = false;
    }

    if (!mod_override_dir_.empty()) {
        const auto mod_file = mod_override_dir_ / norm;
        std::error_code ec;
        if (std::filesystem::is_regular_file(mod_file, ec)) {
            std::ifstream in(mod_file, std::ios::binary | std::ios::ate);
            if (in) {
                const auto sz = static_cast<size_t>(in.tellg());
                in.seekg(0, std::ios::beg);
                out_bytes.resize(sz);
                if (sz == 0 || in.read(reinterpret_cast<char*>(out_bytes.data()), static_cast<std::streamsize>(sz))) {
                    if (out_from_mod) {
                        *out_from_mod = true;
                    }
                    return true;
                }
            }
        }
    }

    if (!mounted_ || !backend_) {
        return false;
    }

    FstEntry entry{};
    if (!find_entry(norm, entry) || entry.is_dir) {
        return false;
    }
    out_bytes.resize(static_cast<size_t>(entry.size));
    if (entry.size == 0) {
        return true;
    }
    return backend_->read_at(entry.offset, out_bytes.data(), static_cast<size_t>(entry.size));
}

int32_t DiscImage::entry_number(const std::string& fst_path) const {
    const std::string norm = normalize_fst_path(fst_path);
    if (norm.empty()) {
        return mounted_ ? 0 : -1;
    }
    const auto it = path_index_.find(norm);
    return it == path_index_.end() ? -1 : static_cast<int32_t>(it->second + 1);
}

const FstEntry* DiscImage::entry_at(int32_t entrynum) const {
    if (entrynum < 1 || static_cast<size_t>(entrynum) > file_list_.size()) {
        return nullptr;
    }
    return &file_list_[static_cast<size_t>(entrynum) - 1];
}

std::filesystem::path DiscImage::mod_override_path(const FstEntry& entry) const {
    if (mod_override_dir_.empty() || entry.is_dir) {
        return {};
    }
    auto mod_file = mod_override_dir_ / normalize_fst_path(entry.path);
    std::error_code ec;
    return std::filesystem::is_regular_file(mod_file, ec) ? mod_file : std::filesystem::path{};
}

uint64_t DiscImage::effective_size(const FstEntry& entry) const {
    const auto mod_file = mod_override_path(entry);
    if (!mod_file.empty()) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(mod_file, ec);
        if (!ec) {
            return sz;
        }
    }
    return entry.size;
}

bool DiscImage::read_logical(uint64_t offset, void* dst, size_t size) {
    if (!mounted_ || !backend_) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!mod_override_dir_.empty()) {
        // Last file starting at or before `offset`.
        auto it = std::upper_bound(files_by_offset_.begin(), files_by_offset_.end(), offset,
                                   [&](uint64_t off, size_t idx) { return off < file_list_[idx].offset; });
        if (it != files_by_offset_.begin()) {
            const FstEntry& entry = file_list_[*std::prev(it)];
            const auto mod_file = mod_override_path(entry);
            const uint64_t rel = offset - entry.offset;
            if (!mod_file.empty() && rel < std::max(entry.size, effective_size(entry))) {
                std::ifstream in(mod_file, std::ios::binary);
                if (!in) {
                    return false;
                }
                auto* out = static_cast<char*>(dst);
                in.seekg(static_cast<std::streamoff>(rel));
                in.read(out, static_cast<std::streamsize>(size));
                const auto got = in ? size : static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0));
                std::memset(out + got, 0, size - got);
                return true;
            }
        }
    }
    if (offset > backend_->logical_size() || size > backend_->logical_size() - offset) {
        return false;
    }
    return backend_->read_at(offset, dst, size);
}

bool DiscImage::read_dol(std::vector<uint8_t>& out_bytes) {
    if (!mounted_ || !backend_ || header_.dol_offset == 0) {
        return false;
    }
    uint8_t dol_hdr[0x100]{};
    if (!backend_->read_at(header_.dol_offset, dol_hdr, sizeof(dol_hdr))) {
        return false;
    }
    uint32_t max_end = 0x100;
    for (int i = 0; i < 18; ++i) {
        const uint32_t off = read_be32(dol_hdr + static_cast<size_t>(i) * 4);
        const uint32_t sz = read_be32(dol_hdr + 0x90 + static_cast<size_t>(i) * 4);
        if (sz > 0) {
            max_end = std::max(max_end, off + sz);
        }
    }
    if (static_cast<uint64_t>(header_.dol_offset) + max_end > backend_->logical_size()) {
        return false;
    }
    out_bytes.resize(max_end);
    return backend_->read_at(header_.dol_offset, out_bytes.data(), max_end);
}

} // namespace fzgx
