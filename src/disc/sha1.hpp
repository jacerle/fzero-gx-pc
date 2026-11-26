#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fzgx {

class Sha1 {
public:
    Sha1();

    void reset();
    void update(const uint8_t* data, size_t length);
    std::array<uint8_t, 20> final_bytes();
    std::string final_hex();

    static std::string hash_bytes_hex(const uint8_t* data, size_t length);
    static bool hash_file_hex(
        const std::filesystem::path& path,
        std::string& out_hex,
        const std::function<bool(uint64_t bytes_done, uint64_t total_bytes)>& progress_cb = {});

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[5]{};
    uint64_t bit_count_{0};
    uint8_t buffer_[64]{};
    size_t buffer_len_{0};
};

} // namespace fzgx
