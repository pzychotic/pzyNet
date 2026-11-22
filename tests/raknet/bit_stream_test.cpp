// Byte-exact checks of the RakNet BitStream encodings. Expected bytes are worked out by hand from
// upstream RakNet 4.081's BitStream: bits are packed most significant first, a bool takes one bit, and
// multi-byte integers are written big-endian unless the stream uses the native (little-endian) order.

#include "pzynet/raknet/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

TEST_CASE("BitStream: booleans take one bit and integers follow at unaligned positions", "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.write(true);
    writer.write(std::uint16_t{0x1234});
    writer.write(false);
    writer.write(std::uint8_t{0xAB});
    writer.write(std::uint32_t{0x01020304});

    // 1 | 0001001000110100 | 0 | 10101011 | 00000001000000100000001100000100, then zero padding
    CHECK(writer.bit_count() == 58);
    CHECK(bytes_of(writer) == Bytes{0x89, 0x1A, 0x2A, 0xC0, 0x40, 0x80, 0xC1, 0x00});
}

TEST_CASE("BitStream: signed integers are written as their two's complement", "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.write(std::int16_t{-2});
    writer.write(std::int8_t{-128});
    writer.write(std::int64_t{-1});

    CHECK(bytes_of(writer) == Bytes{0xFF, 0xFE, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("BitStream: a little-endian stream writes integers least significant byte first",
          "[raknet][bitstream]")
{
    BitStreamWriter writer(ByteOrder::little_endian);
    writer.write(std::uint16_t{0x1234});
    writer.write(true);
    writer.write(std::uint32_t{0x01020304});

    // 00110100 00010010 | 1 | 00000100 00000011 00000010 00000001
    CHECK(bytes_of(writer) == Bytes{0x34, 0x12, 0x82, 0x01, 0x81, 0x00, 0x80});
}

TEST_CASE("BitStream: a reader takes booleans and integers back from unaligned positions",
          "[raknet][bitstream]")
{
    const Bytes data{0x89, 0x1A, 0x2A, 0xC0, 0x40, 0x80, 0xC1, 0x00};
    BitStreamReader reader(data);

    CHECK(reader.read<bool>() == true);
    CHECK(reader.read<std::uint16_t>() == 0x1234);
    CHECK(reader.read<bool>() == false);
    CHECK(reader.read<std::uint8_t>() == 0xAB);
    CHECK(reader.read<std::uint32_t>() == 0x01020304);
    CHECK(reader.ok());
    CHECK(reader.bits_remaining() == 6);
}

TEST_CASE("BitStream: a reader honours its byte order and sign", "[raknet][bitstream]")
{
    const Bytes data{0x34, 0x12, 0xFF, 0xFE};
    BitStreamReader reader(data, ByteOrder::little_endian);

    CHECK(reader.read<std::uint16_t>() == 0x1234);
    CHECK(reader.read<std::int16_t>() == -257);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: reading past the end fails once and stays failed", "[raknet][bitstream]")
{
    const Bytes data{0xFF, 0xFF, 0xFF};
    BitStreamReader reader(data);

    CHECK(reader.read<std::uint16_t>() == 0xFFFF);
    CHECK(reader.read<std::uint16_t>() == 0); // only 8 bits left
    CHECK_FALSE(reader.ok());
    CHECK(reader.read<bool>() == false); // the failure latches, although a bit would be left
    CHECK_FALSE(reader.ok());
}

namespace
{
enum class Shade : std::uint16_t
{
    dark = 0x0102,
};
enum Plain // unscoped, int-sized as in most RakNet code
{
    plain_seven = 7,
};
enum class Tiny : std::int8_t
{
    minus_two = -2,
};
} // namespace

TEST_CASE("BitStream: enums are written in the width of their underlying type", "[raknet][bitstream]")
{
    BitStreamWriter big;
    big.write(Shade::dark);
    big.write(plain_seven);
    big.write(Tiny::minus_two);
    CHECK(bytes_of(big) == Bytes{0x01, 0x02, 0x00, 0x00, 0x00, 0x07, 0xFE});

    BitStreamWriter little(ByteOrder::little_endian);
    little.write(Shade::dark);
    little.write(plain_seven);
    little.write(Tiny::minus_two);
    CHECK(bytes_of(little) == Bytes{0x02, 0x01, 0x07, 0x00, 0x00, 0x00, 0xFE});

    BitStreamReader reader(little.data(), ByteOrder::little_endian);
    CHECK(reader.read<Shade>() == Shade::dark);
    CHECK(reader.read<Plain>() == plain_seven);
    CHECK(reader.read<Tiny>() == Tiny::minus_two);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: float and double are their IEEE 754 bits in the stream's byte order", "[raknet][bitstream]")
{
    // 1.0f = 0x3F800000, -2.5 = 0xC004000000000000
    BitStreamWriter big;
    big.write(true);
    big.write(1.0f);
    big.write(-2.5);
    // 1 | 00111111 10000000 00000000 00000000 | 11000000 00000100 00000000 ... (6 more zero bytes)
    CHECK(big.bit_count() == 97);
    CHECK(bytes_of(big) == Bytes{0x9F, 0xC0, 0x00, 0x00, 0x60, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

    BitStreamWriter little(ByteOrder::little_endian);
    little.write(1.0f);
    little.write(-2.5);
    CHECK(bytes_of(little) == Bytes{0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0});

    BitStreamReader from_big(big.data());
    CHECK(from_big.read<bool>());
    CHECK(from_big.read<float>() == 1.0f);
    CHECK(from_big.read<double>() == -2.5);
    CHECK(from_big.ok());
    BitStreamReader from_little(little.data(), ByteOrder::little_endian);
    CHECK(from_little.read<float>() == 1.0f);
    CHECK(from_little.read<double>() == -2.5);
    CHECK(from_little.ok());
}
