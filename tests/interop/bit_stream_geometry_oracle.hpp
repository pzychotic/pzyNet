#pragma once

// Steps and generators for the BitStream game-math helpers (normalized vectors, vectors, normalized
// quaternions, orthogonal matrices), for the comparison harness in bit_stream_oracle.hpp.
//
// Every step checks, in both directions, that the value read back is exactly what RakNet itself
// decodes from the same bytes (so the round-trip error is RakNet's own), and that it lies within the
// encoding's quantization error of the written value.

#include "bit_stream_oracle.hpp"

#include "pzynet/raknet/bit_stream_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>

namespace oracle
{

// --- values ------------------------------------------------------------------------------------------

template <class T>
const char* component_name()
{
    return std::is_same<T, float>::value ? "float" : "double";
}

template <class T>
T uniform(Rng& rng, T min, T max)
{
    return std::uniform_real_distribution<T>(min, max)(rng);
}

/// A uniformly distributed random unit quaternion.
template <class T>
rn::Quaternion<T> random_unit_quaternion(Rng& rng)
{
    std::normal_distribution<T> normal;
    for (;;)
    {
        const T w = normal(rng), x = normal(rng), y = normal(rng), z = normal(rng);
        const T length = std::sqrt(w * w + x * x + y * y + z * z);
        if (length > T(1e-3))
            return {w / length, x / length, y / length, z / length};
    }
}

/// The rotation matrix of a unit quaternion.
template <class T>
rn::Matrix3<T> rotation_of(const rn::Quaternion<T>& q)
{
    const T w = q.w, x = q.x, y = q.y, z = q.z;
    return rn::Matrix3<T>{{
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    }};
}

/// Whether RakNet builds that take the float and the double overload of sqrt for WriteOrthMatrix's
/// trace sums write the same bytes for `m`. Upstream RakNet (and the fork under MSVC) take the float
/// one, which pzyNet follows; the fork under libstdc++ takes the double one, so a random matrix whose
/// quantized component sits on a truncation boundary would compare differently there. Such matrices
/// are left out of the randomized comparison; the float behaviour is pinned by a hand-checked vector.
template <class T>
bool float_and_double_roots_agree(const rn::Matrix3<T>& matrix)
{
    const auto& m = matrix.m;
    const float sums[] = {
        static_cast<float>(1 + m[0][0] - m[1][1] - m[2][2]),
        static_cast<float>(1 - m[0][0] + m[1][1] - m[2][2]),
        static_cast<float>(1 - m[0][0] - m[1][1] + m[2][2]),
    };
    for (float sum : sums)
    {
        if (sum < 0.0f)
            sum = 0.0f;
        const double by_float = static_cast<double>(std::sqrt(sum)) / 2 * 65535.0;
        const double by_double = std::sqrt(static_cast<double>(sum)) / 2 * 65535.0;
        if (static_cast<long>(by_float) != static_cast<long>(by_double))
            return false;
    }
    return true;
}

template <class T>
std::string describe(const rn::Vector3<T>& v)
{
    std::ostringstream text;
    text.precision(9);
    text << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return text.str();
}

template <class T>
std::string describe(const rn::Quaternion<T>& q)
{
    std::ostringstream text;
    text.precision(9);
    text << "(w " << q.w << ", " << q.x << ", " << q.y << ", " << q.z << ")";
    return text.str();
}

template <class T>
std::string describe(const rn::Matrix3<T>& matrix)
{
    std::ostringstream text;
    text.precision(9);
    for (const auto& row : matrix.m)
        text << "[" << row[0] << " " << row[1] << " " << row[2] << "]";
    return text.str();
}

template <class T>
bool within(T a, T b, double tolerance)
{
    return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= tolerance;
}

/// How far a quaternion's w, recomputed as sqrt(1 - x^2 - y^2 - z^2), can be from the true |w| when
/// x, y and z each carry an error of up to `delta`.
inline double recomputed_w_error(double w, double delta)
{
    const double slack = 3 * (2 * delta + delta * delta) + 1e-6; // + float rounding of the difference
    const double lo = std::sqrt((std::max)(0.0, w * w - slack));
    const double hi = std::sqrt(w * w + slack);
    return (std::max)(std::fabs(w) - lo, hi - std::fabs(w));
}

// --- step factories ----------------------------------------------------------------------------------

/// A normalized vector (WriteNormVector / ReadNormVector). Components within [-1, 1].
template <class T>
Step normalized_vector(rn::Vector3<T> v)
{
    RakNet::BitStream encoded;
    encoded.WriteNormVector(v.x, v.y, v.z);
    rn::Vector3<T> decoded;
    (void)encoded.ReadNormVector(decoded.x, decoded.y, decoded.z);

    const auto close = [v](const rn::Vector3<T>& r) {
        const double step = 2.0 / 65535 + 1e-6;
        return within(r.x, v.x, step) && within(r.y, v.y, step) && within(r.z, v.z, step);
    };
    Step step;
    step.what = std::string("normalized vector<") + component_name<T>() + "> " + describe(v);
    step.write_ours = [v](rn::BitStreamWriter& out) { rn::write_normalized_vector(out, v); };
    step.write_theirs = [v](RakNet::BitStream& out) { out.WriteNormVector(v.x, v.y, v.z); };
    step.read_ours = [=](rn::BitStreamReader& in) {
        const rn::Vector3<T> r = rn::read_normalized_vector<T>(in);
        return in.ok() && same(r, decoded) && close(r);
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        rn::Vector3<T> r;
        return in.GetNumberOfUnreadBits() >= 48 && in.ReadNormVector(r.x, r.y, r.z) && same(r, decoded) &&
               close(r);
    };
    return step;
}

/// A vector of any length (WriteVector / ReadVector).
template <class T>
Step vector(rn::Vector3<T> v)
{
    RakNet::BitStream encoded;
    encoded.WriteVector(v.x, v.y, v.z);
    rn::Vector3<T> decoded;
    (void)encoded.ReadVector(decoded.x, decoded.y, decoded.z);

    const auto close = [v](const rn::Vector3<T>& r) {
        const double length = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                                        static_cast<double>(v.z) * v.z);
        const double step = length <= 0.00001 ? 0.00001 : length * (1.0 / 32767.5 + 1e-6);
        return within(r.x, v.x, step) && within(r.y, v.y, step) && within(r.z, v.z, step);
    };
    Step step;
    step.what = std::string("vector<") + component_name<T>() + "> " + describe(v);
    step.write_ours = [v](rn::BitStreamWriter& out) { rn::write_vector(out, v); };
    step.write_theirs = [v](RakNet::BitStream& out) { out.WriteVector(v.x, v.y, v.z); };
    step.read_ours = [=](rn::BitStreamReader& in) {
        const rn::Vector3<T> r = rn::read_vector<T>(in);
        return in.ok() && same(r, decoded) && close(r);
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        rn::Vector3<T> r;
        return in.ReadVector(r.x, r.y, r.z) && same(r, decoded) && close(r);
    };
    return step;
}

