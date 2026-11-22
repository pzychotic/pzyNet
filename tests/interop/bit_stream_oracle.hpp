#pragma once

// Comparison harness for pzyNet's BitStream against RakNet's own RakNet::BitStream (the oracle).
//
// A test describes a message as a sequence of Steps. Each Step says how both implementations write one
// item and how both read it back and check it. check_against_raknet() then proves, for the sequence:
//   1. both writers produce the same bit count and the same bytes;
//   2. RakNet reads pzyNet's bytes and gets every value back;
//   3. pzyNet reads RakNet's bytes and gets every value back;
//   4. after every step both readers stand at the same bit position.
//
// Extending it (later encoding tickets): write a Step factory for the new encoding (see scalar() or
// raw_bits() below for the pattern), a StepGenerator that draws random arguments for it, and add the
// generator to the list passed to random_sequence(). The checks need no change.
//
// The same source is compiled against a stock RakNet build (big-endian) and against one built with
// __BITSTREAM_NATIVE_END (PZYNET_ORACLE_NATIVE_END, little-endian on the x86/ARM hosts we test on);
// `byte_order` is the pzyNet setting that matches the oracle in use.

#include "pzynet/raknet/bit_stream.hpp"

#include "BitStream.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <typeinfo>
#include <vector>

namespace oracle
{

namespace rn = pzynet::raknet;
using Bytes = std::vector<std::uint8_t>;
using Rng = std::mt19937_64;

#ifdef PZYNET_ORACLE_NATIVE_END
constexpr rn::ByteOrder byte_order = rn::ByteOrder::little_endian;
constexpr const char* oracle_name = "RakNet BitStream built with __BITSTREAM_NATIVE_END";
#else
constexpr rn::ByteOrder byte_order = rn::ByteOrder::big_endian;
constexpr const char* oracle_name = "stock RakNet BitStream";
#endif

/// One item of a message, as both implementations write and read it. The read functions return
/// whether the item read back equals what was written.
struct Step
{
    std::string what; // shown when a check fails
    std::function<void(rn::BitStreamWriter&)> write_ours;
    std::function<void(RakNet::BitStream&)> write_theirs;
    std::function<bool(rn::BitStreamReader&)> read_ours;
    std::function<bool(RakNet::BitStream&)> read_theirs;
};

using StepGenerator = std::function<Step(Rng&)>;

// --- values ----------------------------------------------------------------------------------------

// Enums of the kinds RakNet applications write: an unscoped one without a fixed type (int-sized,
// its enumerators span all of int so every int is a valid value), and fixed narrow and wide ones.
enum PlainEnum
{
    plain_min = INT_MIN,
    plain_max = INT_MAX,
};
enum class ByteEnum : std::uint8_t
{
};
enum class WideEnum : std::int64_t
{
};

template <class T>
T random_value(Rng& rng)
{
    if constexpr (std::is_same<T, bool>::value)
        return (rng() & 1) != 0;
    else if constexpr (std::is_enum<T>::value)
        return static_cast<T>(random_value<std::underlying_type_t<T>>(rng));
    else if constexpr (std::is_integral<T>::value)
        return static_cast<T>(static_cast<std::make_unsigned_t<T>>(rng()));
    else
    {
        // Any bit pattern but NaN, so equality is meaningful; infinities and denormals included.
        for (;;)
        {
            const auto bits = static_cast<std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>(rng());
            T value;
            std::memcpy(&value, &bits, sizeof(T));
            if (!std::isnan(value))
                return value;
        }
    }
}

inline Bytes random_bytes(Rng& rng, std::size_t count)
{
    Bytes bytes(count);
    for (auto& b : bytes)
        b = static_cast<std::uint8_t>(rng());
    return bytes;
}

inline std::size_t random_size(Rng& rng, std::size_t min, std::size_t max)
{
    return std::uniform_int_distribution<std::size_t>(min, max)(rng);
}

template <class T>
bool same(const T& a, const T& b)
{
    return std::memcmp(&a, &b, sizeof(T)) == 0; // bitwise, so -0.0 and 0.0 differ
}

inline std::string hex(const Bytes& bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string text;
    for (std::uint8_t b : bytes)
    {
        if (!text.empty())
            text += ' ';
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text;
}

// --- step factories ----------------------------------------------------------------------------------

/// A scalar written with write(value) / RakNet's Write(value) and read with read<T>() / Read(T&).
template <class T>
Step scalar(T value)
{
    Step step;
    step.what = std::string("scalar ") + typeid(T).name() + " (" + std::to_string(sizeof(T)) + " bytes)";
    step.write_ours = [value](rn::BitStreamWriter& out) { out.write(value); };
    step.write_theirs = [value](RakNet::BitStream& out) { out.Write(value); };
    step.read_ours = [value](rn::BitStreamReader& in) { return same(in.read<T>(), value); };
    step.read_theirs = [value](RakNet::BitStream& in) {
        T read{};
        return in.Read(read) && same(read, value);
    };
    return step;
}

/// `bytes` with the bits of a partial last byte that are not among the first `bit_count` cleared:
/// the high ones for a right-aligned tail, the low ones for a left-aligned one.
inline Bytes only_bits(Bytes bytes, std::size_t bit_count, bool right_aligned)
{
    if (const std::size_t tail = bit_count % 8)
    {
        const unsigned low_mask = (1u << tail) - 1;
        const unsigned keep = right_aligned ? low_mask : low_mask << (8 - tail);
        bytes[bit_count / 8] = static_cast<std::uint8_t>(bytes[bit_count / 8] & keep);
    }
    return bytes;
}

/// `bit_count` raw bits of `bytes` (WriteBits / ReadBits with the given alignment of the tail);
/// `bytes` holds exactly (bit_count + 7) / 8 bytes. bit_count must be at least 1: RakNet's ReadBits
/// fails on zero bits.
/// - A right-aligned tail may carry junk in its unused high bits; both writers ignore it.
/// - A left-aligned tail must have zero low bits: RakNet's WriteBits copies them into the stream,
///   where pzyNet writes only the bits asked for.
/// - Reading a left-aligned tail, RakNet's ReadBits leaves the stream's following bits in the unused
///   low bits; pzyNet zeroes them. The RakNet read is therefore compared on the requested bits only.
inline Step raw_bits(Bytes bytes, std::size_t bit_count, bool right_aligned)
{
    const Bytes expected = only_bits(bytes, bit_count, right_aligned);
    Step step;
    step.what = "raw bits " + std::to_string(bit_count) + (right_aligned ? " right-aligned " : " left-aligned ") +
                "[" + hex(bytes) + "]";
    step.write_ours = [=](rn::BitStreamWriter& out) { out.write_bits(bytes, bit_count, right_aligned); };
    step.write_theirs = [=](RakNet::BitStream& out) {
        out.WriteBits(bytes.data(), static_cast<RakNet::BitSize_t>(bit_count), right_aligned);
    };
    step.read_ours = [=](rn::BitStreamReader& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.read_bits(read, bit_count, right_aligned) && read == expected;
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.ReadBits(read.data(), static_cast<RakNet::BitSize_t>(bit_count), right_aligned) &&
               only_bits(read, bit_count, right_aligned) == expected;
    };
    return step;
}

/// Raw bytes at the current bit position (Write(const char*, n) / Read(char*, n)). At least one byte:
/// RakNet's Read(char*, 0) fails at an unaligned position.
inline Step raw_bytes(Bytes bytes)
{
    Step step;
    step.what = "raw bytes [" + hex(bytes) + "]";
    step.write_ours = [=](rn::BitStreamWriter& out) { out.write_bytes(bytes); };
    step.write_theirs = [=](RakNet::BitStream& out) {
        out.Write(reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned>(bytes.size()));
    };
    step.read_ours = [=](rn::BitStreamReader& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.read_bytes(read) && read == bytes;
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.Read(reinterpret_cast<char*>(read.data()), static_cast<unsigned>(read.size())) && read == bytes;
    };
    return step;
}

/// Aligned bytes (WriteAlignedBytes / ReadAlignedBytes). At least one byte: RakNet's
/// ReadAlignedBytes fails on zero bytes without aligning, where pzyNet aligns.
inline Step aligned_bytes(Bytes bytes)
{
    Step step;
    step.what = "aligned bytes [" + hex(bytes) + "]";
    step.write_ours = [=](rn::BitStreamWriter& out) { out.write_aligned_bytes(bytes); };
    step.write_theirs = [=](RakNet::BitStream& out) {
        out.WriteAlignedBytes(bytes.data(), static_cast<unsigned>(bytes.size()));
    };
    step.read_ours = [=](rn::BitStreamReader& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.read_aligned_bytes(read) && read == bytes;
    };
    step.read_theirs = [=](RakNet::BitStream& in) {
        Bytes read(bytes.size(), 0xEE);
        return in.ReadAlignedBytes(read.data(), static_cast<unsigned>(read.size())) && read == bytes;
    };
    return step;
}

/// Byte alignment on both sides (AlignWriteToByteBoundary / AlignReadToByteBoundary).
inline Step align()
{
    Step step;
    step.what = "align";
    step.write_ours = [](rn::BitStreamWriter& out) { out.align(); };
    step.write_theirs = [](RakNet::BitStream& out) { out.AlignWriteToByteBoundary(); };
    step.read_ours = [](rn::BitStreamReader& in) {
        in.align();
        return in.ok();
    };
    step.read_theirs = [](RakNet::BitStream& in) {
        in.AlignReadToByteBoundary();
        return true;
    };
    return step;
}

/// Raw bits the reader skips over (skip_bits / IgnoreBits) instead of reading.
inline Step skipped_bits(Bytes bytes, std::size_t bit_count)
{
    Step step = raw_bits(std::move(bytes), bit_count, true);
    step.what = "skipped " + step.what;
    step.read_ours = [bit_count](rn::BitStreamReader& in) {
        in.skip_bits(bit_count);
        return in.ok();
    };
    step.read_theirs = [bit_count](RakNet::BitStream& in) {
        if (in.GetNumberOfUnreadBits() < bit_count)
            return false;
        in.IgnoreBits(static_cast<RakNet::BitSize_t>(bit_count));
        return true;
    };
    return step;
}

// --- generators --------------------------------------------------------------------------------------

template <class T>
StepGenerator scalar_of()
{
    return [](Rng& rng) { return scalar(random_value<T>(rng)); };
}

/// Every scalar type a RakNet application writes with Write(): bool, all integer and character
/// types, enums, float and double.
inline std::vector<StepGenerator> scalar_generators()
{
    return {
        scalar_of<bool>(),          scalar_of<char>(),          scalar_of<signed char>(),
        scalar_of<unsigned char>(), scalar_of<short>(),         scalar_of<unsigned short>(),
        scalar_of<int>(),           scalar_of<unsigned int>(),  scalar_of<long>(),
        scalar_of<unsigned long>(), scalar_of<long long>(),     scalar_of<unsigned long long>(),
        scalar_of<wchar_t>(),       scalar_of<char16_t>(),      scalar_of<char32_t>(),
        scalar_of<PlainEnum>(),     scalar_of<ByteEnum>(),      scalar_of<WideEnum>(),
        scalar_of<float>(),         scalar_of<double>(),
    };
}

/// Raw bits (both tail alignments), raw and aligned bytes, alignment and skipping.
inline std::vector<StepGenerator> raw_generators()
{
    return {
        [](Rng& rng) {
            const std::size_t bits = random_size(rng, 1, 40);
            return raw_bits(random_bytes(rng, (bits + 7) / 8), bits, true);
        },
        [](Rng& rng) {
            const std::size_t bits = random_size(rng, 1, 40);
            return raw_bits(only_bits(random_bytes(rng, (bits + 7) / 8), bits, false), bits, false);
        },
        [](Rng& rng) { return raw_bytes(random_bytes(rng, random_size(rng, 1, 6))); },
        [](Rng& rng) { return aligned_bytes(random_bytes(rng, random_size(rng, 1, 6))); },
        [](Rng&) { return align(); },
        [](Rng& rng) {
            const std::size_t bits = random_size(rng, 1, 20);
            return skipped_bits(random_bytes(rng, (bits + 7) / 8), bits);
        },
    };
}

inline std::vector<StepGenerator> concat(std::vector<StepGenerator> a, const std::vector<StepGenerator>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// `count` steps, each from a generator picked uniformly at random.
inline std::vector<Step> random_sequence(Rng& rng, const std::vector<StepGenerator>& generators, std::size_t count)
{
    std::vector<Step> steps;
    steps.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        steps.push_back(generators[random_size(rng, 0, generators.size() - 1)](rng));
    return steps;
}

// --- checks ------------------------------------------------------------------------------------------

inline Bytes bytes_of(const RakNet::BitStream& stream)
{
    return Bytes(stream.GetData(), stream.GetData() + stream.GetNumberOfBytesUsed());
}

/// Writes `steps` with both implementations and checks them against each other in both directions
/// (see the top of this file).
inline void check_against_raknet(const std::vector<Step>& steps)
{
    INFO("oracle: " << oracle_name);

    rn::BitStreamWriter ours(byte_order);
    RakNet::BitStream theirs;
    for (const Step& step : steps)
    {
        step.write_ours(ours);
        step.write_theirs(theirs);
    }
    const Bytes our_bytes(ours.data().begin(), ours.data().end());
    const Bytes their_bytes = bytes_of(theirs);
    CHECK(ours.bit_count() == theirs.GetNumberOfBitsUsed());
    REQUIRE(hex(our_bytes) == hex(their_bytes));

    // Each reads the other's bytes.
    rn::BitStreamReader our_reader(their_bytes, byte_order);
    RakNet::BitStream their_reader(const_cast<unsigned char*>(our_bytes.data()),
                                   static_cast<unsigned int>(our_bytes.size()), true);
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        INFO("step " << i << ": " << steps[i].what);
        const bool our_read = steps[i].read_ours(our_reader);
        const bool their_read = steps[i].read_theirs(their_reader);
        CHECK(our_read);
        CHECK(their_read);
        CHECK(our_reader.bit_position() == their_reader.GetReadOffset());
        if (!our_read || !their_read)
            break;
    }
    CHECK(our_reader.ok());
}

} // namespace oracle
