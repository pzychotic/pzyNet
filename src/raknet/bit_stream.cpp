#include "pzynet/raknet/bit_stream.hpp"

#include "pzynet/core/bit_io.hpp"

namespace pzynet
{
namespace raknet
{

void BitStreamWriter::write(bool value)
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

} // namespace raknet
} // namespace pzynet
