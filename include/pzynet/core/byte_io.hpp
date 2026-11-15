#pragma once

#include "pzynet/core/span.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace pzynet
{

/// Appends fixed-width integers to a byte vector. Suffix = byte order on the wire.
class ByteWriter
{
public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    std::size_t size() const noexcept { return out_.size(); }

    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16_be(std::uint16_t v) { put_be(v, 2); }
    void u32_be(std::uint32_t v) { put_be(v, 4); }
    void u64_be(std::uint64_t v) { put_be(v, 8); }
    /// 24-bit little-endian; the upper byte of `v` is ignored.
    void u24_le(std::uint32_t v)
    {
        out_.push_back(static_cast<std::uint8_t>(v));
        out_.push_back(static_cast<std::uint8_t>(v >> 8));
        out_.push_back(static_cast<std::uint8_t>(v >> 16));
    }
    void f32_be(float v)
    {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32_be(bits);
    }
    void bytes(ByteView v) { out_.insert(out_.end(), v.begin(), v.end()); }
    void zeros(std::size_t count) { out_.insert(out_.end(), count, std::uint8_t{0}); }
    /// Zero-pads up to `total` bytes; no-op if already that long.
    void pad_to(std::size_t total)
    {
        if (out_.size() < total)
            out_.resize(total, std::uint8_t{0});
    }

private:
    void put_be(std::uint64_t v, int width)
    {
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
            out_.push_back(static_cast<std::uint8_t>(v >> shift));
    }

    std::vector<std::uint8_t>& out_;
};

/// Bounds-checked reader. A read past the end returns 0 and latches ok() == false, so a parser can
/// read a whole structure and check once.
class ByteReader
{
public:
    explicit ByteReader(ByteView data) noexcept : data_(data) {}

    bool ok() const noexcept { return ok_; }
    std::size_t remaining() const noexcept { return data_.size() - pos_; }
    std::size_t position() const noexcept { return pos_; }

    std::uint8_t u8() { return static_cast<std::uint8_t>(get_be(1)); }
    std::uint16_t u16_be() { return static_cast<std::uint16_t>(get_be(2)); }
    std::uint32_t u32_be() { return static_cast<std::uint32_t>(get_be(4)); }
    std::uint64_t u64_be() { return get_be(8); }
    std::uint32_t u24_le()
    {
        if (!need(3))
            return 0;
        const std::uint8_t* p = data_.data() + pos_;
        pos_ += 3;
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16);
    }
    float f32_be()
    {
        const std::uint32_t bits = u32_be();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    /// View of the next `count` bytes; empty (and !ok()) if fewer remain.
    ByteView bytes(std::size_t count)
    {
        if (!need(count))
            return {};
        ByteView v = data_.subspan(pos_, count);
        pos_ += count;
        return v;
    }
    ByteView rest() { return bytes(remaining()); }
    void skip(std::size_t count) { (void)bytes(count); }

private:
    bool need(std::size_t count) noexcept
    {
        if (!ok_ || remaining() < count)
        {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::uint64_t get_be(std::size_t width)
    {
        if (!need(width))
            return 0;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < width; ++i)
            v = (v << 8) | data_[pos_ + i];
        pos_ += width;
        return v;
    }

    ByteView data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace pzynet
