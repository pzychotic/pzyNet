#include "pzynet/raknet/bit_stream.hpp"

#include "pzynet/core/bit_io.hpp"

#include <algorithm>
#include <cassert>

namespace pzynet
{
namespace raknet
{

void BitStreamWriter::write_bit(bool value)
{
    BitWriter(data_, bits_).bit(value);
    ++bits_;
}

void BitStreamWriter::write_integer(std::uint64_t value, std::size_t width)
{
    BitWriter bits(data_, bits_);
    for (std::size_t i = 0; i < width; ++i)
    {
        const std::size_t byte = order_ == ByteOrder::big_endian ? width - 1 - i : i;
        bits.bits(static_cast<std::uint32_t>((value >> (byte * 8)) & 0xFFu), 8);
    }
    bits_ += width * 8;
}

void BitStreamWriter::write_bits(ByteView bytes, std::size_t bit_count, bool right_aligned)
{
    assert(bytes.size() * 8 >= bit_count);
    if (bit_count > bytes.size() * 8)
        bit_count = bytes.size() * 8;

    BitWriter bits(data_, bits_);
    const std::size_t whole = bit_count / 8;
    for (std::size_t i = 0; i < whole; ++i)
        bits.bits(bytes[i], 8);
    if (const auto tail = static_cast<unsigned>(bit_count % 8))
    {
        const unsigned byte = bytes[whole];
        bits.bits(right_aligned ? byte : byte >> (8 - tail), tail);
    }
    bits_ += bit_count;
}

bool BitStreamReader::need(std::size_t bits) noexcept
{
    if (ok_ && bits_.bits_remaining() >= bits)
        return true;
    ok_ = false;
    return false;
}

bool BitStreamReader::read_bit()
{
    return need(1) && bits_.bit();
}

std::uint64_t BitStreamReader::read_integer(std::size_t width)
{
    if (!need(width * 8))
        return 0;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
    {
        const std::uint64_t byte = bits_.bits(8);
        const std::size_t shift = order_ == ByteOrder::big_endian ? width - 1 - i : i;
        value |= byte << (shift * 8);
    }
    return value;
}

void BitStreamReader::skip_bits(std::size_t count) noexcept
{
    if (need(count))
        bits_.skip(count);
}

bool BitStreamReader::read_bits(MutableByteView out, std::size_t bit_count, bool right_aligned)
{
    const std::size_t byte_count = (bit_count + 7) / 8;
    const std::size_t touched = byte_count < out.size() ? byte_count : out.size();
    std::fill_n(out.begin(), touched, std::uint8_t{0});
    if (out.size() < byte_count || !need(bit_count))
    {
        ok_ = false;
        return false;
    }

    const std::size_t whole = bit_count / 8;
    for (std::size_t i = 0; i < whole; ++i)
        out[i] = static_cast<std::uint8_t>(bits_.bits(8));
    if (const auto tail = static_cast<unsigned>(bit_count % 8))
    {
        const std::uint32_t value = bits_.bits(tail);
        out[whole] = static_cast<std::uint8_t>(right_aligned ? value : value << (8 - tail));
    }
    return true;
}

} // namespace raknet
} // namespace pzynet
