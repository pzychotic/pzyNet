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