/// A normalized quaternion (WriteNormQuat / ReadNormQuat).
///
/// For double components the recomputed w is compared to RakNet's within float precision only: upstream
/// RakNet (and pzyNet) take the float sqrt, the fork under libstdc++ the double one.
template <class T>
Step normalized_quaternion(rn::Quaternion<T> q)
{
    RakNet::BitStream encoded;
    encoded.WriteNormQuat(q.w, q.x, q.y, q.z);
    rn::Quaternion<T> decoded;
    (void)encoded.ReadNormQuat(decoded.w, decoded.x, decoded.y, decoded.z);

    const auto as_decoded = [decoded](const rn::Quaternion<T>& r) {
        const bool w_matches = std::is_same<T, float>::value ? same(r.w, decoded.w) : within(r.w, decoded.w, 1e-6);
        return w_matches && same(r.x, decoded.x) && same(r.y, decoded.y) && same(r.z, decoded.z);
    };
    const auto close = [q](const rn::Quaternion<T>& r) {
        const double step = 1.0 / 65535 + 1e-7;
        return within(r.x, q.x, step) && within(r.y, q.y, step) && within(r.z, q.z, step) &&
               std::signbit(r.w) == (q.w < 0) &&
               within(std::fabs(r.w), std::fabs(q.w), recomputed_w_error(q.w, step));
    };
    Step step;
    step.what = std::string("normalized quaternion<") + component_name<T>() + "> " + describe(q);
    step.write_ours = [q](rn::BitStreamWriter& out) { rn::write_normalized_quaternion(out, q); };
    step.write_theirs = [q](RakNet::BitStream& out) { out.WriteNormQuat(q.w, q.x, q.y, q.z); };
    step.read_ours = [=](rn::BitStreamReader& in) {
        const rn::Quaternion<T> r = rn::read_normalized_quaternion<T>(in);
        return in.ok() && as_decoded(r) && close(r);
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        rn::Quaternion<T> r;
        return in.ReadNormQuat(r.w, r.x, r.y, r.z) && as_decoded(r) && close(r);
    };
    return step;
}

