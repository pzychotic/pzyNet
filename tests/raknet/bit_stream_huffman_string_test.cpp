// Byte-exact checks of Huffman-compressed strings (RakNet's StringCompressor with its default English
// frequency table), as upstream RakNet 4.081 writes them for WriteCompressed(RakString) and
// WriteCompressed(const char*): StringCompressor.cpp:366-427, DS_HuffmanEncodingTree.cpp. Upstream
// defines compatibility (ADR-0003); the fork's Huffman tree and frequency table are the same.

#include "pzynet/raknet/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
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

TEST_CASE("BitStream: a Huffman string is its compressed bit length, then its Huffman codes",
          "[raknet][bitstream]")
{
    // "ab": 16 code bits, 01100011 11010101 (no padding needed). The bit length 16 is a compressed uint32, which
    // a big-endian stream cannot shrink: 0 + 00 00 00 10 (33 bits).
    // 00000000 00000000 00000000 00001000 0|0110001 11101010 1
    BitStreamWriter writer;
    writer.write_huffman_string("ab");
    CHECK(writer.bit_count() == 33 + 16);
    CHECK(bytes_of(writer) == Bytes{0x00, 0x00, 0x00, 0x08, 0x31, 0xEA, 0x80});

    BitStreamReader reader(writer.data());
    CHECK(reader.read_huffman_string() == "ab");
    CHECK(reader.ok());
    CHECK(reader.bits_remaining() == 7);
}

TEST_CASE("BitStream: a longer Huffman string matches upstream's coder", "[raknet][bitstream]")
{
    // 19 characters in 136 code bits (17 bytes); the same vector pins the fork's StringCompressor.
    BitStreamWriter writer;
    writer.write_huffman_string("Hello, World! 0x7F~");
    CHECK(bytes_of(writer) == Bytes{0x00, 0x00, 0x00, 0x44, 0x3E, 0x04, 0xA5, 0x1A, 0xFA, 0xB9, 0x19,
                                    0x93, 0x95, 0xDF, 0x51, 0x54, 0xD5, 0x75, 0xAB, 0x2A, 0xDA, 0x80});

    BitStreamReader reader(writer.data());
    CHECK(reader.read_huffman_string() == "Hello, World! 0x7F~");
    CHECK(reader.ok());
}

