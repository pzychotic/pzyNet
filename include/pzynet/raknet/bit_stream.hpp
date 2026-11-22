#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/bit_io.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/core/span.hpp"
#include "pzynet/raknet/guid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
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

class BitStreamWriter;
class BitStreamReader;

namespace bit_stream_detail
{

template <class T>
struct always_false : std::false_type
{
};

// User-type hooks, found by argument-dependent lookup in the user type's namespace. The two
// declarations below stop ordinary lookup here, so unrelated read/write functions of enclosing
// namespaces (or ::read/::write from POSIX) are never considered. They take a type nobody passes and
// are never defined; deleted functions would do, but MSVC without /permissive- then fails to detect
// hooks that exist.
struct no_hook
{
};
void write(no_hook);
void read(no_hook);

template <class T, class = void>
struct has_write_hook : std::false_type
{
};
template <class T>
struct has_write_hook<T, std::void_t<decltype(write(std::declval<BitStreamWriter&>(), std::declval<const T&>()))>>
    : std::true_type
{
};
template <class T, class = void>
struct has_read_hook : std::false_type
{
};
template <class T>
struct has_read_hook<T, std::void_t<decltype(read(std::declval<BitStreamReader&>(), std::declval<T&>()))>>
    : std::true_type
{
};

template <class T>
void call_write_hook(BitStreamWriter& out, const T& value)
{
    static_assert(has_write_hook<T>::value,
                  "BitStreamWriter::write: unsupported type; for a user type, declare "
                  "void write(pzynet::raknet::BitStreamWriter&, const T&) in T's namespace");
    write(out, value);
}
template <class T>
void call_read_hook(BitStreamReader& in, T& value)
{
    static_assert(has_read_hook<T>::value,
                  "BitStreamReader::read: unsupported type; for a user type, declare "
                  "void read(pzynet::raknet::BitStreamReader&, T&) in T's namespace");
    read(in, value);
}

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
    /// Bytes the written bits occupy, (bit_count() + 7) / 8.
    std::size_t byte_count() const noexcept { return data_.size(); }
    /// The written bytes; the last one is zero-padded if bit_count() is not a multiple of 8.
    ByteView data() const noexcept { return ByteView(data_.data(), data_.size()); }

