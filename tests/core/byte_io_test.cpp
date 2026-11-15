#include "pzynet/core/bit_io.hpp"
#include "pzynet/core/byte_io.hpp"
#include "pzynet/core/sequence.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace pzynet;
using Bytes = std::vector<std::uint8_t>;

TEST_CASE("ByteWriter lays out integers in the documented byte order", "[core][byte_io]")
{
    Bytes out;
    ByteWriter w(out);
    w.u8(0xAB);
    w.u16_be(0x1234);
    w.u32_be(0xDEADBEEF);
    w.u64_be(0x0102030405060708ull);
    w.u24_le(0x00ABCDEF);
    w.zeros(2);
    CHECK(out == Bytes{0xAB, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 0xEF, 0xCD, 0xAB, 0, 0});

    w.pad_to(24);
    CHECK(out.size() == 24);
    w.pad_to(10); // never shrinks
    CHECK(out.size() == 24);
}

TEST_CASE("ByteReader mirrors ByteWriter", "[core][byte_io]")
{
    Bytes data;
    ByteWriter w(data);
    w.u8(7);
    w.u16_be(0xBEEF);
    w.u24_le(0x123456);
    w.u32_be(0xCAFEBABE);
    w.u64_be(0xFFEEDDCCBBAA9988ull);
    w.f32_be(1.5f);
    w.bytes(Bytes{9, 8, 7});

    ByteReader r(data);
    CHECK(r.u8() == 7);
    CHECK(r.u16_be() == 0xBEEF);
    CHECK(r.u24_le() == 0x123456);
    CHECK(r.u32_be() == 0xCAFEBABE);
    CHECK(r.u64_be() == 0xFFEEDDCCBBAA9988ull);
    CHECK(r.f32_be() == 1.5f);
    const ByteView tail = r.bytes(3);
    CHECK(Bytes(tail.begin(), tail.end()) == Bytes{9, 8, 7});
    CHECK(r.ok());
    CHECK(r.remaining() == 0);
}

TEST_CASE("ByteReader latches failure on a short read", "[core][byte_io]")
{
    const Bytes data{1, 2, 3};
    ByteReader r(data);
    CHECK(r.u16_be() == 0x0102);
    CHECK(r.u32_be() == 0);
    CHECK_FALSE(r.ok());
    CHECK(r.u8() == 0); // stays failed even though one byte is left
    CHECK_FALSE(r.ok());
}

TEST_CASE("bits are packed most significant first", "[core][bit_io]")
{
    Bytes out;
    BitWriter w(out);
    w.bit(true);
    w.bit(false);
    w.bit(false);
    w.bits(0b101, 3);
    CHECK(out == Bytes{0b10010100});
    w.align();
    w.bit(true);
    CHECK(out == Bytes{0b10010100, 0b10000000});

    BitReader r(out);
    CHECK(r.bit());
    CHECK_FALSE(r.bit());
    CHECK(r.bits(4) == 0b0101);
    r.align();
    CHECK(r.bytes_consumed() == 1);
    CHECK(r.bit());
    CHECK(r.bits(7) == 0);
    CHECK(r.ok());
    r.bit();
    CHECK_FALSE(r.ok());
}

TEST_CASE("24-bit sequence arithmetic wraps", "[core][sequence]")
{
    CHECK(seq24::distance(5, 9) == 4);
    CHECK(seq24::distance(0xFFFFFE, 1) == 3);
    CHECK(seq24::wrap(0x1000002) == 2);
    CHECK(seq24::less(0xFFFFFF, 0));
    CHECK_FALSE(seq24::less(0, 0xFFFFFF));
    CHECK(seq24::less(10, 11));
    CHECK_FALSE(seq24::less(11, 11));
}
