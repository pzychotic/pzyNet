#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/bit_io.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/core/span.hpp"
#include "pzynet/raknet/guid.hpp"

#include <cassert>
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

// --- compressed, delta, half-float and range-packed encodings ---

// RakNet's compressed float in [-1, 1]: (unsigned short)((x + 1) * 32767.5f), after clamping.
std::uint16_t compress_unit_float(float value) noexcept;
float expand_unit_float(std::uint16_t value) noexcept;
// RakNet's compressed double in [-1, 1]: (uint32_t)((x + 1) * 2^31), after clamping.
std::uint32_t compress_unit_double(double value) noexcept;
double expand_unit_double(std::uint32_t value) noexcept;

/// Bits needed to hold `value`: 0 for 0, else the index of its highest set bit plus one.
constexpr std::size_t bit_width(std::uint64_t value) noexcept
{
    std::size_t bits = 0;
    for (; value != 0; value >>= 1)
        ++bits;
    return bits;
}

template <class T>
using ranged_unsigned = std::make_unsigned_t<T>;

template <class T>
void check_ranged_type()
{
    static_assert(std::is_integral<T>::value && !std::is_same<T, bool>::value,
                  "BitStream ranged integers: T must be an integer type");
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

    // --- compressed, delta, half-float and range-packed encodings ---

    /// Writes a value as RakNet's BitStream::WriteCompressed does:
    /// - integers and enums (an enum as its underlying type): the value's bytes are taken in the
    ///   stream's byte order and looked at from the last one back; each 0x00 byte costs a single 1 bit,
    ///   and at the first other byte a 0 bit is written followed by that byte and all before it. If only
    ///   the first byte is left, it takes a 1 bit and its low nibble when its high nibble is zero, else a
    ///   0 bit and all 8 bits. Big-endian streams therefore drop *trailing* zero bytes: small numbers do
    ///   not shrink there (5u takes 33 bits), they do in little-endian streams. Only 0x00 bytes are ever
    ///   dropped; RakNet's 0xFF branch for negative numbers is never reached, so -1 takes 33 bits too.
    /// - float, double in [-1, 1] (clamped): a uint16 (x + 1) * 32767.5 or a uint32 (x + 1) * 2^31,
    ///   truncated, in the stream's byte order. As in RakNet on x86-64, a double of exactly 1.0
    ///   overflows to 0 and reads back as -1.0.
    /// - bool and any other type: written unchanged, as by write() (RakNet does the same for bool,
    ///   RakNetGUID, SystemAddress and uint24_t).
    template <class T>
    void write_compressed(const T& value)
    {
        if constexpr (std::is_same<T, bool>::value)
            write_bit(value);
        else if constexpr (std::is_enum<T>::value)
            write_compressed(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_integral<T>::value)
            write_compressed_integer(static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value)),
                                     sizeof(T));
        else if constexpr (std::is_same<T, float>::value)
            write(bit_stream_detail::compress_unit_float(value));
        else if constexpr (std::is_same<T, double>::value)
            write(bit_stream_detail::compress_unit_double(value));
        else
            write(value);
    }

    /// RakNet's WriteDelta: a 0 bit if `current == last`, else a 1 bit and write(current). A bool is
    /// written as itself, without the flag bit.
    template <class T>
    void write_delta(const T& current, const T& last)
    {
        if constexpr (std::is_same<T, bool>::value)
            write_bit(current);
        else if (current == last)
            write_bit(false);
        else
        {
            write_bit(true);
            write(current);
        }
    }
    /// RakNet's WriteDelta without a last value: a 1 bit and write(current) - for a bool too, which
    /// makes two bits that BitStreamReader::read_delta (one bit for a bool, as in RakNet) does not read.
    template <class T>
    void write_delta(const T& current)
    {
        write_bit(true);
        write(current);
    }
    /// RakNet's WriteCompressedDelta: as write_delta(current, last), with write_compressed(current).
    template <class T>
    void write_compressed_delta(const T& current, const T& last)
    {
        if constexpr (std::is_same<T, bool>::value)
            write_bit(current);
        else if (current == last)
            write_bit(false);
        else
        {
            write_bit(true);
            write_compressed(current);
        }
    }
    /// RakNet's WriteCompressedDelta without a last value: a 1 bit and write_compressed(current); a
    /// bool is written as itself, without the flag bit.
    template <class T>
    void write_compressed_delta(const T& current)
    {
        if constexpr (!std::is_same<T, bool>::value)
            write_bit(true);
        write_compressed(current);
    }

    /// RakNet's WriteFloat16: `value` as a uint16 fraction of [min, max], 65535 * (value - min) /
    /// (max - min) computed in float, clamped to [0, 65535] and truncated, in the stream's byte order.
    /// NaN is written as 0. Requires max > min.
    void write_float16(float value, float min, float max);

    /// RakNet's WriteBitsFromIntegerRange: `value - min` in the fewest bits that hold `max - min`
    /// (nothing at all when min == max), least significant byte first whatever the stream's byte
    /// order, the bits of a partial last byte taken from its low end. With `allow_outside_range`, a
    /// flag bit comes first, and a value outside [min, max] is written in full by write() after a 1
    /// bit. Without it, the value must be in range (only its low bits would be written).
    template <class T>
    void write_ranged(T value, T min, T max, bool allow_outside_range = false)
    {
        bit_stream_detail::check_ranged_type<T>();
        using U = bit_stream_detail::ranged_unsigned<T>;
        assert(max >= min);
        assert(allow_outside_range || (value >= min && value <= max));
        if (allow_outside_range)
        {
            const bool outside = value < min || value > max;
            write_bit(outside);
            if (outside)
            {
                write(value);
                return;
            }
        }
        const U offset = static_cast<U>(static_cast<U>(value) - static_cast<U>(min));
        const U span = static_cast<U>(static_cast<U>(max) - static_cast<U>(min));
        write_ranged_offset(offset, bit_stream_detail::bit_width(span));
    }

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
    void write_compressed_integer(std::uint64_t value, std::size_t width);
    void write_ranged_offset(std::uint64_t offset, std::size_t bit_count);

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

    // --- compressed, delta, half-float and range-packed encodings ---

    /// Reads a value written by BitStreamWriter::write_compressed(). Compressed floats come back
    /// quantised: u16 / 32767.5 - 1 and u32 / 2^31 - 1.
    template <class T>
    T read_compressed()
    {
        if constexpr (std::is_same<T, bool>::value)
            return read_bit();
        else if constexpr (std::is_enum<T>::value)
            return static_cast<T>(read_compressed<std::underlying_type_t<T>>());
        else if constexpr (std::is_integral<T>::value)
            return static_cast<T>(static_cast<std::make_unsigned_t<T>>(read_compressed_integer(sizeof(T))));
        else if constexpr (std::is_same<T, float>::value)
        {
            const auto packed = read<std::uint16_t>();
            return ok_ ? bit_stream_detail::expand_unit_float(packed) : 0.0f;
        }
        else if constexpr (std::is_same<T, double>::value)
        {
            const auto packed = read<std::uint32_t>();
            return ok_ ? bit_stream_detail::expand_unit_double(packed) : 0.0;
        }
        else
            return read<T>();
    }
    /// read_compressed<T>() into `value`; returns ok().
    template <class T>
    bool read_compressed(T& value)
    {
        if constexpr (std::is_arithmetic<T>::value || std::is_enum<T>::value)
            value = read_compressed<T>();
        else
            read(value);
        return ok_;
    }

    /// Reads what BitStreamWriter::write_delta() wrote: a flag bit, then the value if the flag is 1;
    /// on a 0 flag `value` keeps what it had. A bool is a single bit with no flag. Returns ok().
    template <class T>
    bool read_delta(T& value)
    {
        if constexpr (std::is_same<T, bool>::value)
            value = read_bit();
        else if (read_bit())
            read(value);
        return ok_;
    }
    /// As read_delta(), for BitStreamWriter::write_compressed_delta().
    template <class T>
    bool read_compressed_delta(T& value)
    {
        if constexpr (std::is_same<T, bool>::value)
            value = read_bit();
        else if (read_bit())
            read_compressed(value);
        return ok_;
    }

    /// Reads what BitStreamWriter::write_float16() wrote: min + u16 / 65535 * (max - min), clamped to
    /// [min, max]. Returns 0 if the read fails.
    float read_float16(float min, float max);

    /// Reads what BitStreamWriter::write_ranged() wrote with the same min, max and allow_outside_range.
    /// When min == max no bits are read and min is returned (RakNet's read fails there, although its
    /// writer wrote nothing).
    template <class T>
    T read_ranged(T min, T max, bool allow_outside_range = false)
    {
        bit_stream_detail::check_ranged_type<T>();
        using U = bit_stream_detail::ranged_unsigned<T>;
        assert(max >= min);
        if (allow_outside_range && read_bit())
            return read<T>();
        const U span = static_cast<U>(static_cast<U>(max) - static_cast<U>(min));
        const auto offset = static_cast<U>(read_ranged_offset(bit_stream_detail::bit_width(span)));
        if (!ok_)
            return T{};
        return static_cast<T>(static_cast<U>(offset + static_cast<U>(min)));
    }

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
    std::uint64_t read_compressed_integer(std::size_t width);
    std::uint64_t read_ranged_offset(std::size_t bit_count);

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
