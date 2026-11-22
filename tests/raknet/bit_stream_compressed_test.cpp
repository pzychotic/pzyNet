// Byte-exact checks of RakNet's space-saving BitStream encodings: compressed integers and floats, delta
// writes, half-precision floats and range-packed integers. Expected bytes are worked out by hand from
// upstream RakNet 4.081 (BitStream.h / BitStream.cpp), bit by bit, most significant bit first.

#include "pzynet/raknet/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pzynet;
using namespace pzynet::raknet;
using Bytes = std::vector<std::uint8_t>;

namespace
{

Bytes bytes_of(const BitStreamWriter& writer)
{
    return Bytes(writer.data().begin(), writer.data().end());
}

template <class T>
Bytes compressed(T value, ByteOrder order, std::size_t expected_bits)
{
    BitStreamWriter writer(order);
    writer.write_compressed(value);
    CHECK(writer.bit_count() == expected_bits);
    return bytes_of(writer);
}

template <class T>
T read_back_compressed(const Bytes& data, ByteOrder order, std::size_t expected_bits)
{
    BitStreamReader reader(data, order);
    const T value = reader.read_compressed<T>();
    CHECK(reader.ok());
    CHECK(reader.bit_position() == expected_bits);
    return value;
}

} // namespace

// --- compressed integers --------------------------------------------------------------------------------
//
// RakNet looks at the value's bytes in stream order from the last one back: each zero byte costs one 1 bit;
// at the first non-zero byte it writes a 0 bit and every byte up to and including that one. The last byte
// left is written as a 1 bit and its low nibble if its high nibble is zero, else a 0 bit and all 8 bits.
// Only 0x00 bytes are ever dropped: RakNet's code has a 0xFF branch for negative numbers, but every caller
// passes unsignedData = true, so negative numbers are written in full.

TEST_CASE("BitStream: a compressed one-byte value takes a nibble when it fits, else a whole byte",
          "[raknet][bitstream][compressed]")
{
    for (ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        CHECK(compressed(std::uint8_t{0x00}, order, 5) == Bytes{0x80});       // 1 0000
        CHECK(compressed(std::uint8_t{0x0F}, order, 5) == Bytes{0xF8});       // 1 1111
        CHECK(compressed(std::uint8_t{0x10}, order, 9) == Bytes{0x08, 0x00}); // 0 00010000
        CHECK(compressed(std::uint8_t{0xAB}, order, 9) == Bytes{0x55, 0x80}); // 0 10101011
        CHECK(compressed(std::int8_t{-1}, order, 9) == Bytes{0x7F, 0x80});    // 0 11111111
        CHECK(compressed(char{'\n'}, order, 5) == Bytes{0xD0});               // 1 1010
    }
}

TEST_CASE("BitStream: a big-endian stream compresses away trailing zero bytes, so small numbers do not shrink",
          "[raknet][bitstream][compressed]")
{
    const ByteOrder be = ByteOrder::big_endian;
    // 0: 1 1 1 for bytes 3..1, then 1 0000 for byte 0
    CHECK(compressed(std::uint32_t{0}, be, 8) == Bytes{0xF0});
    // 5 = 00 00 00 05: the last byte is non-zero, so 0 and all four bytes
    CHECK(compressed(std::uint32_t{5}, be, 33) == Bytes{0x00, 0x00, 0x00, 0x02, 0x80});
    // 05 00 00 00: 1 1 1, then 1 0101
    CHECK(compressed(std::uint32_t{0x05000000}, be, 8) == Bytes{0xF5});
    // 12 34 00 00: 1 1, then 0 and bytes 12 34
    CHECK(compressed(std::uint32_t{0x12340000}, be, 19) == Bytes{0xC2, 0x46, 0x80});
    // 01 00: 1, then 1 0001
    CHECK(compressed(std::uint16_t{0x0100}, be, 6) == Bytes{0xC4});
    CHECK(compressed(std::uint16_t{1}, be, 17) == Bytes{0x00, 0x00, 0x80});
}

