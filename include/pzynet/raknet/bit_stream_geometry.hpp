#pragma once

// RakNet's BitStream helpers for game math: normalized vectors, vectors, normalized quaternions and
// orthogonal (rotation) matrices, written and read bit for bit as RakNet 4.081 does
// (WriteNormVector / WriteVector / WriteNormQuat / WriteOrthMatrix and their Read forms).
//
// Every helper takes float or double components. As in RakNet, the component type matters for a few
// intermediate results (a vector's length, a matrix's trace sums), so a float and a double message
// can differ in the last quantization step.

#include "pzynet/raknet/bit_stream.hpp"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace pzynet
{
namespace raknet
{

template <class T>
struct Vector3
{
    T x{}, y{}, z{};

    friend bool operator==(const Vector3& a, const Vector3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
    friend bool operator!=(const Vector3& a, const Vector3& b) { return !(a == b); }
};

template <class T>
struct Quaternion
{
    T w{}, x{}, y{}, z{};

    friend bool operator==(const Quaternion& a, const Quaternion& b)
    {
        return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend bool operator!=(const Quaternion& a, const Quaternion& b) { return !(a == b); }
};

/// A 3x3 matrix, m[row][column]: m[0][1] is RakNet's m01.
template <class T>
struct Matrix3
{
    T m[3][3]{};

    friend bool operator==(const Matrix3& a, const Matrix3& b)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (a.m[r][c] != b.m[r][c])
                    return false;
        return true;
    }
    friend bool operator!=(const Matrix3& a, const Matrix3& b) { return !(a == b); }
};

namespace bit_stream_detail
{

template <class T>
constexpr void check_component_type()
{
    static_assert(std::is_same<T, float>::value || std::is_same<T, double>::value,
                  "BitStream geometry helpers take float or double components");
}

/// RakNet's (unsigned short) conversion of a quantized value, as x86 compilers carry it out: truncation
/// through a 32-bit integer, keeping the low 16 bits; NaN and values beyond the 32-bit range become 0.
/// (In C++ an out-of-range conversion is undefined; this pins the result RakNet gets in practice.)
inline std::uint16_t truncate_to_uint16(double value) noexcept
{
    if (!(value > -2147483649.0 && value < 2147483648.0))
        return 0;
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
}

// --- Quantized floats ---------------------------------------------------------------------------------
// Local copies of the half-float and compressed-float encodings (ticket #4 adds them to the BitStream
// itself; these can be replaced by those once both are in).

/// RakNet's WriteFloat16(value, min, max): 65535 * (value - min) / (max - min) in float arithmetic,
/// clamped to [0, 65535], truncated to a uint16 in the stream's byte order.
inline void write_float16(BitStreamWriter& out, float value, float min, float max)
{
    float percentile = 65535.0f * (value - min) / (max - min);
    if (percentile < 0.0f)
        percentile = 0.0f;
    if (percentile > 65535.0f)
        percentile = 65535.0f;
    out.write(truncate_to_uint16(percentile));
}

/// RakNet's ReadFloat16: min + (u16 / 65535) * (max - min), clamped to [min, max].
inline float read_float16(BitStreamReader& in, float min, float max)
{
    const auto percentile = in.read<std::uint16_t>();
    if (!in.ok())
        return 0.0f;
    float value = min + (static_cast<float>(percentile) / 65535.0f) * (max - min);
    if (value < min)
        value = min;
    else if (value > max)
        value = max;
    return value;
}

/// RakNet's WriteCompressed(float): the value clamped to [-1, 1], as (uint16)((value + 1) * 32767.5)
/// in float arithmetic, in the stream's byte order.
inline void write_compressed_float(BitStreamWriter& out, float value)
{
    if (value < -1.0f)
        value = -1.0f;
    if (value > 1.0f)
        value = 1.0f;
    out.write(truncate_to_uint16((value + 1.0f) * 32767.5f));
}

/// RakNet's ReadCompressed(float): u16 / 32767.5 - 1.
inline float read_compressed_float(BitStreamReader& in)
{
    const auto compressed = in.read<std::uint16_t>();
    return in.ok() ? static_cast<float>(compressed) / 32767.5f - 1.0f : 0.0f;
}

} // namespace bit_stream_detail

/// Writes a vector of length up to 1 as three half floats in [-1, 1] (RakNet's WriteNormVector):
/// 6 bytes, at the current bit position. Components outside [-1, 1] are clamped.
template <class T>
void write_normalized_vector(BitStreamWriter& out, const Vector3<T>& v)
{
    bit_stream_detail::check_component_type<T>();
    bit_stream_detail::write_float16(out, static_cast<float>(v.x), -1.0f, 1.0f);
    bit_stream_detail::write_float16(out, static_cast<float>(v.y), -1.0f, 1.0f);
    bit_stream_detail::write_float16(out, static_cast<float>(v.z), -1.0f, 1.0f);
}

/// Reads a vector written by write_normalized_vector(). Each component is within 2 / 65535 of the
/// written one. On a short read the reader fails and the result is zero (RakNet's ReadNormVector
/// reports success regardless).
template <class T>
Vector3<T> read_normalized_vector(BitStreamReader& in)
{
    bit_stream_detail::check_component_type<T>();
    Vector3<T> v;
    v.x = bit_stream_detail::read_float16(in, -1.0f, 1.0f);
    v.y = bit_stream_detail::read_float16(in, -1.0f, 1.0f);
    v.z = bit_stream_detail::read_float16(in, -1.0f, 1.0f);
    return in.ok() ? v : Vector3<T>{};
}

/// Writes any vector as its length and direction (RakNet's WriteVector): the length (computed in T)
/// as a float, then, if it is above 1e-5, each component divided by it as a compressed float in
/// [-1, 1] (2 bytes each). 4 or 10 bytes, at the current bit position.
template <class T>
void write_vector(BitStreamWriter& out, const Vector3<T>& v)
{
    bit_stream_detail::check_component_type<T>();
    const T length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    out.write(static_cast<float>(length));
    if (length > 0.00001f)
    {
        bit_stream_detail::write_compressed_float(out, static_cast<float>(v.x / length));
        bit_stream_detail::write_compressed_float(out, static_cast<float>(v.y / length));
        bit_stream_detail::write_compressed_float(out, static_cast<float>(v.z / length));
    }
}

/// Reads a vector written by write_vector(). Each component is within length / 32767.5 (plus float
/// rounding) of the written one; a vector of length 1e-5 or less reads as zero. On a short read the
/// reader fails and the result is zero.
template <class T>
Vector3<T> read_vector(BitStreamReader& in)
{
    bit_stream_detail::check_component_type<T>();
    const auto length = in.read<float>();
    Vector3<T> v;
    if (in.ok() && length > 0.00001f)
    {
        v.x = bit_stream_detail::read_compressed_float(in);
        v.y = bit_stream_detail::read_compressed_float(in);
        v.z = bit_stream_detail::read_compressed_float(in);
        v.x *= length;
        v.y *= length;
        v.z *= length;
    }
    return in.ok() ? v : Vector3<T>{};
}

/// Writes a unit quaternion (RakNet's WriteNormQuat): one sign bit each for w, x, y and z (1 for
/// negative), then |x|, |y| and |z| as (uint16)(|c| * 65535). w is left out; the reader recomputes it.
/// 52 bits, at the current bit position.
template <class T>
void write_normalized_quaternion(BitStreamWriter& out, const Quaternion<T>& q)
{
    bit_stream_detail::check_component_type<T>();
    out.write(q.w < 0.0);
    out.write(q.x < 0.0);
    out.write(q.y < 0.0);
    out.write(q.z < 0.0);
    out.write(bit_stream_detail::truncate_to_uint16(std::fabs(static_cast<double>(q.x)) * 65535.0));
    out.write(bit_stream_detail::truncate_to_uint16(std::fabs(static_cast<double>(q.y)) * 65535.0));
    out.write(bit_stream_detail::truncate_to_uint16(std::fabs(static_cast<double>(q.z)) * 65535.0));
}

/// Reads a quaternion written by write_normalized_quaternion(). x, y and z are within 1 / 65535 of the
/// written ones; w = sqrt(1 - x^2 - y^2 - z^2) (0 if that is negative), computed in float precision as
/// RakNet does, with the written sign. On a short read the reader fails and the result is zero.
template <class T>
Quaternion<T> read_normalized_quaternion(BitStreamReader& in)
{
    bit_stream_detail::check_component_type<T>();
    const bool w_negative = in.read<bool>();
    const bool x_negative = in.read<bool>();
    const bool y_negative = in.read<bool>();
    const bool z_negative = in.read<bool>();
    const auto cx = in.read<std::uint16_t>();
    const auto cy = in.read<std::uint16_t>();
    const auto cz = in.read<std::uint16_t>();
    if (!in.ok())
        return {};

    Quaternion<T> q;
    q.x = static_cast<T>(cx / 65535.0);
    q.y = static_cast<T>(cy / 65535.0);
    q.z = static_cast<T>(cz / 65535.0);
    if (x_negative)
        q.x = -q.x;
    if (y_negative)
        q.y = -q.y;
    if (z_negative)
        q.z = -q.z;
    // Upstream RakNet takes the float overload of sqrt here (and in write_orthogonal_matrix()), also
    // for double components.
    float difference = static_cast<float>(1.0f - q.x * q.x - q.y * q.y - q.z * q.z);
    if (difference < 0.0f)
        difference = 0.0f;
    q.w = static_cast<T>(std::sqrt(difference));
    if (w_negative)
        q.w = -q.w;
    return q;
}

namespace bit_stream_detail
{

/// One quaternion component of WriteOrthMatrix: sqrt(max(0, sum)) / 2 with the sum rounded to float
/// and the float square root (upstream RakNet's overload).
inline double half_root(float sum) noexcept
{
    if (sum < 0.0f)
        sum = 0.0f;
    return static_cast<double>(std::sqrt(sum)) / 2;
}

} // namespace bit_stream_detail

/// Writes a rotation matrix as its unit quaternion (RakNet's WriteOrthMatrix), 52 bits as
/// write_normalized_quaternion(). The quaternion is derived from the diagonal (sums rounded to float)
/// and the signs of x, y, z from m21 - m12, m02 - m20 and m10 - m01, so the matrix must be orthogonal
/// with determinant 1 to come back as written.
template <class T>
void write_orthogonal_matrix(BitStreamWriter& out, const Matrix3<T>& matrix)
{
    bit_stream_detail::check_component_type<T>();
    const auto& m = matrix.m;
    using bit_stream_detail::half_root;
    const double w = half_root(static_cast<float>(1 + m[0][0] + m[1][1] + m[2][2]));
    double x = half_root(static_cast<float>(1 + m[0][0] - m[1][1] - m[2][2]));
    double y = half_root(static_cast<float>(1 - m[0][0] + m[1][1] - m[2][2]));
    double z = half_root(static_cast<float>(1 - m[0][0] - m[1][1] + m[2][2]));
    x = std::copysign(x, static_cast<double>(m[2][1] - m[1][2]));
    y = std::copysign(y, static_cast<double>(m[0][2] - m[2][0]));
    z = std::copysign(z, static_cast<double>(m[1][0] - m[0][1]));
    write_normalized_quaternion(out, Quaternion<double>{w, x, y, z});
}

/// Reads a matrix written by write_orthogonal_matrix(): the quaternion is read in float (as RakNet
/// does) and expanded into a rotation matrix in double arithmetic. On a short read the reader fails
/// and the result is zero.
template <class T>
Matrix3<T> read_orthogonal_matrix(BitStreamReader& in)
{
    bit_stream_detail::check_component_type<T>();
    const Quaternion<float> q = read_normalized_quaternion<float>(in);
    if (!in.ok())
        return {};

    const double qw = q.w, qx = q.x, qy = q.y, qz = q.z;
    const double sqw = qw * qw, sqx = qx * qx, sqy = qy * qy, sqz = qz * qz;
    Matrix3<T> matrix;
    auto& m = matrix.m;
    m[0][0] = static_cast<T>(sqx - sqy - sqz + sqw);
    m[1][1] = static_cast<T>(-sqx + sqy - sqz + sqw);
    m[2][2] = static_cast<T>(-sqx - sqy + sqz + sqw);

    double tmp1 = qx * qy;
    double tmp2 = qz * qw;
    m[1][0] = static_cast<T>(2.0 * (tmp1 + tmp2));
    m[0][1] = static_cast<T>(2.0 * (tmp1 - tmp2));

    tmp1 = qx * qz;
    tmp2 = qy * qw;
    m[2][0] = static_cast<T>(2.0 * (tmp1 - tmp2));
    m[0][2] = static_cast<T>(2.0 * (tmp1 + tmp2));

    tmp1 = qy * qz;
    tmp2 = qx * qw;
    m[2][1] = static_cast<T>(2.0 * (tmp1 + tmp2));
    m[1][2] = static_cast<T>(2.0 * (tmp1 - tmp2));
    return matrix;
}

} // namespace raknet
} // namespace pzynet
