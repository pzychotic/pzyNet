// RakNet's space-saving BitStream encodings: compressed integers and floats, half-precision floats and
// range-packed integers (the delta writes are templates in the header). Each one follows upstream RakNet
// 4.081's BitStream.h / BitStream.cpp, including its quirks, so the bytes match.

#include "pzynet/raknet/bit_stream.hpp"

#include <algorithm>
#include <cmath>

namespace pzynet
{
namespace raknet
{

namespace bit_stream_detail
{

std::uint16_t compress_unit_float(float value) noexcept
{
    const float clamped = std::isnan(value) ? -1.0f : std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::uint16_t>((clamped + 1.0f) * 32767.5f);
}

float expand_unit_float(std::uint16_t value) noexcept
{
    return static_cast<float>(value) / 32767.5f - 1.0f;
}

std::uint32_t compress_unit_double(double value) noexcept
{
    const double clamped = std::isnan(value) ? -1.0 : std::clamp(value, -1.0, 1.0);
    // RakNet converts straight to uint32_t, which is undefined for 1.0 (2^32); x86-64 compilers convert
    // through a 64-bit integer and keep its low 32 bits, giving 0. Do that explicitly.
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>((clamped + 1.0) * 2147483648.0));
}

double expand_unit_double(std::uint32_t value) noexcept
{
    return static_cast<double>(value) / 2147483648.0 - 1.0;
}

} // namespace bit_stream_detail

namespace
{

// The value's `width` bytes in the stream's byte order.
void stream_bytes(std::uint64_t value, std::size_t width, ByteOrder order, std::uint8_t* out) noexcept
{
    for (std::size_t i = 0; i < width; ++i)
    {
        const std::size_t byte = order == ByteOrder::big_endian ? width - 1 - i : i;
        out[i] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

std::uint64_t value_of_stream_bytes(const std::uint8_t* bytes, std::size_t width, ByteOrder order) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
    {
        const std::size_t byte = order == ByteOrder::big_endian ? width - 1 - i : i;
        value |= std::uint64_t{bytes[i]} << (byte * 8);
    }
    return value;
}

} // namespace

// RakNet's private BitStream::WriteCompressed(bytes, size, unsignedData = true).
void BitStreamWriter::write_compressed_integer(std::uint64_t value, std::size_t width)
{
    std::uint8_t bytes[8];
    stream_bytes(value, width, order_, bytes);
    for (std::size_t last = width - 1; last > 0; --last)
    {
        if (bytes[last] != 0)
        {
            write_bit(false);
            write_bits(ByteView(bytes, last + 1), (last + 1) * 8);
            return;
        }
        write_bit(true);
    }
    const bool nibble = (bytes[0] & 0xF0) == 0;
    write_bit(nibble);
    write_bits(ByteView(bytes, 1), nibble ? 4 : 8);
}

void BitStreamWriter::write_ranged_offset(std::uint64_t offset, std::size_t bit_count)
{
    std::uint8_t bytes[8];
    stream_bytes(offset, sizeof(bytes), ByteOrder::little_endian, bytes);
    write_bits(ByteView(bytes, sizeof(bytes)), bit_count);
}

void BitStreamWriter::write_float16(float value, float min, float max)
{
    assert(max > min);
    const float fraction = 65535.0f * (value - min) / (max - min);
    const float clamped = std::isnan(fraction) ? 0.0f : std::clamp(fraction, 0.0f, 65535.0f);
    write(static_cast<std::uint16_t>(clamped));
}

// RakNet's private BitStream::ReadCompressed(bytes, size, unsignedData = true).
std::uint64_t BitStreamReader::read_compressed_integer(std::size_t width)
{
    std::uint8_t bytes[8] = {};
    std::size_t last = width - 1;
    for (; last > 0; --last)
    {
        if (!read_bit())
            break;
    }
    if (!ok_)
        return 0;
    if (last > 0)
        read_bits(MutableByteView(bytes, last + 1), (last + 1) * 8);
    else
        read_bits(MutableByteView(bytes, 1), read_bit() ? 4 : 8);
    return ok_ ? value_of_stream_bytes(bytes, width, order_) : 0;
}

std::uint64_t BitStreamReader::read_ranged_offset(std::size_t bit_count)
{
    std::uint8_t bytes[8] = {};
    if (!read_bits(MutableByteView(bytes, sizeof(bytes)), bit_count))
        return 0;
    return value_of_stream_bytes(bytes, sizeof(bytes), ByteOrder::little_endian);
}

float BitStreamReader::read_float16(float min, float max)
{
    assert(max > min);
    const auto packed = read<std::uint16_t>();
    if (!ok_)
        return 0.0f;
    return std::clamp(min + (static_cast<float>(packed) / 65535.0f) * (max - min), min, max);
}

} // namespace raknet
} // namespace pzynet