TEST_CASE("BitStream: a little-endian stream compresses away leading zero bytes", "[raknet][bitstream][compressed]")
{
    const ByteOrder le = ByteOrder::little_endian;
    CHECK(compressed(std::uint32_t{0}, le, 8) == Bytes{0xF0});
    CHECK(compressed(std::uint32_t{5}, le, 8) == Bytes{0xF5});
    CHECK(compressed(std::uint32_t{0x05000000}, le, 33) == Bytes{0x00, 0x00, 0x00, 0x02, 0x80});
    // F0 00: 1, then 0 11110000
    CHECK(compressed(std::uint16_t{0x00F0}, le, 10) == Bytes{0xBC, 0x00});
    // 34 12 00 00 00 00 00 00: six 1 bits, then 0 and bytes 34 12
    CHECK(compressed(std::uint64_t{0x1234}, le, 23) == Bytes{0xFC, 0x68, 0x24});
}

TEST_CASE("BitStream: compressed negative numbers and all-ones values are written in full",
          "[raknet][bitstream][compressed]")
{
    for (ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        CHECK(compressed(std::int32_t{-1}, order, 33) == Bytes{0x7F, 0xFF, 0xFF, 0xFF, 0x80});
        CHECK(compressed(std::int16_t{-1}, order, 17) == Bytes{0x7F, 0xFF, 0x80});
        CHECK(compressed(UINT64_MAX, order, 65) ==
              Bytes{0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80});
    }
    // -2 = FF FF FF FE (big-endian), FE FF FF FF (little-endian)
    CHECK(compressed(std::int32_t{-2}, ByteOrder::big_endian, 33) == Bytes{0x7F, 0xFF, 0xFF, 0xFF, 0x00});
    CHECK(compressed(std::int32_t{-2}, ByteOrder::little_endian, 33) == Bytes{0x7F, 0x7F, 0xFF, 0xFF, 0x80});
}

TEST_CASE("BitStream: compressed enums use the width of their underlying type", "[raknet][bitstream][compressed]")
{
    enum class Small : std::uint8_t
    {
        three = 3,
    };
    enum class Wide : std::uint32_t
    {
        big = 0x07000000,
    };
    CHECK(compressed(Small::three, ByteOrder::big_endian, 5) == Bytes{0x98});
    CHECK(compressed(Wide::big, ByteOrder::big_endian, 8) == Bytes{0xF7});
}

TEST_CASE("BitStream: compressed integers read back from both byte orders", "[raknet][bitstream][compressed]")
{
    const ByteOrder be = ByteOrder::big_endian;
    const ByteOrder le = ByteOrder::little_endian;
    CHECK(read_back_compressed<std::uint32_t>({0xF0}, be, 8) == 0u);
    CHECK(read_back_compressed<std::uint32_t>({0x00, 0x00, 0x00, 0x02, 0x80}, be, 33) == 5u);
    CHECK(read_back_compressed<std::uint32_t>({0xF5}, be, 8) == 0x05000000u);
    CHECK(read_back_compressed<std::uint32_t>({0xC2, 0x46, 0x80}, be, 19) == 0x12340000u);
    CHECK(read_back_compressed<std::uint32_t>({0xF5}, le, 8) == 5u);
    CHECK(read_back_compressed<std::uint64_t>({0xFC, 0x68, 0x24}, le, 23) == 0x1234u);
    CHECK(read_back_compressed<std::uint16_t>({0xBC, 0x00}, le, 10) == 0x00F0u);
    CHECK(read_back_compressed<std::int32_t>({0x7F, 0x7F, 0xFF, 0xFF, 0x80}, le, 33) == -2);
    CHECK(read_back_compressed<std::int8_t>({0x7F, 0x80}, be, 9) == -1);
    CHECK(read_back_compressed<std::uint8_t>({0xF8}, be, 5) == 0x0F);
}

