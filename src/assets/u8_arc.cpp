#include "assets/u8_arc.hpp"
#include "assets/avlz.hpp"
#include "assets/byte_reader.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace fzgx {

namespace {

std::string normalize_arc_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    for (char& c : path) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}

} // namespace

void U8Archive::clear() {
    buffer_.clear();
    entries_.clear();
    index_by_path_.clear();
    file_count_ = 0;
}

bool U8Archive::load(std::span<const uint8_t> raw_bytes,
                     bool auto_decompress_lz,
                     std::string* error_out) {
    clear();
    if (raw_bytes.empty()) {
        if (error_out) *error_out = "Empty U8 archive input.";
        return false;
    }

    if (auto_decompress_lz && ByteReader::peek_be32(raw_bytes, 0) != kMagic && is_avlz_buffer(raw_bytes)) {
        if (!decompress_avlz(raw_bytes, buffer_, error_out)) {
            return false;
        }
    } else {
        buffer_.assign(raw_bytes.begin(), raw_bytes.end());
    }

    if (buffer_.size() < 32) {
        if (error_out) *error_out = "U8 archive too small (< 32 bytes).";
        clear();
        return false;
    }

    const uint32_t magic = ByteReader::peek_be32(buffer_, 0x00);
    if (magic != kMagic) {
        if (error_out) *error_out = "Invalid U8 magic (expected 0x55AA382D).";
        clear();
        return false;
    }

    const uint32_t fst_offset = ByteReader::peek_be32(buffer_, 0x04);
    const uint32_t fst_size = ByteReader::peek_be32(buffer_, 0x08);
    const uint32_t data_offset = ByteReader::peek_be32(buffer_, 0x0C);
    (void)fst_size;
    (void)data_offset;

    if (fst_offset < 0x20 || static_cast<uint64_t>(fst_offset) + 12ull > buffer_.size()) {
        if (error_out) *error_out = "U8 FST header out of bounds.";
        clear();
        return false;
    }

    const uint32_t total_entries = ByteReader::peek_be32(buffer_, fst_offset + 8);
    if (total_entries == 0 ||
        static_cast<uint64_t>(fst_offset) + static_cast<uint64_t>(total_entries) * 12ull > buffer_.size()) {
        if (error_out) *error_out = "U8 FST entry count out of bounds.";
        clear();
        return false;
    }

    const size_t string_table_off = static_cast<size_t>(fst_offset) + static_cast<size_t>(total_entries) * 12ull;

    struct DirFrame {
        std::string prefix;
        uint32_t end_index;
    };
    std::vector<DirFrame> stack;
    stack.push_back({"", total_entries});

    for (uint32_t i = 1; i < total_entries; ++i) {
        while (!stack.empty() && i >= stack.back().end_index) {
            stack.pop_back();
        }
        const size_t off = static_cast<size_t>(fst_offset) + static_cast<size_t>(i) * 12ull;
        const uint32_t type_and_name = ByteReader::peek_be32(buffer_, off + 0);
        const bool is_dir = ((type_and_name >> 24) & 0xFFu) != 0;
        const uint32_t name_off = type_and_name & 0x00FFFFFFu;
        const uint32_t field1 = ByteReader::peek_be32(buffer_, off + 4);
        const uint32_t field2 = ByteReader::peek_be32(buffer_, off + 8);

        const std::string name = ByteReader::read_cstr_at(buffer_, string_table_off + name_off);
        const std::string parent = stack.empty() ? std::string{} : stack.back().prefix;
        const std::string full = parent.empty() ? name : (parent + "/" + name);

        U8Entry ent{};
        ent.path = full;
        ent.is_dir = is_dir;
        if (is_dir) {
            ent.offset = 0;
            ent.size = 0;
            const uint32_t end_idx = std::max(i + 1, std::min(field2, total_entries));
            stack.push_back({full, end_idx});
        } else {
            ent.offset = field1;
            ent.size = field2;
            if (static_cast<uint64_t>(ent.offset) + ent.size > buffer_.size()) {
                if (error_out) *error_out = "U8 file entry '" + full + "' exceeds archive bounds.";
                clear();
                return false;
            }
            ++file_count_;
        }

        const size_t idx = entries_.size();
        entries_.push_back(std::move(ent));
        index_by_path_[normalize_arc_path(entries_.back().path)] = idx;
    }

    return true;
}

const U8Entry* U8Archive::find(const std::string& path) const {
    const auto it = index_by_path_.find(normalize_arc_path(path));
    if (it == index_by_path_.end()) {
        return nullptr;
    }
    return &entries_[it->second];
}

std::span<const uint8_t> U8Archive::entry_bytes(const U8Entry& entry) const {
    if (entry.is_dir || static_cast<uint64_t>(entry.offset) + entry.size > buffer_.size()) {
        return {};
    }
    return std::span<const uint8_t>(buffer_.data() + entry.offset, entry.size);
}

bool U8Archive::read_file(const std::string& path, std::vector<uint8_t>& out) const {
    out.clear();
    const U8Entry* ent = find(path);
    if (!ent || ent->is_dir) {
        return false;
    }
    const auto bytes = entry_bytes(*ent);
    out.assign(bytes.begin(), bytes.end());
    return true;
}

} // namespace fzgx
