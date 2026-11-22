// Byte-exact checks of RakNet's BitStream helpers for game math (normalized vectors, vectors,
// normalized quaternions, orthogonal matrices). Expected bytes are worked out by hand from upstream
// RakNet 4.081's BitStream.h (WriteNormVector, WriteVector, WriteNormQuat, WriteOrthMatrix).

#include "pzynet/raknet/bit_stream_geometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
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

TEST_CASE("BitStream: a normalized vector is three half floats in [-1, 1]", "[raknet][bitstream]")
{
    // Each component c becomes (uint16)(65535 * (c + 1) / 2): 0 -> 7FFF, 1 -> FFFF, -1 -> 0000.
    // A leading bit shows the components are not aligned.
    BitStreamWriter big;
    big.write(true);
    write_normalized_vector(big, Vector3<float>{0.0f, 1.0f, -1.0f});
    CHECK(big.bit_count() == 49);
    CHECK(bytes_of(big) == Bytes{0xBF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00});

    BitStreamWriter little(ByteOrder::little_endian);
    little.write(true);
    write_normalized_vector(little, Vector3<double>{0.0, 1.0, -1.0});
    CHECK(bytes_of(little) == Bytes{0xFF, 0xBF, 0xFF, 0xFF, 0x80, 0x00, 0x00});

    BitStreamReader in(big.data());
    CHECK(in.read<bool>());
    const Vector3<float> v = read_normalized_vector<float>(in);
    CHECK(in.ok());
    CHECK(v.x == 32767.0f / 65535.0f * 2.0f - 1.0f);
    CHECK(v.y == 1.0f);
    CHECK(v.z == -1.0f);
}

TEST_CASE("BitStream: normalized vector components are truncated and clamped", "[raknet][bitstream]")
{
    // 0.5 -> 65535 * 1.5 / 2 = 49151.25 -> BFFF. Out-of-range components clamp to the range ends.
    BitStreamWriter out;
    write_normalized_vector(out, Vector3<float>{0.5f, 1.5f, -3.0f});
    CHECK(bytes_of(out) == Bytes{0xBF, 0xFF, 0xFF, 0xFF, 0x00, 0x00});
}

TEST_CASE("BitStream: a vector is its length as a float and its direction as compressed floats",
          "[raknet][bitstream]")
{
    // (3, 0, -4): length 5.0f = 40A00000, then each component / length as (uint16)((c + 1) * 32767.5):
    // 0.6 -> CCCC, 0 -> 7FFF, -0.8 -> 1999.
    BitStreamWriter big;
    write_vector(big, Vector3<float>{3.0f, 0.0f, -4.0f});
    CHECK(bytes_of(big) == Bytes{0x40, 0xA0, 0x00, 0x00, 0xCC, 0xCC, 0x7F, 0xFF, 0x19, 0x99});

    BitStreamWriter little(ByteOrder::little_endian);
    write_vector(little, Vector3<double>{3.0, 0.0, -4.0});
    CHECK(bytes_of(little) == Bytes{0x00, 0x00, 0xA0, 0x40, 0xCC, 0xCC, 0xFF, 0x7F, 0x99, 0x19});

    BitStreamReader in(big.data());
    const Vector3<float> v = read_vector<float>(in);
    CHECK(in.ok());
    CHECK(v.x == (52428.0f / 32767.5f - 1.0f) * 5.0f);
    CHECK(v.y == (32767.0f / 32767.5f - 1.0f) * 5.0f);
    CHECK(v.z == (6553.0f / 32767.5f - 1.0f) * 5.0f);
    CHECK(in.bits_remaining() == 0);
}