TEST_CASE("BitStream: a truncated compressed integer fails the reader", "[raknet][bitstream][compressed]")
{
    // 0 then only three of the four bytes
    const Bytes data{0x00, 0x00, 0x00, 0x02};
    BitStreamReader reader(data);
    std::uint32_t value = 7;
    CHECK_FALSE(reader.read_compressed(value));
    CHECK(value == 0);
    CHECK_FALSE(reader.ok());

    BitStreamReader empty(ByteView{});
    CHECK(empty.read_compressed<std::uint8_t>() == 0);
    CHECK_FALSE(empty.ok());
}

// --- compressed floats in [-1, 1] -------------------------------------------------------------------------

TEST_CASE("BitStream: a compressed float is a uint16 of (x + 1) * 32767.5 in the stream's byte order",
          "[raknet][bitstream][compressed]")
{
    const ByteOrder be = ByteOrder::big_endian;
    CHECK(compressed(-1.0f, be, 16) == Bytes{0x00, 0x00});
    CHECK(compressed(0.0f, be, 16) == Bytes{0x7F, 0xFF}); // 32767.5 truncated
    CHECK(compressed(0.5f, be, 16) == Bytes{0xBF, 0xFF}); // 49151.25
    CHECK(compressed(1.0f, be, 16) == Bytes{0xFF, 0xFF});
    CHECK(compressed(0.0f, ByteOrder::little_endian, 16) == Bytes{0xFF, 0x7F});
    // out of range: clamped
    CHECK(compressed(7.0f, be, 16) == Bytes{0xFF, 0xFF});
    CHECK(compressed(-1.5f, be, 16) == Bytes{0x00, 0x00});

    CHECK(read_back_compressed<float>({0x00, 0x00}, be, 16) == -1.0f);
    CHECK(read_back_compressed<float>({0xFF, 0xFF}, be, 16) == 1.0f);
    // 32767 / 32767.5 - 1 = -1 / 65535
    CHECK(std::fabs(read_back_compressed<float>({0x7F, 0xFF}, be, 16) + 1.5259022e-5f) < 1e-7f);
    CHECK(std::fabs(read_back_compressed<float>({0xFF, 0x7F}, ByteOrder::little_endian, 16) + 1.5259022e-5f) <
          1e-7f);
}

TEST_CASE("BitStream: a compressed double is a uint32 of (x + 1) * 2^31, and 1.0 wraps to 0 as in RakNet",
          "[raknet][bitstream][compressed]")
{
    const ByteOrder be = ByteOrder::big_endian;
    CHECK(compressed(-1.0, be, 32) == Bytes{0x00, 0x00, 0x00, 0x00});
    CHECK(compressed(0.0, be, 32) == Bytes{0x80, 0x00, 0x00, 0x00});
    CHECK(compressed(0.5, be, 32) == Bytes{0xC0, 0x00, 0x00, 0x00});
    CHECK(compressed(-0.75, be, 32) == Bytes{0x20, 0x00, 0x00, 0x00});
    CHECK(compressed(-0.75, ByteOrder::little_endian, 32) == Bytes{0x00, 0x00, 0x00, 0x20});
    // (1 + 1) * 2^31 = 2^32 does not fit; RakNet on x86-64 keeps the low 32 bits
    CHECK(compressed(1.0, be, 32) == Bytes{0x00, 0x00, 0x00, 0x00});
    // 1 - 2^-30 still fits: 2^32 - 2
    CHECK(compressed(1.0 - std::ldexp(1.0, -30), be, 32) == Bytes{0xFF, 0xFF, 0xFF, 0xFE});

    CHECK(read_back_compressed<double>({0x80, 0x00, 0x00, 0x00}, be, 32) == 0.0);
    CHECK(read_back_compressed<double>({0x00, 0x00, 0x00, 0x20}, ByteOrder::little_endian, 32) == -0.75);
    CHECK(read_back_compressed<double>({0xFF, 0xFF, 0xFF, 0xFF}, be, 32) == 1.0 - std::ldexp(1.0, -31));
}

