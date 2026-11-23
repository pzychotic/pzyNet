// Byte-exact checks of the BitStream encodings of RakNet's own types and strings: 24-bit integers,
// GUIDs, IPv4 system addresses, length-prefixed strings and wide strings. Expected bytes are worked
// out by hand from upstream RakNet 4.081 (BitStream.h, RakString.cpp, RakWString.cpp), which defines
// compatibility (ADR-0003).

#include "pzynet/raknet/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

// --- 24-bit integers (upstream BitStream.h, Write/Read(const uint24_t&)) ---------------------------

TEST_CASE("BitStream: a 24-bit integer is byte-aligned and little-endian in either byte order",
          "[raknet][bitstream]")
{
    for (const ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
    {
        BitStreamWriter writer(order);
        writer.write(true);
        writer.write_uint24(0x123456);
        writer.write_uint24(0xABCDEF);

        // 1, padding to the byte boundary, then 56 34 12 and EF CD AB
        CHECK(writer.bit_count() == 56);
        CHECK(bytes_of(writer) == Bytes{0x80, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB});

        BitStreamReader reader(writer.data(), order);
        CHECK(reader.read<bool>());
        CHECK(reader.read_uint24() == 0x123456);
        CHECK(reader.read_uint24() == 0xABCDEF);
        CHECK(reader.ok());
        CHECK(reader.bits_remaining() == 0);
    }
}

TEST_CASE("BitStream: only the low 24 bits of a 24-bit integer are written", "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.write_uint24(0xFF010203);
    CHECK(bytes_of(writer) == Bytes{0x03, 0x02, 0x01});
}

TEST_CASE("BitStream: reading a 24-bit integer past the end fails", "[raknet][bitstream]")
{
    const Bytes data{0x80, 0x01, 0x02};
    BitStreamReader reader(data);
    CHECK(reader.read<bool>());
    CHECK(reader.read_uint24() == 0); // aligned to byte 1, two bytes left
    CHECK_FALSE(reader.ok());
}

// --- GUIDs (upstream BitStream.h, Write/Read(const RakNetGUID&): the uint64_t g) ---------------------

TEST_CASE("BitStream: a GUID is its 64-bit value in the stream's byte order, unaligned",
          "[raknet][bitstream]")
{
    const Guid guid{0x0102030405060708u};

    BitStreamWriter big;
    big.write(true);
    big.write(guid);
    // 1 | 00000001 00000010 ... 00001000
    CHECK(big.bit_count() == 65);
    CHECK(bytes_of(big) == Bytes{0x80, 0x81, 0x01, 0x82, 0x02, 0x83, 0x03, 0x84, 0x00});

    BitStreamWriter little(ByteOrder::little_endian);
    little.write(guid);
    CHECK(bytes_of(little) == Bytes{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});

    BitStreamReader reader(big.data());
    CHECK(reader.read<bool>());
    CHECK(reader.read<Guid>() == guid);
    CHECK(reader.ok());

    BitStreamReader short_reader(little.data().first(7));
    Guid partial;
    CHECK_FALSE(short_reader.read(partial));
}

// --- System addresses (upstream BitStream.h, Write/Read(const SystemAddress&)) ----------------------

TEST_CASE("BitStream: an IPv4 address is its version, the inverted address and the port, never swapped",
          "[raknet][bitstream]")
{
    const Endpoint endpoint(IpAddress::v4(192, 168, 1, 2), 0x1234);

    SECTION("at a byte boundary, in either byte order")
    {
        for (const ByteOrder order : {ByteOrder::big_endian, ByteOrder::little_endian})
        {
            BitStreamWriter writer(order);
            writer.write(endpoint);
            // 4 | ~C0 ~A8 ~01 ~02 | port in network order
            CHECK(bytes_of(writer) == Bytes{0x04, 0x3F, 0x57, 0xFE, 0xFD, 0x12, 0x34});

            BitStreamReader reader(writer.data(), order);
            CHECK(reader.read<Endpoint>() == endpoint);
            CHECK(reader.ok());
        }
    }
    SECTION("unaligned")
    {
        BitStreamWriter writer;
        writer.write(true);
        writer.write(endpoint);
        // 1 | 00000100 | 00111111 01010111 11111110 11111101 | 00010010 00110100
        CHECK(writer.bit_count() == 57);
        CHECK(bytes_of(writer) == Bytes{0x82, 0x1F, 0xAB, 0xFF, 0x7E, 0x89, 0x1A, 0x00});

        BitStreamReader reader(writer.data());
        CHECK(reader.read<bool>());
        CHECK(reader.read<Endpoint>() == endpoint);
        CHECK(reader.bit_position() == 57);
        CHECK(reader.ok());
    }
}

TEST_CASE("BitStream: an IPv6 address is rejected with an error and nothing is written", "[raknet][bitstream]")
{
    const Endpoint v6(IpAddress::loopback_v6(), 60000);
    const Endpoint mapped(IpAddress::v4(10, 0, 0, 1).to_v4_mapped(), 60000);

    for (const Endpoint& endpoint : {v6, mapped})
    {
        BitStreamWriter writer;
        writer.write(true);

        std::error_code ec;
        writer.write(endpoint, ec);
        CHECK(ec == Errc::address_family_mismatch);
        CHECK(writer.bit_count() == 1);

        CHECK_THROWS_AS(writer.write(endpoint), std::system_error);
        CHECK(writer.bit_count() == 1);

        writer.write(Endpoint(IpAddress::v4(1, 2, 3, 4), 5), ec);
        CHECK_FALSE(ec);
        CHECK(writer.bit_count() == 57);
    }
}

TEST_CASE("BitStream: reading an address fails on a version other than 4 or a short stream",
          "[raknet][bitstream]")
{
    SECTION("IPv6, which upstream RakNet reads only when built with IPv6 support")
    {
        const Bytes data{0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        BitStreamReader reader(data);
        CHECK(reader.read<Endpoint>() == Endpoint());
        CHECK_FALSE(reader.ok());
    }
    SECTION("truncated port")
    {
        const Bytes data{0x04, 0x3F, 0x57, 0xFE, 0xFD, 0x12};
        BitStreamReader reader(data);
        Endpoint endpoint;
        CHECK_FALSE(reader.read(endpoint));
    }
}

// --- Length-prefixed strings (upstream RakString::Serialize / Deserialize) --------------------------

TEST_CASE("BitStream: a string is a 16-bit length, then its bytes from the next byte boundary",
          "[raknet][bitstream]")
{
    SECTION("at a byte boundary")
    {
        BitStreamWriter writer;
        writer.write_string("hi");
        CHECK(bytes_of(writer) == Bytes{0x00, 0x02, 0x68, 0x69});
    }
    SECTION("the length follows the stream's byte order")
    {
        BitStreamWriter writer(ByteOrder::little_endian);
        writer.write_string("hi");
        CHECK(bytes_of(writer) == Bytes{0x02, 0x00, 0x68, 0x69});

        BitStreamReader reader(writer.data(), ByteOrder::little_endian);
        CHECK(reader.read_string() == "hi");
        CHECK(reader.ok());
    }
    SECTION("unaligned: the length is unaligned, the bytes are aligned")
    {
        BitStreamWriter writer;
        writer.write(true);
        writer.write_string("hi");
        // 1 | 00000000 00000010 | padding | 68 69
        CHECK(writer.bit_count() == 40);
        CHECK(bytes_of(writer) == Bytes{0x80, 0x01, 0x00, 0x68, 0x69});

        BitStreamReader reader(writer.data());
        CHECK(reader.read<bool>());
        CHECK(reader.read_string() == "hi");
        CHECK(reader.bits_remaining() == 0);
        CHECK(reader.ok());
    }
    SECTION("an empty string still aligns, on writing and on reading")
    {
        BitStreamWriter writer;
        writer.write(true);
        writer.write_string("");
        writer.write(std::uint8_t{0xAB});
        CHECK(bytes_of(writer) == Bytes{0x80, 0x00, 0x00, 0xAB});

        BitStreamReader reader(writer.data());
        CHECK(reader.read<bool>());
        CHECK(reader.read_string().empty());
        CHECK(reader.bit_position() == 24);
        CHECK(reader.read<std::uint8_t>() == 0xAB);
        CHECK(reader.ok());
    }
}

TEST_CASE("BitStream: a string's bytes are written as they are, embedded zeros included",
          "[raknet][bitstream]")
{
    // Upstream measures a C string with strlen; a string_view's bytes have no terminator to stop at.
    using namespace std::string_literals;
    const std::string text = "a\0b\xFF"s;
    BitStreamWriter writer;
    writer.write_string(text);
    CHECK(bytes_of(writer) == Bytes{0x00, 0x04, 0x61, 0x00, 0x62, 0xFF});

    BitStreamReader reader(writer.data());
    CHECK(reader.read_string() == text);
}

TEST_CASE("BitStream: a string longer than 65535 bytes is rejected and nothing is written",
          "[raknet][bitstream]")
{
    const std::string longest(65535, 'x');
    const std::string too_long(65536, 'x');

    BitStreamWriter writer;
    std::error_code ec;
    writer.write_string(too_long, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK(writer.bit_count() == 0);
    CHECK_THROWS_AS(writer.write_string(too_long), std::system_error);
    CHECK(writer.bit_count() == 0);

    writer.write_string(longest, ec);
    CHECK_FALSE(ec);
    CHECK(writer.byte_count() == 2 + 65535);
    BitStreamReader reader(writer.data());
    CHECK(reader.read_string() == longest);
}

TEST_CASE("BitStream: reading a string fails when its bytes are missing", "[raknet][bitstream]")
{
    const Bytes data{0x00, 0x03, 0x68, 0x69};
    BitStreamReader reader(data);
    CHECK(reader.read_string().empty());
    CHECK_FALSE(reader.ok());
}

// --- Wide strings (upstream RakWString::Serialize / Deserialize) ------------------------------------
// Upstream 4.081's WriteCompressed(RakWString) and WriteCompressed(const wchar_t*) are its only
// "compressed" strings that do not go through the Huffman coder: they call RakWString::Serialize,
// the same as Write(RakWString). The fork removed RakWString, so these vectors are the reference.

TEST_CASE("BitStream: a wide string is a 16-bit count, then 16-bit code units, all unaligned",
          "[raknet][bitstream]")
{
    const std::u16string text = u"hi☃";

    SECTION("big-endian")
    {
        BitStreamWriter writer;
        writer.write_wide_string(text);
        CHECK(bytes_of(writer) == Bytes{0x00, 0x03, 0x00, 0x68, 0x00, 0x69, 0x26, 0x03});
    }
    SECTION("little-endian")
    {
        BitStreamWriter writer(ByteOrder::little_endian);
        writer.write_wide_string(text);
        CHECK(bytes_of(writer) == Bytes{0x03, 0x00, 0x68, 0x00, 0x69, 0x00, 0x03, 0x26});

        BitStreamReader reader(writer.data(), ByteOrder::little_endian);
        CHECK(reader.read_wide_string() == text);
        CHECK(reader.ok());
    }
    SECTION("unaligned, with nothing aligned")
    {
        BitStreamWriter writer;
        writer.write(true);
        writer.write_wide_string(u"A");
        // 1 | 00000000 00000001 | 00000000 01000001
        CHECK(writer.bit_count() == 33);
        CHECK(bytes_of(writer) == Bytes{0x80, 0x00, 0x80, 0x20, 0x80});

        BitStreamReader reader(writer.data());
        CHECK(reader.read<bool>());
        CHECK(reader.read_wide_string() == u"A");
        CHECK(reader.bit_position() == 33);
        CHECK(reader.ok());
    }
    SECTION("an empty string is just its count")
    {
        BitStreamWriter writer;
        writer.write(true);
        writer.write_wide_string(u"");
        writer.write(std::uint8_t{0xAB});
        // 1 | 00000000 00000000 | 10101011
        CHECK(bytes_of(writer) == Bytes{0x80, 0x00, 0x55, 0x80});

        BitStreamReader reader(writer.data());
        CHECK(reader.read<bool>());
        CHECK(reader.read_wide_string().empty());
        CHECK(reader.read<std::uint8_t>() == 0xAB);
        CHECK(reader.ok());
    }
}

TEST_CASE("BitStream: a wide string of more than 65535 code units is rejected", "[raknet][bitstream]")
{
    const std::u16string too_long(65536, u'x');
    BitStreamWriter writer;
    std::error_code ec;
    writer.write_wide_string(too_long, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK(writer.bit_count() == 0);
    CHECK_THROWS_AS(writer.write_wide_string(too_long), std::system_error);
    CHECK(writer.bit_count() == 0);
}

TEST_CASE("BitStream: reading a wide string fails when code units are missing", "[raknet][bitstream]")
{
    const Bytes data{0x00, 0x02, 0x00, 0x68, 0x00};
    BitStreamReader reader(data);
    CHECK(reader.read_wide_string().empty());
    CHECK_FALSE(reader.ok());
}