TEST_CASE("BitStream: a vector of length 1e-5 or less is only its length", "[raknet][bitstream]")
{
    BitStreamWriter out;
    write_vector(out, Vector3<float>{0.0f, 0.0f, 0.0f});
    write_vector(out, Vector3<double>{0.0, 0.0, -1e-6});
    // 1e-6 as a float: 358637BD.
    CHECK(bytes_of(out) == Bytes{0x00, 0x00, 0x00, 0x00, 0x35, 0x86, 0x37, 0xBD});

    BitStreamReader in(out.data());
    CHECK(read_vector<float>(in) == Vector3<float>{});
    const Vector3<double> tiny = read_vector<double>(in);
    CHECK(in.ok());
    CHECK(tiny == Vector3<double>{});
    CHECK(in.bits_remaining() == 0);
}

TEST_CASE("BitStream: a short vector read fails the reader", "[raknet][bitstream]")
{
    const Bytes data{0x40, 0xA0, 0x00, 0x00, 0xCC, 0xCC, 0x7F, 0xFF, 0x19};
    BitStreamReader in(data);
    CHECK(read_vector<float>(in) == Vector3<float>{});
    CHECK_FALSE(in.ok());
}

TEST_CASE("BitStream: a normalized quaternion is four sign bits and three magnitudes", "[raknet][bitstream]")
{
    // Signs of w, x, y, z (1 = negative): 1010, then |x|, |y|, |z| as (uint16)(c * 65535): 0.5 -> 7FFF.
    // w is left out and recomputed by the reader. 52 bits.
    BitStreamWriter big;
    write_normalized_quaternion(big, Quaternion<float>{-0.5f, 0.5f, -0.5f, 0.5f});
    CHECK(big.bit_count() == 52);
    CHECK(bytes_of(big) == Bytes{0xA7, 0xFF, 0xF7, 0xFF, 0xF7, 0xFF, 0xF0});

    BitStreamWriter little(ByteOrder::little_endian);
    write_normalized_quaternion(little, Quaternion<double>{-0.5, 0.5, -0.5, 0.5});
    CHECK(bytes_of(little) == Bytes{0xAF, 0xF7, 0xFF, 0xF7, 0xFF, 0xF7, 0xF0});

    BitStreamReader in(big.data());
    const Quaternion<float> q = read_normalized_quaternion<float>(in);
    CHECK(in.ok());
    const float half = 32767.0f / 65535.0f;
    CHECK(q.x == half);
    CHECK(q.y == -half);
    CHECK(q.z == half);
    CHECK(q.w < 0.0f);
    CHECK(std::fabs(q.w + 0.5f) < 1e-4f);
}

TEST_CASE("BitStream: a quaternion's w is recomputed in float precision, clamped at zero", "[raknet][bitstream]")
{
    // RakNet computes w = sqrt(1 - x^2 - y^2 - z^2) with a float difference and a float sqrt, also when
    // reading doubles.
    BitStreamWriter out;
    write_normalized_quaternion(out, Quaternion<double>{0.3, 0.1, 0.3, 0.9});
    write_normalized_quaternion(out, Quaternion<double>{-0.0, 1.0, 1.0, 0.0}); // |v| > 1: w = 0
    BitStreamReader in(out.data());
    const Quaternion<double> q = read_normalized_quaternion<double>(in);
    CHECK(static_cast<double>(static_cast<float>(q.w)) == q.w);
    CHECK(std::fabs(q.w - 0.3) < 2e-4);
    const Quaternion<double> over = read_normalized_quaternion<double>(in);
    CHECK(over.w == 0.0);
    CHECK(over.x == 1.0);
    CHECK(over.y == 1.0);
    CHECK(in.ok());
}

TEST_CASE("BitStream: a short quaternion read fails the reader", "[raknet][bitstream]")
{
    const Bytes data{0xA7, 0xFF, 0xF7, 0xFF, 0xF7, 0xFF};
    BitStreamReader in(data);
    CHECK(read_normalized_quaternion<float>(in) == Quaternion<float>{});
    CHECK_FALSE(in.ok());
}