// --- types compressed writes leave unchanged ---------------------------------------------------------------

namespace unchanged
{
struct Tag
{
    std::uint16_t value = 0;
};
void write(BitStreamWriter& out, const Tag& tag)
{
    out.write(tag.value);
}
void read(BitStreamReader& in, Tag& tag)
{
    in.read(tag.value);
}
} // namespace unchanged

TEST_CASE("BitStream: compressed bools and user types are written as by write()", "[raknet][bitstream][compressed]")
{
    BitStreamWriter writer;
    writer.write_compressed(true);
    writer.write_compressed(unchanged::Tag{0x0001});
    writer.write_compressed(false);
    // 1 | 00000000 00000001 | 0
    CHECK(writer.bit_count() == 18);
    CHECK(bytes_of(writer) == Bytes{0x80, 0x00, 0x80});

    BitStreamReader reader(writer.data());
    CHECK(reader.read_compressed<bool>() == true);
    unchanged::Tag tag;
    CHECK(reader.read_compressed(tag));
    CHECK(tag.value == 1);
    CHECK(reader.read_compressed<bool>() == false);
    CHECK(reader.bit_position() == 18);
}

// --- delta writes ---------------------------------------------------------------------------------------------

TEST_CASE("BitStream: a delta write is one 0 bit for an unchanged value, else a 1 bit and the value",
          "[raknet][bitstream][delta]")
{
    BitStreamWriter writer;
    writer.write_delta(std::uint8_t{5}, std::uint8_t{5});        // 0
    writer.write_delta(std::uint16_t{0x1234}, std::uint16_t{0}); // 1 00010010 00110100
    writer.write_delta(0.0f, -0.0f);                             // 0: compared with ==
    writer.write_delta(std::uint8_t{7});                         // 1 00000111
    CHECK(writer.bit_count() == 28);
    CHECK(bytes_of(writer) == Bytes{0x44, 0x8D, 0x10, 0x70});

    BitStreamReader reader(writer.data());
    std::uint8_t a = 5;
    std::uint16_t b = 0;
    float c = -0.0f;
    std::uint8_t d = 0;
    CHECK(reader.read_delta(a));
    CHECK(reader.read_delta(b));
    CHECK(reader.read_delta(c));
    CHECK(reader.read_delta(d));
    CHECK(a == 5);
    CHECK(b == 0x1234);
    CHECK(std::signbit(c)); // unchanged: still the caller's -0.0
    CHECK(d == 7);
    CHECK(reader.bit_position() == 28);
}

TEST_CASE("BitStream: a bool delta is the bool alone, except without a last value", "[raknet][bitstream][delta]")
{
    BitStreamWriter writer;
    writer.write_delta(true, true);              // 1
    writer.write_compressed_delta(false, false); // 0
    writer.write_compressed_delta(true);         // 1
    writer.write_delta(false);                   // 1 0: RakNet's WriteDelta(bool) has a flag bit here
    CHECK(writer.bit_count() == 5);
    CHECK(bytes_of(writer) == Bytes{0xB0});

    BitStreamReader reader(writer.data());
    bool a = false, b = true, c = false, flag = false, d = true;
    CHECK(reader.read_delta(a));
    CHECK(reader.read_compressed_delta(b));
    CHECK(reader.read_compressed_delta(c));
    CHECK(reader.read_delta(flag)); // as in RakNet, read_delta(bool) reads one bit: the flag
    CHECK(reader.read_delta(d));
    CHECK(a);
    CHECK_FALSE(b);
    CHECK(c);
    CHECK(flag);
    CHECK_FALSE(d);
}

