#pragma once

// Steps and generators for RakNet's space-saving BitStream encodings (compressed, delta, half-float and
// range-packed), for the comparison harness in bit_stream_oracle.hpp.
//
// The value a read must give back is what RakNet itself reads after writing it: identical to the written
// value for lossless encodings, the quantised value for compressed floats and float16, and the reader's
// previous value for an unchanged delta.

#include "bit_stream_oracle.hpp"

#include <algorithm>
#include <limits>

namespace oracle
{

/// What RakNet reads back after `write` into a fresh stream, starting from `initial`.
template <class T, class Write, class Read>
T raknet_round_trip(T initial, Write write, Read read)
{
    RakNet::BitStream stream;
    write(stream);
    T value = initial;
    const bool ok = read(stream, value);
    REQUIRE(ok);
    return value;
}

inline std::string type_name_of(const char* kind, const std::type_info& type, std::size_t size)
{
    return std::string(kind) + " " + type.name() + " (" + std::to_string(size) + " bytes)";
}

// --- step factories ----------------------------------------------------------------------------------

/// write_compressed / WriteCompressed, read_compressed / ReadCompressed.
template <class T>
Step compressed(T value)
{
    const T expected = raknet_round_trip(
        T{}, [&](RakNet::BitStream& out) { out.WriteCompressed(value); },
        [](RakNet::BitStream& in, T& v) { return in.ReadCompressed(v); });
    Step step;
    step.what = type_name_of("compressed", typeid(T), sizeof(T));
    step.write_ours = [value](rn::BitStreamWriter& out) { out.write_compressed(value); };
    step.write_theirs = [value](RakNet::BitStream& out) { out.WriteCompressed(value); };
    step.read_ours = [expected](rn::BitStreamReader& in) { return same(in.read_compressed<T>(), expected); };
    step.read_theirs = [expected](RakNet::BitStream& in) {
        T read{};
        return in.ReadCompressed(read) && same(read, expected);
    };
    return step;
}

/// A delta write: with or without a last value, plain or compressed. The readers start from `last`
/// (or a value-initialised T without one), so an unchanged value reads back as last.
template <class T>
Step delta(T current, T last, bool has_last, bool compressed)
{
    const auto write_theirs = [=](RakNet::BitStream& out) {
        if (compressed)
            has_last ? out.WriteCompressedDelta(current, last) : out.WriteCompressedDelta(current);
        else
            has_last ? out.WriteDelta(current, last) : out.WriteDelta(current);
    };
    const auto read_theirs_into = [=](RakNet::BitStream& in, T& value) {
        return compressed ? in.ReadCompressedDelta(value) : in.ReadDelta(value);
    };
    const T initial = has_last ? last : T{};
    const T expected = raknet_round_trip(initial, write_theirs, read_theirs_into);

    Step step;
    step.what = type_name_of(compressed ? "compressed delta" : "delta", typeid(T), sizeof(T)) +
                (has_last ? (current == last ? " unchanged" : " changed") : " without last");
    step.write_ours = [=](rn::BitStreamWriter& out) {
        if (compressed)
            has_last ? out.write_compressed_delta(current, last) : out.write_compressed_delta(current);
        else
            has_last ? out.write_delta(current, last) : out.write_delta(current);
    };
    step.write_theirs = write_theirs;
    step.read_ours = [=](rn::BitStreamReader& in) {
        T value = initial;
        const bool ok = compressed ? in.read_compressed_delta(value) : in.read_delta(value);
        return ok && same(value, expected);
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        T value = initial;
        return read_theirs_into(in, value) && same(value, expected);
    };
    return step;
}

/// write_float16 / WriteFloat16, read_float16 / ReadFloat16.
inline Step float16(float value, float min, float max)
{
    const float expected = raknet_round_trip(
        0.0f, [&](RakNet::BitStream& out) { out.WriteFloat16(value, min, max); },
        [&](RakNet::BitStream& in, float& v) { return in.ReadFloat16(v, min, max); });
    Step step;
    step.what = "float16 " + std::to_string(value) + " in [" + std::to_string(min) + ", " + std::to_string(max) + "]";
    step.write_ours = [=](rn::BitStreamWriter& out) { out.write_float16(value, min, max); };
    step.write_theirs = [=](RakNet::BitStream& out) { out.WriteFloat16(value, min, max); };
    step.read_ours = [=](rn::BitStreamReader& in) { return same(in.read_float16(min, max), expected); };
    step.read_theirs = [=](RakNet::BitStream& in) {
        float read = 0.0f;
        return in.ReadFloat16(read, min, max) && same(read, expected);
    };
    return step;
}

/// write_ranged / WriteBitsFromIntegerRange, read_ranged / ReadBitsFromIntegerRange. T must be one of
/// the <cstdint> fixed-width types (RakNet's NumberOfLeadingZeroes has overloads only for those).
///
/// With min == max RakNet writes no bits for an in-range value but its read fails (ReadBits of zero
/// bits); pzyNet's read succeeds with min. The RakNet read is then skipped - there is nothing to read
/// but the flag bit.
template <class T>
Step ranged(T value, T min, T max, bool allow_outside_range)
{
    const bool inside = value >= min && value <= max;
    Step step;
    step.what = type_name_of("ranged", typeid(T), sizeof(T)) + " " + std::to_string(value) + " in [" +
                std::to_string(min) + ", " + std::to_string(max) + "]" + (allow_outside_range ? " allowing outside" : "");
    step.write_ours = [=](rn::BitStreamWriter& out) { out.write_ranged(value, min, max, allow_outside_range); };
    step.write_theirs = [=](RakNet::BitStream& out) {
        out.WriteBitsFromIntegerRange(value, min, max, allow_outside_range);
    };
    step.read_ours = [=](rn::BitStreamReader& in) {
        return in.read_ranged(min, max, allow_outside_range) == value && in.ok();
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        if (min == max && inside)
        {
            bool outside = false;
            return !allow_outside_range || (in.Read(outside) && !outside);
        }
        T read{};
        return in.ReadBitsFromIntegerRange(read, min, max, allow_outside_range) && read == value;
    };
    return step;
}

// --- values ------------------------------------------------------------------------------------------

/// A random integer with some of its bytes cleared, so the compressed encoding's zero-byte paths are
/// taken as often as its full-width one; sometimes one of 0, -1 / all ones, the minimum or maximum.
template <class T, bool = std::is_enum<T>::value>
struct integer_of
{
    using type = T;
};
template <class T>
struct integer_of<T, true>
{
    using type = std::underlying_type_t<T>;
};

template <class T>
T compressible_value(Rng& rng)
{
    using U = std::make_unsigned_t<typename integer_of<T>::type>;
    switch (rng() % 8)
    {
    case 0: return static_cast<T>(U{0});
    case 1: return static_cast<T>(static_cast<U>(~U{0}));
    case 2: return static_cast<T>(static_cast<U>(U{1} << (sizeof(U) * 8 - 1))); // signed minimum
    case 3: return static_cast<T>(static_cast<U>(static_cast<U>(~U{0}) >> 1));  // signed maximum
    default: break;
    }
    auto bits = static_cast<U>(rng());
    for (std::size_t byte = 0; byte < sizeof(U); ++byte)
        if (rng() % 2 == 0)
            bits = static_cast<U>(bits & ~static_cast<U>(U{0xFF} << (byte * 8)));
    if (rng() % 2 == 0)
        bits = static_cast<U>(bits & static_cast<U>(~U{0xF0})); // low byte fits a nibble
    return static_cast<T>(bits);
}

/// A value for a compressed float or double: in [-1, 1] (RakNet asserts on anything outside), often
/// one of the ends or 0.
template <class T>
T unit_value(Rng& rng)
{
    switch (rng() % 6)
    {
    case 0: return T(-1);
    case 1: return T(0);
    case 2: return T(1);
    default: return std::uniform_real_distribution<T>(T(-1), T(1))(rng);
    }
}

// --- generators --------------------------------------------------------------------------------------

template <class T>
StepGenerator compressed_integer_of()
{
    return [](Rng& rng) { return compressed(compressible_value<T>(rng)); };
}

/// Compressed writes of every integer, character and enum type, of floats and doubles in [-1, 1] and
/// of bools (written unchanged).
inline std::vector<StepGenerator> compressed_generators()
{
    return {
        compressed_integer_of<char>(),          compressed_integer_of<signed char>(),
        compressed_integer_of<unsigned char>(), compressed_integer_of<short>(),
        compressed_integer_of<unsigned short>(), compressed_integer_of<int>(),
        compressed_integer_of<unsigned int>(),  compressed_integer_of<long>(),
        compressed_integer_of<unsigned long>(), compressed_integer_of<long long>(),
        compressed_integer_of<unsigned long long>(), compressed_integer_of<wchar_t>(),
        compressed_integer_of<char16_t>(),      compressed_integer_of<char32_t>(),
        compressed_integer_of<PlainEnum>(),     compressed_integer_of<ByteEnum>(),
        compressed_integer_of<WideEnum>(),
        [](Rng& rng) { return compressed(unit_value<float>(rng)); },
        [](Rng& rng) { return compressed(unit_value<double>(rng)); },
        [](Rng& rng) { return compressed(random_value<bool>(rng)); },
    };
}

template <class T>
StepGenerator delta_of(T (*value)(Rng&), bool compressed)
{
    return [=](Rng& rng) {
        const T current = value(rng);
        const T last = rng() % 2 == 0 ? current : value(rng);
        // RakNet's WriteDelta(bool) without a last value writes a flag bit ReadDelta(bool) does not
        // read; no RakNet message can use it, so it is pinned by a hand vector only.
        const bool has_last = std::is_same<T, bool>::value && !compressed ? true : rng() % 4 != 0;
        return delta(current, last, has_last, compressed);
    };
}

/// Plain and compressed delta writes of a selection of types.
inline std::vector<StepGenerator> delta_generators()
{
    std::vector<StepGenerator> generators;
    for (bool compressed : {false, true})
    {
        generators.push_back(delta_of<bool>(random_value<bool>, compressed));
        generators.push_back(delta_of<std::uint8_t>(compressible_value<std::uint8_t>, compressed));
        generators.push_back(delta_of<std::int16_t>(compressible_value<std::int16_t>, compressed));
        generators.push_back(delta_of<std::uint32_t>(compressible_value<std::uint32_t>, compressed));
        generators.push_back(delta_of<std::int64_t>(compressible_value<std::int64_t>, compressed));
        generators.push_back(delta_of<WideEnum>(compressible_value<WideEnum>, compressed));
    }
    generators.push_back(delta_of<float>(random_value<float>, false));
    generators.push_back(delta_of<double>(random_value<double>, false));
    generators.push_back(delta_of<float>(unit_value<float>, true));
    generators.push_back(delta_of<double>(unit_value<double>, true));
    return generators;
}

/// A float16 range and a value in it, often one of its ends.
inline Step random_float16(Rng& rng)
{
    const float a = std::uniform_real_distribution<float>(-1000.0f, 1000.0f)(rng);
    const float b = std::uniform_real_distribution<float>(-1000.0f, 1000.0f)(rng);
    const float min = (std::min)(a, b);
    const float max = min == (std::max)(a, b) ? min + 1.0f : (std::max)(a, b);
    switch (rng() % 4)
    {
    case 0: return float16(min, min, max);
    case 1: return float16(max, min, max);
    default: return float16(std::uniform_real_distribution<float>(min, max)(rng), min, max);
    }
}

/// A range [min, max] of T (often narrow, sometimes empty or the whole type) and a value: in it, or,
/// with allow_outside_range, sometimes outside it.
template <class T>
Step random_ranged(Rng& rng)
{
    T a = random_value<T>(rng), b = random_value<T>(rng);
    switch (rng() % 4)
    {
    case 0: // narrow range
        b = static_cast<T>(static_cast<std::make_unsigned_t<T>>(a) + rng() % 300);
        break;
    case 1: // empty range
        b = a;
        break;
    case 2:
        a = std::numeric_limits<T>::lowest();
        b = (std::numeric_limits<T>::max)();
        break;
    default: break;
    }
    const T min = (std::min)(a, b), max = (std::max)(a, b);
    const bool allow_outside_range = rng() % 2 == 0;
    T value = random_value<T>(rng);
    if (!allow_outside_range || rng() % 2 == 0)
    {
        switch (rng() % 4)
        {
        case 0: value = min; break;
        case 1: value = max; break;
        default: {
            using U = std::make_unsigned_t<T>;
            const auto span = static_cast<U>(static_cast<U>(max) - static_cast<U>(min));
            const auto offset = span == (std::numeric_limits<U>::max)() ? static_cast<U>(rng())
                                                                        : static_cast<U>(rng() % (std::uint64_t{span} + 1));
            value = static_cast<T>(static_cast<U>(static_cast<U>(min) + offset));
        }
        }
    }
    return ranged(value, min, max, allow_outside_range);
}

/// Half-precision floats and range-packed integers of every fixed-width integer type.
inline std::vector<StepGenerator> packed_generators()
{
    return {
        random_float16,
        random_ranged<std::int8_t>,
        random_ranged<std::uint8_t>,
        random_ranged<std::int16_t>,
        random_ranged<std::uint16_t>,
        random_ranged<std::int32_t>,
        random_ranged<std::uint32_t>,
        random_ranged<std::int64_t>,
        random_ranged<std::uint64_t>,
    };
}

} // namespace oracle
