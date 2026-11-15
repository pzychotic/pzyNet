#pragma once

#include "pzynet/core/span.hpp"

#include <cstdint>
#include <vector>

namespace pzynet
{

/// MSB-first bit packing: the first bit written lands in bit 7 of the first byte.
class BitWriter
{
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    void bit(bool value)
    {
        if (used_ == 0)
            out_.push_back(0);
        if (value)
            out_.back() = static_cast<std::uint8_t>(out_.back() | (0x80u >> used_));
        used_ = (used_ + 1) & 7;
    }
    /// Writes the low `count` bits of `value`, most significant of those first. count <= 32.
    void bits(std::uint32_t value, unsigned count)
    {
        while (count-- > 0)
            bit(((value >> count) & 1u) != 0);
    }
    /// Skips to the next byte boundary; skipped bits are zero.
    void align() noexcept { used_ = 0; }

private:
    std::vector<std::uint8_t>& out_;
    unsigned used_ = 0; // bits already used in out_.back(); 0 = start a new byte
};

class BitReader
{
public:
    explicit BitReader(ByteView data) noexcept : data_(data) {}

    bool ok() const noexcept { return ok_; }
    /// Bytes fully or partly consumed so far.
    std::size_t bytes_consumed() const noexcept { return (pos_ + 7) / 8; }

    bool bit()
    {
        if (pos_ >= data_.size() * 8)
        {
            ok_ = false;
            return false;
        }
        const bool v = (data_[pos_ / 8] & (0x80u >> (pos_ & 7))) != 0;
        ++pos_;
        return v;
    }
    std::uint32_t bits(unsigned count)
    {
        std::uint32_t v = 0;
        while (count-- > 0)
            v = (v << 1) | (bit() ? 1u : 0u);
        return v;
    }
    void align() noexcept { pos_ = (pos_ + 7) & ~std::size_t{7}; }

private:
    ByteView data_;
    std::size_t pos_ = 0; // in bits
    bool ok_ = true;
};

} // namespace pzynet