TEST_CASE("BitStream: a compressed delta write compresses the changed value", "[raknet][bitstream][delta]")
{
    for (ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        BitStreamWriter writer(order);
        writer.write_compressed_delta(std::uint32_t{0}, std::uint32_t{1}); // 1 11110000
        writer.write_compressed_delta(std::uint32_t{9}, std::uint32_t{9}); // 0
        writer.write_compressed_delta(std::uint8_t{3});                    // 1 10011
        CHECK(writer.bit_count() == 16);
        CHECK(bytes_of(writer) == Bytes{0xF8, 0x33});

        BitStreamReader reader(writer.data(), order);
        std::uint32_t a = 1, b = 9;
        std::uint8_t c = 0;
        CHECK(reader.read_compressed_delta(a));
        CHECK(reader.read_compressed_delta(b));
        CHECK(reader.read_compressed_delta(c));
        CHECK(a == 0);
        CHECK(b == 9);
        CHECK(c == 3);
    }
}

TEST_CASE("BitStream: a delta read past the end fails the reader", "[raknet][bitstream][delta]")
{
    const Bytes data{0x80}; // flag 1, then only 7 bits of a uint16
    BitStreamReader reader(data);
    std::uint16_t value = 42;
    CHECK_FALSE(reader.read_delta(value));
    CHECK_FALSE(reader.ok());

    BitStreamReader empty(ByteView{});
    std::uint8_t untouched = 9;
    CHECK_FALSE(empty.read_compressed_delta(untouched));
    CHECK(untouched == 9);
}

// --- half-precision floats --------------------------------------------------------------------------------------

TEST_CASE("BitStream: a float16 is a uint16 fraction of its range, truncated and clamped",
          "[raknet][bitstream][float16]")
{
    const auto float16 = [](float value, float min, float max, ByteOrder order) {
        BitStreamWriter writer(order);
        writer.write_float16(value, min, max);
        CHECK(writer.bit_count() == 16);
        return bytes_of(writer);
    };
    const ByteOrder be = ByteOrder::big_endian;
    CHECK(float16(-1.0f, -1.0f, 1.0f, be) == Bytes{0x00, 0x00});
    CHECK(float16(0.0f, -1.0f, 1.0f, be) == Bytes{0x7F, 0xFF}); // 32767.5
    CHECK(float16(1.0f, -1.0f, 1.0f, be) == Bytes{0xFF, 0xFF});
    CHECK(float16(10.0f, 0.0f, 100.0f, be) == Bytes{0x19, 0x99}); // 6553.5
    CHECK(float16(10.0f, 0.0f, 100.0f, ByteOrder::little_endian) == Bytes{0x99, 0x19});
    CHECK(float16(250.0f, 0.0f, 100.0f, be) == Bytes{0xFF, 0xFF});
    CHECK(float16(-3.0f, 0.0f, 100.0f, be) == Bytes{0x00, 0x00});
}

TEST_CASE("BitStream: a float16 reads back within its range", "[raknet][bitstream][float16]")
{
    const Bytes data{0x00, 0x00, 0xFF, 0xFF, 0x19, 0x99};
    BitStreamReader reader(data);
    CHECK(reader.read_float16(-5.0f, 3.0f) == -5.0f);
    CHECK(reader.read_float16(-5.0f, 3.0f) == 3.0f);
    const float tenth = reader.read_float16(0.0f, 100.0f); // 6553 / 65535 * 100
    CHECK(tenth > 9.999f);
    CHECK(tenth < 10.0f);
    CHECK(reader.ok());
    CHECK(reader.read_float16(0.0f, 1.0f) == 0.0f);
    CHECK_FALSE(reader.ok());
}

// --- range-packed integers -------------------------------------------------------------------------------------

