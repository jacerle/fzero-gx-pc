#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace fzgx {

class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> data, size_t offset = 0)
        : data_(data), pos_(offset <= data.size() ? offset : data.size()) {}

    size_t size() const { return data_.size(); }
    size_t tell() const { return pos_; }
    size_t remaining() const { return pos_ <= data_.size() ? (data_.size() - pos_) : 0; }
    bool ok() const { return !error_; }
    bool at_eof() const { return pos_ >= data_.size(); }
    std::span<const uint8_t> data() const { return data_; }

    bool seek(size_t pos) {
        if (pos > data_.size()) {
            error_ = true;
            return false;
        }
        pos_ = pos;
        return true;
    }

    bool skip(size_t count) {
        if (count > remaining()) {
            error_ = true;
            return false;
        }
        pos_ += count;
        return true;
    }

    uint8_t read_u8() {
        if (remaining() < 1) {
            error_ = true;
            return 0;
        }
        return data_[pos_++];
    }

    int8_t read_s8() {
        return static_cast<int8_t>(read_u8());
    }

    uint16_t read_be16() {
        if (remaining() < 2) {
            error_ = true;
            return 0;
        }
        const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(data_[pos_]) << 8) |
                                                 static_cast<uint16_t>(data_[pos_ + 1]));
        pos_ += 2;
        return v;
    }

    int16_t read_s16_be() {
        return static_cast<int16_t>(read_be16());
    }

    uint32_t read_be32() {
        if (remaining() < 4) {
            error_ = true;
            return 0;
        }
        const uint32_t v = (static_cast<uint32_t>(data_[pos_]) << 24) |
                           (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                           (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
                           static_cast<uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    uint32_t read_le32() {
        if (remaining() < 4) {
            error_ = true;
            return 0;
        }
        const uint32_t v = static_cast<uint32_t>(data_[pos_]) |
                           (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                           (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                           (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }

    float read_f32_be() {
        const uint32_t bits = read_be32();
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    std::span<const uint8_t> read_span(size_t count) {
        if (count > remaining()) {
            error_ = true;
            return {};
        }
        auto s = data_.subspan(pos_, count);
        pos_ += count;
        return s;
    }

    std::span<const uint8_t> subspan(size_t offset, size_t count) const {
        if (offset > data_.size() || count > data_.size() - offset) {
            return {};
        }
        return data_.subspan(offset, count);
    }

    static uint16_t peek_be16(std::span<const uint8_t> s, size_t offset, bool* valid = nullptr) {
        if (offset + 2 > s.size()) {
            if (valid) *valid = false;
            return 0;
        }
        if (valid) *valid = true;
        return static_cast<uint16_t>((static_cast<uint16_t>(s[offset]) << 8) |
                                     static_cast<uint16_t>(s[offset + 1]));
    }

    static uint32_t peek_be32(std::span<const uint8_t> s, size_t offset, bool* valid = nullptr) {
        if (offset + 4 > s.size()) {
            if (valid) *valid = false;
            return 0;
        }
        if (valid) *valid = true;
        return (static_cast<uint32_t>(s[offset]) << 24) |
               (static_cast<uint32_t>(s[offset + 1]) << 16) |
               (static_cast<uint32_t>(s[offset + 2]) << 8) |
               static_cast<uint32_t>(s[offset + 3]);
    }

    static uint32_t peek_le32(std::span<const uint8_t> s, size_t offset, bool* valid = nullptr) {
        if (offset + 4 > s.size()) {
            if (valid) *valid = false;
            return 0;
        }
        if (valid) *valid = true;
        return static_cast<uint32_t>(s[offset]) |
               (static_cast<uint32_t>(s[offset + 1]) << 8) |
               (static_cast<uint32_t>(s[offset + 2]) << 16) |
               (static_cast<uint32_t>(s[offset + 3]) << 24);
    }

    static float peek_f32_be(std::span<const uint8_t> s, size_t offset, bool* valid = nullptr) {
        const uint32_t bits = peek_be32(s, offset, valid);
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    static std::string read_cstr_at(std::span<const uint8_t> s, size_t offset, size_t max_len = 512) {
        if (offset >= s.size()) {
            return {};
        }
        size_t len = 0;
        while (len < max_len && (offset + len) < s.size() && s[offset + len] != 0) {
            ++len;
        }
        return std::string(reinterpret_cast<const char*>(s.data() + offset), len);
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_{0};
    bool error_{false};
};

} // namespace fzgx
