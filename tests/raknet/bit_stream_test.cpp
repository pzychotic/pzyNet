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

TEST_CASE("BitStream: raw bits take whole bytes first, then the tail's low or high bits", "[raknet][bitstream]")
{
    // RakNet's WriteBits: with right alignment the last partial byte contributes its low bits,
    // without it its high bits.
    const Bytes right{0xAB, 0x05}; // tail 0x05 -> low 3 bits 101
    const Bytes left{0xAB, 0xA0};  // tail 0xA0 -> high 3 bits 101

    BitStreamWriter a;
    a.write(true);
    a.write_bits(right, 11);
    BitStreamWriter b;
    b.write(true);
    b.write_bits(left, 11, false);

    // 1 | 10101011 | 101 -> 1101 0101 1101 0000
    CHECK(a.bit_count() == 12);
    CHECK(bytes_of(a) == Bytes{0xD5, 0xD0});
    CHECK(bytes_of(b) == Bytes{0xD5, 0xD0});

    BitStreamReader reader(a.data());
    CHECK(reader.read<bool>());
    std::uint8_t out[2] = {0xEE, 0xEE};
    CHECK(reader.read_bits(out, 11));
    CHECK(Bytes(out, out + 2) == right);

    BitStreamReader reader2(a.data());
    CHECK(reader2.read<bool>());
    CHECK(reader2.read_bits(out, 11, false));
    CHECK(Bytes(out, out + 2) == left);
    CHECK(reader2.ok());
    CHECK(reader2.bits_remaining() == 4);
}

TEST_CASE("BitStream: zero raw bits write and read nothing", "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.write(true);
    writer.write_bits(ByteView(), 0);
    CHECK(writer.bit_count() == 1);

    BitStreamReader reader(writer.data());
    CHECK(reader.read_bits(MutableByteView(), 0)); // RakNet's ReadBits returns false here
    CHECK(reader.bit_position() == 0);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: raw bytes go at the current bit position, aligned bytes after zero padding",
          "[raknet][bitstream]")
{
    const Bytes payload{0x12, 0x34};

    BitStreamWriter raw;
    raw.write(true);
    raw.write_bytes(payload);
    // 1 | 00010010 | 00110100
    CHECK(raw.bit_count() == 17);
    CHECK(raw.byte_count() == 3);
    CHECK(bytes_of(raw) == Bytes{0x89, 0x1A, 0x00});

    BitStreamWriter aligned;
    aligned.write(true);
    aligned.write_aligned_bytes(payload);
    aligned.write(true);
    // 1 0000000 | 00010010 | 00110100 | 1
    CHECK(aligned.bit_count() == 25);
    CHECK(bytes_of(aligned) == Bytes{0x80, 0x12, 0x34, 0x80});

    std::uint8_t out[2] = {};
    BitStreamReader from_raw(raw.data());
    CHECK(from_raw.read<bool>());
    CHECK(from_raw.read_bytes(out));
    CHECK(Bytes(out, out + 2) == payload);

    BitStreamReader from_aligned(aligned.data());
    CHECK(from_aligned.read<bool>());
    CHECK(from_aligned.read_aligned_bytes(out));
    CHECK(Bytes(out, out + 2) == payload);
    CHECK(from_aligned.read<bool>());
    CHECK(from_aligned.ok());
}