/// An orthogonal matrix (WriteOrthMatrix / ReadOrthMatrix), a rotation not too close to 180 degrees:
/// the encoding takes the signs of x, y and z from products with w, so they are lost as w nears 0.
template <class T>
Step orthogonal_matrix(rn::Matrix3<T> m)
{
    // Round-trip error bound per element. x, y and z come from square roots of float trace sums
    // (error up to sqrt(float rounding), about 5e-4) and are quantized to 1 / 65535; w is recomputed
    // from them. Each element is a sum of products of two components, so it moves by at most
    // 8 d + 4 d^2 for a component error d.
    const double true_w = std::sqrt((std::max)(0.0, 1.0 + m.m[0][0] + m.m[1][1] + m.m[2][2])) / 2;
    const double delta = 1.0 / 65535 + 5e-4;
    const double error = delta + recomputed_w_error(true_w, delta);
    const double tolerance = 8 * error + 4 * error * error;

    RakNet::BitStream encoded;
    const auto& e = m.m;
    encoded.WriteOrthMatrix(e[0][0], e[0][1], e[0][2], e[1][0], e[1][1], e[1][2], e[2][0], e[2][1], e[2][2]);
    rn::Matrix3<T> decoded;
    auto& d = decoded.m;
    (void)encoded.ReadOrthMatrix(d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2], d[2][0], d[2][1], d[2][2]);

    const auto close = [m, tolerance](const rn::Matrix3<T>& r) {
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                if (!within(r.m[row][column], m.m[row][column], tolerance))
                    return false;
        return true;
    };
    Step step;
    step.what = std::string("orthogonal matrix<") + component_name<T>() + "> " + describe(m);
    step.write_ours = [m](rn::BitStreamWriter& out) { rn::write_orthogonal_matrix(out, m); };
    step.write_theirs = [m](RakNet::BitStream& out) {
        const auto& e = m.m;
        out.WriteOrthMatrix(e[0][0], e[0][1], e[0][2], e[1][0], e[1][1], e[1][2], e[2][0], e[2][1], e[2][2]);
    };
    step.read_ours = [=](rn::BitStreamReader& in) {
        const rn::Matrix3<T> r = rn::read_orthogonal_matrix<T>(in);
        return in.ok() && same(r, decoded) && close(r);
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        rn::Matrix3<T> r;
        auto& o = r.m;
        return in.ReadOrthMatrix(o[0][0], o[0][1], o[0][2], o[1][0], o[1][1], o[1][2], o[2][0], o[2][1], o[2][2]) &&
               same(r, decoded) && close(r);
    };
    return step;
}

// --- generators --------------------------------------------------------------------------------------

template <class T>
StepGenerator normalized_vector_of()
{
    return [](Rng& rng) {
        if (rng() % 2 == 0)
        {
            const rn::Quaternion<T> q = random_unit_quaternion<T>(rng); // any unit vector
            const T length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
            if (length > T(1e-3))
                return normalized_vector(rn::Vector3<T>{q.x / length, q.y / length, q.z / length});
        }
        return normalized_vector(rn::Vector3<T>{uniform<T>(rng, -1, 1), uniform<T>(rng, -1, 1), uniform<T>(rng, -1, 1)});
    };
}

template <class T>
StepGenerator vector_of()
{
    return [](Rng& rng) {
        const T scale = std::pow(T(10), uniform<T>(rng, -7, 4)); // lengths from below 1e-5 to 10000s
        return vector(rn::Vector3<T>{uniform<T>(rng, -1, 1) * scale, uniform<T>(rng, -1, 1) * scale,
                                     uniform<T>(rng, -1, 1) * scale});
    };
}

template <class T>
StepGenerator normalized_quaternion_of()
{
    return [](Rng& rng) { return normalized_quaternion(random_unit_quaternion<T>(rng)); };
}

template <class T>
StepGenerator orthogonal_matrix_of()
{
    return [](Rng& rng) {
        for (;;)
        {
            const rn::Quaternion<T> q = random_unit_quaternion<T>(rng);
            const rn::Matrix3<T> m = rotation_of(q);
            if (std::fabs(q.w) > T(0.01) && float_and_double_roots_agree(m))
                return orthogonal_matrix(m);
        }
    };
}

/// The game-math helpers, each for float and double components.
inline std::vector<StepGenerator> geometry_generators()
{
    return {
        normalized_vector_of<float>(),     normalized_vector_of<double>(), vector_of<float>(),
        vector_of<double>(),               normalized_quaternion_of<float>(), normalized_quaternion_of<double>(),
        orthogonal_matrix_of<float>(),     orthogonal_matrix_of<double>(),
    };
}

} // namespace oracle
