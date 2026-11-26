#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fzgx {

struct DolSection {
    int index{0};
    bool is_text{false};
    uint32_t file_offset{0};
    uint32_t load_address{0};
    uint32_t size{0};
};

struct DolInfo {
    std::vector<DolSection> sections;
    uint32_t bss_address{0};
    uint32_t bss_size{0};
    uint32_t entry_point{0};
    uint64_t total_code_bytes{0};
    uint64_t total_data_bytes{0};
    std::string sha1_hex;
};

struct RelSection {
    uint32_t index{0};
    uint32_t offset{0};
    uint32_t size{0};
    bool is_exec{false};
};

struct RelInfo {
    uint32_t id{0};
    uint32_t version{0};
    uint32_t num_sections{0};
    uint32_t bss_size{0};
    uint32_t rel_offset{0};
    uint32_t imp_offset{0};
    uint32_t imp_size{0};
    uint8_t prolog_section{0};
    uint8_t epilog_section{0};
    uint8_t unresolved_section{0};
    uint32_t prolog_offset{0};
    uint32_t epilog_offset{0};
    uint32_t unresolved_offset{0};
    std::vector<RelSection> sections;
    std::string sha1_hex;
};

bool parse_dol_header(std::span<const uint8_t> dol_bytes, DolInfo& out_info);
bool parse_rel_header(std::span<const uint8_t> rel_bytes, RelInfo& out_info);

} // namespace fzgx