    /// Writes a value as RakNet's BitStream::Write does, at the current bit position (no alignment):
    /// - bool: one bit;
    /// - integers and enums: their full width (an enum's underlying type), two's complement, in the
    ///   stream's byte order;
    /// - float, double: their IEEE 754 bits, in the stream's byte order;
    /// - any other type: through a free function `void write(BitStreamWriter&, const T&)` declared in
    ///   T's namespace, found by argument-dependent lookup.
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
            bit_stream_detail::call_write_hook(*this, value);
    }

    /// Writes `bit_count` raw bits from `bytes`, as RakNet's WriteBits: whole bytes in order, most
    /// significant bit first, then the remaining bit_count % 8 bits from the next byte - its low bits
    /// when `right_aligned` (RakNet's default), its high bits otherwise. Requires
    /// bytes.size() * 8 >= bit_count; bits beyond the view are never read.
    void write_bits(ByteView bytes, std::size_t bit_count, bool right_aligned = true);
    /// Writes raw bytes at the current bit position, without alignment or byte swapping
    /// (RakNet's Write(const char*, unsigned)).
    void write_bytes(ByteView bytes) { write_bits(bytes, bytes.size() * 8); }
    /// Aligns to a byte boundary, then writes raw bytes (RakNet's WriteAlignedBytes). Aligns even
    /// when `bytes` is empty.
    void write_aligned_bytes(ByteView bytes)
    {
        align();
        write_bytes(bytes);
    }
    /// Pads with zero bits up to the next byte boundary; nothing happens on a boundary
    /// (RakNet's AlignWriteToByteBoundary).
    void align() noexcept { bits_ = byte_count() * 8; }

    // --- RakNet's own types and strings (bit_stream_types.cpp) ---

    /// Aligns to a byte boundary, then writes the low 24 bits of `value` as three bytes, least
    /// significant first, whatever the stream's byte order (RakNet's Write(uint24_t)).
    void write_uint24(std::uint32_t value);
    /// Writes an IPv4 endpoint as RakNet's Write(SystemAddress): the version byte 4, the four address
    /// bytes bitwise inverted, then the port, big-endian whatever the stream's byte order; unaligned.
    /// An IPv6 endpoint (an IPv4-mapped one included) fails with Errc::address_family_mismatch and
    /// writes nothing. write(endpoint) throws std::system_error instead.
    void write(const Endpoint& endpoint, std::error_code& ec);

    /// Writes a string as RakNet's Write(RakString) / RakString::Serialize: its length as a uint16 in
    /// the stream's byte order at the current bit position, then its bytes from the next byte boundary
    /// (aligning even when the string is empty). Every byte is written, a zero byte too; upstream would
    /// stop a C string there. A string of more than 65535 bytes, which RakNet would truncate silently,
    /// fails with Errc::invalid_argument and writes nothing; the overload without `ec` throws
    /// std::system_error.
    void write_string(std::string_view text, std::error_code& ec);
    void write_string(std::string_view text);
    /// Writes a wide string as upstream RakNet's RakWString::Serialize, which both Write(RakWString)
    /// and WriteCompressed(RakWString) / WriteCompressed(const wchar_t*) use - the only "compressed"
    /// strings upstream does not Huffman-code: the count of UTF-16 code units, then each code unit,
    /// all as uint16 in the stream's byte order with no alignment. More than 65535 code units fail
    /// with Errc::invalid_argument and write nothing (RakNet would write a truncated count).
    void write_wide_string(std::u16string_view text, std::error_code& ec);
    void write_wide_string(std::u16string_view text);

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
    /// Bits in the data, read or not: eight per byte of the view.
    std::size_t bit_count() const noexcept { return bits_.bit_position() + bits_.bits_remaining(); }
    /// Bits consumed so far.
    std::size_t bit_position() const noexcept { return bits_.bit_position(); }
    /// Bits left to read; padding bits of the last byte count, as they do in RakNet.
    std::size_t bits_remaining() const noexcept { return bits_.bits_remaining(); }

    /// Reads a value written as BitStreamWriter::write() writes it. A user type is default-constructed
    /// and then filled by a free function `void read(BitStreamReader&, T&)` declared in its namespace.
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
        {
            T value{};
            bit_stream_detail::call_read_hook(*this, value);
            return value;
        }
    }

    /// Reads into `value` (a user type through its read() hook, without default-constructing a
    /// new one) and returns ok(): false if this or any earlier read failed.
    template <class T>
    bool read(T& value)
    {
        if constexpr (std::is_arithmetic<T>::value || std::is_enum<T>::value)
            value = read<T>();
        else
            bit_stream_detail::call_read_hook(*this, value);
        return ok_;
    }

    /// Reads `bit_count` raw bits into `out`, the inverse of BitStreamWriter::write_bits(): the first
    /// (bit_count + 7) / 8 bytes of `out` are overwritten, a partial last byte holding the bits in its
    /// low (`right_aligned`) or high end and zeros elsewhere. Reading zero bits succeeds and does
    /// nothing (RakNet's ReadBits fails on zero). Fails, filling those bytes with zeros, if fewer bits
    /// are left or `out` is too small.
    bool read_bits(MutableByteView out, std::size_t bit_count, bool right_aligned = true);
    /// Fills `out` with raw bytes from the current bit position (RakNet's Read(char*, unsigned)).
    bool read_bytes(MutableByteView out) { return read_bits(out, out.size() * 8); }
    /// Aligns to a byte boundary, then fills `out` (RakNet's ReadAlignedBytes). Aligns even when
    /// `out` is empty, mirroring write_aligned_bytes(); RakNet instead fails without aligning.
    bool read_aligned_bytes(MutableByteView out)
    {
        align();
        return read_bytes(out);
    }
    /// Skips to the next byte boundary; nothing happens on a boundary (RakNet's
    /// AlignReadToByteBoundary). Never moves past the end of the data.
    void align() noexcept { bits_.align(); }
    /// Moves the read position forward (RakNet's IgnoreBits / IgnoreBytes). Skipping past the end
    /// fails the reader without moving; RakNet would only fail the next read.
    void skip_bits(std::size_t count) noexcept;
    void skip_bytes(std::size_t count) noexcept { skip_bits(count * 8); }

    // --- RakNet's own types and strings (bit_stream_types.cpp) ---

    /// Aligns to a byte boundary, then reads a value written by write_uint24().
    std::uint32_t read_uint24();
    /// Reads a string written by write_string() (RakNet's Read(RakString)): the uint16 length, then
    /// aligns - also for an empty string - and reads that many bytes. Returns "" on failure.
    std::string read_string();
    /// Reads a wide string written by write_wide_string(). Returns an empty string on failure.
    std::u16string read_wide_string();

    /// Latches ok() == false, e.g. from a user type's read() hook that finds an invalid value.
    void fail() noexcept { ok_ = false; }

private:
    bool read_bit();
    std::uint64_t read_integer(std::size_t width);
    bool need(std::size_t bits) noexcept;

    BitReader bits_;
    ByteOrder order_;
    bool ok_ = true;
};

// --- RakNet's own types: write()/read() hooks, found by argument-dependent lookup ---

/// A GUID is its 64-bit value in the stream's byte order, unaligned (RakNet's Write(RakNetGUID)).
inline void write(BitStreamWriter& out, const Guid& guid)
{
    out.write(guid.value);
}
inline void read(BitStreamReader& in, Guid& guid)
{
    guid.value = in.read<std::uint64_t>();
}

/// An endpoint as BitStreamWriter::write(const Endpoint&, std::error_code&) writes it; throws
/// std::system_error for an IPv6 endpoint.
void write(BitStreamWriter& out, const Endpoint& endpoint);
/// Reads an IPv4 endpoint. Any other version byte fails the reader, as in a RakNet build without
/// IPv6 support (the default).
void read(BitStreamReader& in, Endpoint& endpoint);

} // namespace raknet
} // namespace pzynet
