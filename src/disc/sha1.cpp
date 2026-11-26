#include "disc/sha1.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace fzgx {

namespace {

inline uint32_t rotl32(uint32_t v, uint32_t n) {
    return (v << n) | (v >> (32U - n));
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

Sha1::Sha1() {
    reset();
}

void Sha1::reset() {
    state_[0] = 0x67452301u;
    state_[1] = 0xEFCDAB89u;
    state_[2] = 0x98BADCFEu;
    state_[3] = 0x10325476u;
    state_[4] = 0xC3D2E1F0u;
    bit_count_ = 0;
    buffer_len_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha1::transform(const uint8_t block[64]) {
    uint32_t w[80]{};
    for (int i = 0; i < 16; ++i) {
        w[i] = read_be32(block + static_cast<size_t>(i) * 4);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return;
    }
    bit_count_ += static_cast<uint64_t>(length) * 8ULL;

    size_t offset = 0;
    if (buffer_len_ > 0) {
        const size_t copy_len = std::min(64 - buffer_len_, length);
        std::memcpy(buffer_ + buffer_len_, data, copy_len);
        buffer_len_ += copy_len;
        offset += copy_len;
        if (buffer_len_ == 64) {
            transform(buffer_);
            buffer_len_ = 0;
        }
    }

    while (offset + 64 <= length) {
        transform(data + offset);
        offset += 64;
    }

    if (offset < length) {
        buffer_len_ = length - offset;
        std::memcpy(buffer_, data + offset, buffer_len_);
    }
}

std::array<uint8_t, 20> Sha1::final_bytes() {
    uint8_t pad = 0x80;
    uint64_t total_bits = bit_count_;

    update(&pad, 1);
    pad = 0x00;
    while (buffer_len_ != 56) {
        update(&pad, 1);
    }

    uint8_t len_be[8]{};
    for (int i = 0; i < 8; ++i) {
        len_be[7 - i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFFU);
    }
    // Transform final block directly without mutating bit_count_ again.
    std::memcpy(buffer_ + 56, len_be, 8);
    transform(buffer_);

    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFFU);
        digest[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFFU);
        digest[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFFU);
        digest[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFFU);
    }
    return digest;
}

std::string Sha1::final_hex() {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = final_bytes();
    std::string out;
    out.reserve(40);
    for (uint8_t b : digest) {
        out.push_back(kHex[(b >> 4) & 0xFU]);
        out.push_back(kHex[b & 0xFU]);
    }
    return out;
}

std::string Sha1::hash_bytes_hex(const uint8_t* data, size_t length) {
    Sha1 sha;
    sha.update(data, length);
    return sha.final_hex();
}

bool Sha1::hash_file_hex(
    const std::filesystem::path& path,
    std::string& out_hex,
    const std::function<bool(uint64_t bytes_done, uint64_t total_bytes)>& progress_cb) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const uint64_t total_size = static_cast<uint64_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    Sha1 sha;
    std::vector<uint8_t> buf(1024 * 1024);
    uint64_t done = 0;
    while (file) {
        file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<size_t>(file.gcount());
        if (got > 0) {
            sha.update(buf.data(), got);
            done += got;
            if (progress_cb && !progress_cb(done, total_size)) {
                return false;
            }
        }
    }
    if (done != total_size) {
        return false;
    }
    out_hex = sha.final_hex();
    return true;
}

} // namespace fzgx