namespace
{

template <class T>
Bytes ranged(T value, T min, T max, bool allow_outside, ByteOrder order, std::size_t expected_bits)
{
    BitStreamWriter writer(order);
    writer.write_ranged(value, min, max, allow_outside);
    CHECK(writer.bit_count() == expected_bits);
    BitStreamReader reader(writer.data(), order);
    CHECK(reader.read_ranged(min, max, allow_outside) == value);
    CHECK(reader.ok());
    CHECK(reader.bit_position() == expected_bits);
    return bytes_of(writer);
}

} // namespace

TEST_CASE("BitStream: a ranged integer takes the bits of max - min, low byte first in either byte order",
          "[raknet][bitstream][ranged]")
{
    for (ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        // 1000 = 0x3E8 needs 10 bits; 300 = 0x12C: byte 2C, then the low 2 bits of 01
        CHECK(ranged<std::uint16_t>(300, 0, 1000, false, order, 10) == Bytes{0x2C, 0x40});
        CHECK(ranged<std::uint16_t>(0, 0, 1000, false, order, 10) == Bytes{0x00, 0x00});
        CHECK(ranged<std::uint16_t>(1000, 0, 1000, false, order, 10) == Bytes{0xE8, 0xC0});
        // max - min = 7: 3 bits; -1 - -2 = 1
        CHECK(ranged<std::int8_t>(-1, -2, 5, false, order, 3) == Bytes{0x20});
        CHECK(ranged<std::int8_t>(5, -2, 5, false, order, 3) == Bytes{0xE0});
        // the whole int32 range: 32 bits of value - INT32_MIN
        CHECK(ranged<std::int32_t>(0, INT32_MIN, INT32_MAX, false, order, 32) == Bytes{0x00, 0x00, 0x00, 0x80});
        CHECK(ranged<std::int32_t>(-1, INT32_MIN, INT32_MAX, false, order, 32) == Bytes{0xFF, 0xFF, 0xFF, 0x7F});
        CHECK(ranged<std::uint64_t>(UINT64_MAX, 0, UINT64_MAX, false, order, 64) == Bytes(8, 0xFF));
        // a 9-bit range: 511 = 0x1FF -> FF, then the low bit of 01
        CHECK(ranged<std::uint32_t>(1023, 512, 1023, false, order, 9) == Bytes{0xFF, 0x80});
    }
}

TEST_CASE("BitStream: a ranged integer with min == max takes no bits", "[raknet][bitstream][ranged]")
{
    CHECK(ranged<std::int16_t>(-7, -7, -7, false, ByteOrder::big_endian, 0).empty());
    // with allow_outside_range only the flag bit
    CHECK(ranged<std::uint8_t>(4, 4, 4, true, ByteOrder::big_endian, 1) == Bytes{0x00});
}

TEST_CASE("BitStream: a ranged integer allowed outside its range has a flag bit, then the value in full if outside",
          "[raknet][bitstream][ranged]")
{
    const ByteOrder be = ByteOrder::big_endian;
    const ByteOrder le = ByteOrder::little_endian;
    // inside: 0, then 4 bits for max - min = 10
    CHECK(ranged<std::uint8_t>(5, 0, 10, true, be, 5) == Bytes{0x28});
    // outside: 1, then the full value
    CHECK(ranged<std::uint8_t>(200, 0, 10, true, be, 9) == Bytes{0xE4, 0x00});
    CHECK(ranged<std::uint16_t>(2000, 0, 1000, true, be, 17) == Bytes{0x83, 0xE8, 0x00});
    CHECK(ranged<std::uint16_t>(2000, 0, 1000, true, le, 17) == Bytes{0xE8, 0x03, 0x80});
    CHECK(ranged<std::int32_t>(-1, 0, 100, true, be, 33) == Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0x80});
}

TEST_CASE("BitStream: a truncated ranged integer fails the reader", "[raknet][bitstream][ranged]")
{
    const Bytes data{0xFF};
    BitStreamReader reader(data);
    CHECK(reader.read_ranged<std::uint16_t>(0, 1000) == 0);
    CHECK_FALSE(reader.ok());
}
