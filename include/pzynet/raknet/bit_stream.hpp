#pragma once

#include "pzynet/core/bit_io.hpp"
#include "pzynet/core/span.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace pzynet
{
namespace raknet
{

/// Byte order of multi-byte values in a BitStream. Stock RakNet writes big-endian; a RakNet build with
/// __BITSTREAM_NATIVE_END writes the host's order, which is little-endian on every common platform.
enum class ByteOrder : std::uint8_t
{
    big_endian,
    little_endian,
};

namespace bit_stream_detail
{

template <class T>
struct always_false : std::false_type
{
};

template <std::size_t Size>
struct uint_of_size;
template <>
struct uint_of_size<4>
{
    using type = std::uint32_t;
};
template <>
struct uint_of_size<8>
{
    using type = std::uint64_t;
};

template <class T>
std::uint64_t float_bits(T value) noexcept
{
    static_assert(std::numeric_limits<T>::is_iec559 && (sizeof(T) == 4 || sizeof(T) == 8),
                  "BitStream: only IEEE 754 float and double are supported");
    typename uint_of_size<sizeof(T)>::type bits;
    std::memcpy(&bits, &value, sizeof(T));
    return bits;
}

template <class T>
T float_from_bits(std::uint64_t value) noexcept
{
    static_assert(std::numeric_limits<T>::is_iec559 && (sizeof(T) == 4 || sizeof(T) == 8),
                  "BitStream: only IEEE 754 float and double are supported");
    const auto bits = static_cast<typename uint_of_size<sizeof(T)>::type>(value);
    T result;
    std::memcpy(&result, &bits, sizeof(T));
    return result;
}

} // namespace bit_stream_detail

/// Writes RakNet's BitStream format: bit-packed, most significant bit first, with no regard for byte
/// boundaries. A bool takes one bit. The stream owns its growing buffer; unused bits of the last byte
/// are zero.
class BitStreamWriter
{
public:
    explicit BitStreamWriter(ByteOrder order = ByteOrder::big_endian) noexcept : order_(order) {}

    ByteOrder byte_order() const noexcept { return order_; }
    /// Bits written so far.
    std::size_t bit_count() const noexcept { return bits_; }
    /// The written bytes; the last one is zero-padded if bit_count() is not a multiple of 8.
    ByteView data() const noexcept { return ByteView(data_.data(), data_.size()); }

    /// Writes a value as RakNet's BitStream::Write does, at the current bit position (no alignment):
    /// - bool: one bit;
    /// - integers and enums: their full width (an enum's underlying type), two's complement, in the
    ///   stream's byte order;
    /// - float, double: their IEEE 754 bits, in the stream's byte order.
    template <class T>
    void write(const T& value)
    {
        if constexpr (std::is_same<T, bool>::value)
            write_bit(value);
        else if constexpr (std::is_enum<T>::value)
            write(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_integral<T>::value)
            write_integer(static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)), sizeof(T));
        else if constexpr (std::is_floating_point<T>::value)
            write_integer(bit_stream_detail::float_bits(value), sizeof(T));
        else
            static_assert(bit_stream_detail::always_false<T>::value, "BitStreamWriter::write: unsupported type");
    }

private:
    void write_bit(bool value);
    void write_integer(std::uint64_t value, std::size_t width);

    std::vector<std::uint8_t> data_;
    std::size_t bits_ = 0;
    ByteOrder order_;
};

/// Reads RakNet's BitStream format from a non-owning view. A read that would run past the end returns
/// a zero value and latches ok() == false, so a whole message can be read and checked once; every read
/// after a failure fails too.
class BitStreamReader
{
public:
    explicit BitStreamReader(ByteView data, ByteOrder order = ByteOrder::big_endian) noexcept
        : bits_(data), order_(order)
    {
    }

    ByteOrder byte_order() const noexcept { return order_; }
    bool ok() const noexcept { return ok_; }
    /// Bits consumed so far.
    std::size_t bit_position() const noexcept { return bits_.bit_position(); }
    /// Bits left to read; padding bits of the last byte count, as they do in RakNet.
    std::size_t bits_remaining() const noexcept { return bits_.bits_remaining(); }

    /// Reads a value written as BitStreamWriter::write() writes it.
    template <class T>
    T read()
    {
        if constexpr (std::is_same<T, bool>::value)
            return read_bit();
        else if constexpr (std::is_enum<T>::value)
            return static_cast<T>(read<std::underlying_type_t<T>>());
        else if constexpr (std::is_integral<T>::value)
            return static_cast<T>(static_cast<std::make_unsigned_t<T>>(read_integer(sizeof(T))));
        else if constexpr (std::is_floating_point<T>::value)
            return bit_stream_detail::float_from_bits<T>(read_integer(sizeof(T)));
        else
            static_assert(bit_stream_detail::always_false<T>::value, "BitStreamReader::read: unsupported type");
    }

private:
    bool read_bit();
    std::uint64_t read_integer(std::size_t width);
    bool need(std::size_t bits) noexcept;

    BitReader bits_;
    ByteOrder order_;
    bool ok_ = true;
};

} // namespace raknet
} // namespace pzynet