TEST_CASE("BitStream: a Huffman string's bit length follows the stream's byte order", "[raknet][bitstream]")
{
    // Little-endian, 16 compresses: three 00 bytes as 1 bits, then 0 + 00010000; then the codes.
    // 1110 0001 0000|0110 00111101 0101
    BitStreamWriter writer(ByteOrder::little_endian);
    writer.write_huffman_string("ab");
    CHECK(writer.bit_count() == 12 + 16);
    CHECK(bytes_of(writer) == Bytes{0xE1, 0x06, 0x3D, 0x50});

    BitStreamReader reader(writer.data(), ByteOrder::little_endian);
    CHECK(reader.read_huffman_string() == "ab");
    CHECK(reader.bit_position() == 28);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: an empty Huffman string is just a compressed zero bit length", "[raknet][bitstream]")
{
    for (const ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        // 1 1 1 for the three upper 00 bytes, then 1 and the low nibble 0000
        BitStreamWriter writer(order);
        writer.write_huffman_string("");
        writer.write(std::uint8_t{0xAB});
        CHECK(bytes_of(writer) == Bytes{0xF0, 0xAB});

        BitStreamReader reader(writer.data(), order);
        CHECK(reader.read_huffman_string().empty());
        CHECK(reader.read<std::uint8_t>() == 0xAB);
        CHECK(reader.ok());
    }
}

TEST_CASE("BitStream: a Huffman string is not aligned", "[raknet][bitstream]")
{
    // 1 | 0 00000000 00000000 00000000 00010000 | 01100011 11010101
    BitStreamWriter writer;
    writer.write(true);
    writer.write_huffman_string("ab");
    CHECK(writer.bit_count() == 50);
    CHECK(bytes_of(writer) == Bytes{0x80, 0x00, 0x00, 0x04, 0x18, 0xF5, 0x40});

    BitStreamReader reader(writer.data());
    CHECK(reader.read<bool>());
    CHECK(reader.read_huffman_string() == "ab");
    CHECK(reader.bit_position() == 50);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: Huffman codes are padded to whole bytes, and the padding decodes to nothing",
          "[raknet][bitstream]")
{
    for (int c = 1; c < 256; ++c)
    {
        const std::string text(1, static_cast<char>(c));
        BitStreamWriter writer;
        writer.write_huffman_string(text);

        BitStreamReader reader(writer.data());
        const auto bit_length = reader.read_compressed<std::uint32_t>();
        INFO("byte " << c << ", " << bit_length << " code bits");
        CHECK(bit_length % 8 == 0);
        CHECK(writer.bit_count() == 33 + bit_length);

        BitStreamReader again(writer.data());
        CHECK(again.read_huffman_string() == text);
        CHECK(again.bit_position() == 33 + bit_length);
        CHECK(again.ok());
    }
}

TEST_CASE("BitStream: every byte value but zero survives a Huffman string", "[raknet][bitstream]")
{
    std::string text;
    for (int c = 1; c < 256; ++c)
        text.push_back(static_cast<char>(c));
    for (const ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        BitStreamWriter writer(order);
        writer.write_huffman_string(text);
        writer.write(true);
        BitStreamReader reader(writer.data(), order);
        CHECK(reader.read_huffman_string() == text);
        CHECK(reader.read<bool>());
        CHECK(reader.ok());
    }
}

TEST_CASE("BitStream: a Huffman string with a zero byte or of more than 65534 bytes is rejected",
          "[raknet][bitstream]")
{
    using namespace std::string_literals;
    const std::string longest(65534, 'e');
    const std::string too_long(65535, 'e');

    BitStreamWriter writer;
    std::error_code ec;
    writer.write_huffman_string(too_long, ec);
    CHECK(ec == Errc::invalid_argument);
    writer.write_huffman_string("a\0b"s, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK(writer.bit_count() == 0);
    CHECK_THROWS_AS(writer.write_huffman_string(too_long), std::system_error);
    CHECK_THROWS_AS(writer.write_huffman_string("\0"s), std::system_error);
    CHECK(writer.bit_count() == 0);

    writer.write_huffman_string(longest, ec);
    CHECK_FALSE(ec);
    BitStreamReader reader(writer.data());
    CHECK(reader.read_huffman_string() == longest);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: reading a Huffman string keeps at most 65534 bytes but consumes all its bits",
          "[raknet][bitstream]")
{
    // Two padded payloads of 40000 'e's under one bit length: RakNet decodes them all into its
    // 65535-byte buffer and keeps the first 65534.
    BitStreamWriter single;
    single.write_huffman_string(std::string(40000, 'e'));
    BitStreamReader header(single.data());
    const auto payload_bits = header.read_compressed<std::uint32_t>();
    std::vector<std::uint8_t> payload((payload_bits + 7) / 8);
    REQUIRE(header.read_bits(MutableByteView(payload.data(), payload.size()), payload_bits));

    BitStreamWriter doubled;
    doubled.write_compressed(static_cast<std::uint32_t>(2 * payload_bits));
    doubled.write_bits(ByteView(payload.data(), payload.size()), payload_bits);
    doubled.write_bits(ByteView(payload.data(), payload.size()), payload_bits);
    doubled.write(std::uint8_t{0xAB});

    BitStreamReader reader(doubled.data());
    CHECK(reader.read_huffman_string() == std::string(65534, 'e'));
    CHECK(reader.read<std::uint8_t>() == 0xAB);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: reading a malformed Huffman string fails without reading past the end",
          "[raknet][bitstream]")
{
    SECTION("the bit length is cut short")
    {
        const Bytes data{0x00, 0x00};
        BitStreamReader reader(data);
        CHECK(reader.read_huffman_string().empty());
        CHECK_FALSE(reader.ok());
    }
    SECTION("the bit length announces more bits than are left")
    {
        // 16 code bits announced, 7 left
        const Bytes data{0x00, 0x00, 0x00, 0x08, 0x31};
        BitStreamReader reader(data);
        CHECK(reader.read_huffman_string().empty());
        CHECK_FALSE(reader.ok());
    }
    SECTION("the largest bit length")
    {
        BitStreamWriter writer;
        writer.write_compressed((std::numeric_limits<std::uint32_t>::max)());
        const std::uint8_t tail[4] = {0x61, 0x62, 0x63, 0x64};
        writer.write_bytes(ByteView(tail, sizeof tail));
        BitStreamReader reader(writer.data());
        CHECK(reader.read_huffman_string().empty());
        CHECK_FALSE(reader.ok());
    }
}
