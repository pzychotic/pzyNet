#pragma once

#include "pzynet/core/bit_io.hpp"
#include "pzynet/core/span.hpp"

#include <cstddef>
#include <cstdint>
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

    /// One bit, as RakNet writes a bool.
    void write(bool value);

    /// Every integer type, in its full width and the stream's byte order.
    template <class T, std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value, int> = 0>
    void write(T value)
    {
        write_integer(static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)), sizeof(T));
    }

private:
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

    /// Reads a bool (one bit) or an integer of any type, written as BitStreamWriter::write() does.
    template <class T>
    T read()
    {
        static_assert(std::is_integral<T>::value, "BitStreamReader::read<T>: unsupported type");
        if constexpr (std::is_same<T, bool>::value)
            return read_bit();
        else
            return static_cast<T>(static_cast<std::make_unsigned_t<T>>(read_integer(sizeof(T))));
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