namespace
{

Matrix3<float> diagonal(float m00, float m11, float m22)
{
    return Matrix3<float>{{{m00, 0.0f, 0.0f}, {0.0f, m11, 0.0f}, {0.0f, 0.0f, m22}}};
}

Bytes written_matrix(const Matrix3<float>& m, ByteOrder order = ByteOrder::big_endian)
{
    BitStreamWriter out(order);
    write_orthogonal_matrix(out, m);
    return bytes_of(out);
}

} // namespace

TEST_CASE("BitStream: an orthogonal matrix is written as its rotation quaternion", "[raknet][bitstream]")
{
    // The quaternion's components come from the trace sums: w = sqrt(1 + m00 + m11 + m22) / 2,
    // x = sqrt(1 + m00 - m11 - m22) / 2, ...; a negative sum counts as 0.
    // Identity: w's sum is 4, x's, y's and z's are 0.
    CHECK(written_matrix(diagonal(1.0f, 1.0f, 1.0f)) == Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    // 180 degrees about z: z's sum is 4 -> FFFF.
    CHECK(written_matrix(diagonal(-1.0f, -1.0f, 1.0f)) == Bytes{0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0xF0});
    // w's sum is -2, taken as 0; x's, y's and z's are 2 -> sqrt(2) / 2 * 65535 = 46340.95 -> B504.
    CHECK(written_matrix(diagonal(-1.0f, -1.0f, -1.0f)) == Bytes{0x0B, 0x50, 0x4B, 0x50, 0x4B, 0x50, 0x40});
    CHECK(written_matrix(diagonal(-1.0f, -1.0f, -1.0f), ByteOrder::little_endian) ==
          Bytes{0x00, 0x4B, 0x50, 0x4B, 0x50, 0x4B, 0x50});
}

TEST_CASE("BitStream: an orthogonal matrix's quaternion signs come from its off-diagonal differences",
          "[raknet][bitstream]")
{
    // 90 degrees about z: w and z are sqrt(2) / 2 (B504); z's sign follows m10 - m01.
    const Matrix3<float> plus_90{{{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    const Matrix3<float> minus_90{{{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    CHECK(written_matrix(plus_90) == Bytes{0x00, 0x00, 0x00, 0x00, 0x0B, 0x50, 0x40});
    CHECK(written_matrix(minus_90) == Bytes{0x10, 0x00, 0x00, 0x00, 0x0B, 0x50, 0x40});

    BitStreamWriter out;
    write_orthogonal_matrix(out, plus_90);
    BitStreamReader in(out.data());
    const Matrix3<double> read = read_orthogonal_matrix<double>(in);
    CHECK(in.ok());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            CHECK(std::fabs(read.m[r][c] - plus_90.m[r][c]) < 1e-4);
}

TEST_CASE("BitStream: an orthogonal matrix's trace sums are floats with a float square root",
          "[raknet][bitstream]")
{
    // Upstream RakNet sums in float and takes the float overload of sqrt. For diag(a, -1, -1) with
    // a = 0.0100542903f (3C24BAC0), x's sum is 3.01005435f; its float root / 2 * 65535 truncates to
    // DE11 (a double root would give DE12). y's and z's sums are 1 - a in two float roundings, both 7F5A.
    const std::uint32_t bits = 0x3C24BAC0u;
    float a;
    std::memcpy(&a, &bits, sizeof a);
    CHECK(written_matrix(diagonal(a, -1.0f, -1.0f)) == Bytes{0x0D, 0xE1, 0x17, 0xF5, 0xA7, 0xF5, 0xA0});
    CHECK(written_matrix(diagonal(a, -1.0f, -1.0f), ByteOrder::little_endian) ==
          Bytes{0x01, 0x1D, 0xE5, 0xA7, 0xF5, 0xA7, 0xF0});
}

TEST_CASE("BitStream: a short matrix read fails the reader", "[raknet][bitstream]")
{
    const Bytes data{0x00, 0x00, 0x00};
    BitStreamReader in(data);
    const Matrix3<float> m = read_orthogonal_matrix<float>(in);
    CHECK_FALSE(in.ok());
    CHECK(m == Matrix3<float>{});
}
