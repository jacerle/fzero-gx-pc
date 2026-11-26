#include "game/dol_rel_loader.hpp"
#include "disc/sha1.hpp"

namespace fzgx {

namespace {

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

bool parse_dol_header(std::span<const uint8_t> dol_bytes, DolInfo& out_info) {
    if (dol_bytes.size() < 0x100) {
        return false;
    }

    out_info = {};
    const uint8_t* d = dol_bytes.data();

    for (int i = 0; i < 18; ++i) {
        const uint32_t foff = read_be32(d + 0x00 + static_cast<size_t>(i) * 4);
        const uint32_t addr = read_be32(d + 0x48 + static_cast<size_t>(i) * 4);
        const uint32_t sz = read_be32(d + 0x90 + static_cast<size_t>(i) * 4);
        if (sz == 0) {
            continue;
        }
        const bool is_text = (i < 7);
        out_info.sections.push_back({is_text ? i : (i - 7), is_text, foff, addr, sz});
        if (is_text) {
            out_info.total_code_bytes += sz;
        } else {
            out_info.total_data_bytes += sz;
        }
    }

    out_info.bss_address = read_be32(d + 0xD8);
    out_info.bss_size = read_be32(d + 0xDC);
    out_info.entry_point = read_be32(d + 0xE0);
    out_info.sha1_hex = Sha1::hash_bytes_hex(dol_bytes.data(), dol_bytes.size());
    return !out_info.sections.empty() && out_info.entry_point != 0;
}

bool parse_rel_header(std::span<const uint8_t> rel_bytes, RelInfo& out_info) {
    if (rel_bytes.size() < 0x40) {
        return false;
    }

    out_info = {};
    const uint8_t* r = rel_bytes.data();

    out_info.id = read_be32(r + 0x00);
    out_info.num_sections = read_be32(r + 0x0C);
    const uint32_t sec_info_off = read_be32(r + 0x10);
    out_info.version = read_be32(r + 0x1C);
    out_info.bss_size = read_be32(r + 0x20);
    out_info.rel_offset = read_be32(r + 0x24);
    out_info.imp_offset = read_be32(r + 0x28);
    out_info.imp_size = read_be32(r + 0x2C);
    out_info.prolog_section = r[0x30];
    out_info.epilog_section = r[0x31];
    out_info.unresolved_section = r[0x32];
    out_info.prolog_offset = read_be32(r + 0x34);
    out_info.epilog_offset = read_be32(r + 0x38);
    out_info.unresolved_offset = read_be32(r + 0x3C);

    if (out_info.num_sections == 0 ||
        static_cast<uint64_t>(sec_info_off) + static_cast<uint64_t>(out_info.num_sections) * 8ULL > rel_bytes.size()) {
        return false;
    }

    out_info.sections.reserve(out_info.num_sections);
    for (uint32_t i = 0; i < out_info.num_sections; ++i) {
        const uint8_t* s = r + sec_info_off + static_cast<size_t>(i) * 8;
        const uint32_t raw_off = read_be32(s + 0);
        const uint32_t sz = read_be32(s + 4);
        RelSection sec{};
        sec.index = i;
        sec.is_exec = (raw_off & 1u) != 0;
        sec.offset = raw_off & ~3u;
        sec.size = sz;
        out_info.sections.push_back(sec);
    }

    out_info.sha1_hex = Sha1::hash_bytes_hex(rel_bytes.data(), rel_bytes.size());
    return true;
}

} // namespace fzgx