TEST_CASE("BitStream: aligning pads to the next byte boundary and does nothing on one", "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.align();
    CHECK(writer.bit_count() == 0);
    writer.write(true);
    writer.align();
    CHECK(writer.bit_count() == 8);
    writer.align();
    CHECK(writer.bit_count() == 8);
    writer.write(std::uint8_t{0xFF});
    writer.write(false);
    writer.write(true);
    writer.align();
    CHECK(writer.bit_count() == 24);
    CHECK(bytes_of(writer) == Bytes{0x80, 0xFF, 0x40});

    // An empty aligned write still aligns, as RakNet's WriteAlignedBytes does.
    writer.write(true);
    writer.write_aligned_bytes(ByteView());
    CHECK(writer.bit_count() == 32);

    BitStreamReader reader(writer.data());
    reader.align();
    CHECK(reader.bit_position() == 0);
    CHECK(reader.read<bool>());
    reader.align();
    CHECK(reader.bit_position() == 8);
    CHECK(reader.read<std::uint8_t>() == 0xFF);
    CHECK(reader.read<bool>() == false);
    reader.align();
    CHECK(reader.bit_position() == 24);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: skipping moves the read position by bits or bytes", "[raknet][bitstream]")
{
    const Bytes data{0x12, 0x34, 0x56, 0x78};
    BitStreamReader reader(data);
    CHECK(reader.bit_count() == 32);
    reader.skip_bits(4);
    CHECK(reader.read<std::uint8_t>() == 0x23);
    reader.skip_bytes(1);
    CHECK(reader.bit_position() == 20);
    CHECK(reader.bits_remaining() == 12);
    CHECK(reader.read<std::uint8_t>() == 0x67);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: every kind of read past the end fails, stays failed and reads nothing",
          "[raknet][bitstream]")
{
    // The view covers only the first two bytes; the guard bytes behind it must never be read.
    const Bytes storage{0xFF, 0xFF, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    const ByteView data(storage.data(), 2);

    SECTION("float")
    {
        BitStreamReader reader(data);
        CHECK(reader.read<float>() == 0.0f);
        CHECK_FALSE(reader.ok());
    }
    SECTION("double after a bit")
    {
        BitStreamReader reader(data);
        CHECK(reader.read<bool>());
        CHECK(reader.read<double>() == 0.0);
        CHECK_FALSE(reader.ok());
    }
    SECTION("raw bits, leaving the output zeroed")
    {
        BitStreamReader reader(data);
        std::uint8_t out[3] = {0xEE, 0xEE, 0xEE};
        CHECK_FALSE(reader.read_bits(out, 17));
        CHECK(Bytes(out, out + 3) == Bytes{0, 0, 0});
        CHECK_FALSE(reader.ok());
    }
    SECTION("raw bits into a buffer that is too small")
    {
        BitStreamReader reader(data);
        std::uint8_t out[1] = {0xEE};
        CHECK_FALSE(reader.read_bits(out, 9));
        CHECK_FALSE(reader.ok());
    }
    SECTION("aligned bytes")
    {
        BitStreamReader reader(data);
        CHECK(reader.read<bool>());
        std::uint8_t out[2] = {};
        CHECK_FALSE(reader.read_aligned_bytes(out)); // aligned to bit 8, one byte left
        CHECK_FALSE(reader.ok());
    }
    SECTION("skipping")
    {
        BitStreamReader reader(data);
        reader.skip_bytes(3);
        CHECK_FALSE(reader.ok());
        CHECK(reader.bit_position() == 0);
    }

    // Whatever failed, the reader stays failed even for reads that would fit.
    BitStreamReader reader(data);
    CHECK(reader.read<std::uint32_t>() == 0);
    CHECK(reader.read<bool>() == false);
    std::uint8_t out[1] = {0xEE};
    CHECK_FALSE(reader.read_bytes(out));
    CHECK(out[0] == 0);
    CHECK_FALSE(reader.ok());
}

namespace game
{
struct Position
{
    std::int16_t x = 0;
    float y = 0;
};
struct Unit
{
    bool alive = false;
    Position where;
};

void write(BitStreamWriter& out, const Position& p)
{
    out.write(p.x);
    out.write(p.y);
}
void read(BitStreamReader& in, Position& p)
{
    p.x = in.read<std::int16_t>();
    p.y = in.read<float>();
}
void write(BitStreamWriter& out, const Unit& u)
{
    out.write(u.alive);
    out.write(u.where);
}
void read(BitStreamReader& in, Unit& u)
{
    in.read(u.alive);
    in.read(u.where);
}
} // namespace game

TEST_CASE("BitStream: user types are written and read through their read/write free functions",
          "[raknet][bitstream]")
{
    BitStreamWriter writer;
    writer.write(game::Unit{true, {-2, 1.0f}});
    // 1 | 11111111 11111110 | 00111111 10000000 00000000 00000000
    CHECK(writer.bit_count() == 49);
    CHECK(bytes_of(writer) == Bytes{0xFF, 0xFF, 0x1F, 0xC0, 0x00, 0x00, 0x00});

    BitStreamReader reader(writer.data());
    const game::Unit unit = reader.read<game::Unit>();
    CHECK(reader.ok());
    CHECK(unit.alive);
    CHECK(unit.where.x == -2);
    CHECK(unit.where.y == 1.0f);

    BitStreamReader short_reader(writer.data().first(4));
    game::Unit partial;
    CHECK_FALSE(short_reader.read(partial));
    CHECK_FALSE(short_reader.ok());
}
