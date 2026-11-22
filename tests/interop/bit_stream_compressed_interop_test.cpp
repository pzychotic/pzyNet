// Byte-for-byte comparison of pzyNet's compressed, delta, half-float and range-packed BitStream encodings
// with RakNet's own BitStream, in both directions and (through the native-end build) both byte orders.

#include "bit_stream_compressed_oracle.hpp"

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cstdint>
#include <limits>

using namespace oracle;

TEST_CASE("BitStream: compressed integers at their boundaries match RakNet", "[interop][bitstream][compressed]")
{
    check_against_raknet({
        compressed(std::uint8_t{0}),
        compressed(std::uint8_t{0x0F}),
        compressed(std::uint8_t{0x10}),
        compressed(std::uint8_t{0xFF}),
        compressed(std::int8_t{-1}),
        compressed(std::uint16_t{0}),
        compressed(std::uint16_t{1}),
        compressed(std::uint16_t{0x0100}),
        compressed(std::uint16_t{0x00F0}),
        compressed(std::int16_t{-1}),
        compressed(std::uint32_t{0}),
        compressed(std::uint32_t{5}),
        compressed(std::uint32_t{0x05000000}),
        compressed(std::uint32_t{0x12340000}),
        compressed(std::int32_t{-1}),
        compressed(std::int32_t{-2}),
        compressed(std::numeric_limits<std::int32_t>::lowest()),
        compressed((std::numeric_limits<std::int32_t>::max)()),
        compressed(std::uint64_t{0}),
        compressed(std::uint64_t{0x1234}),
        compressed(std::uint64_t{0x1234} << 48),
        compressed((std::numeric_limits<std::uint64_t>::max)()),
        compressed(std::numeric_limits<std::int64_t>::lowest()),
        compressed(plain_min),
        compressed(static_cast<ByteEnum>(3)),
        compressed(static_cast<WideEnum>(-1)),
        compressed(true),
        compressed(false),
    });
}

TEST_CASE("BitStream: compressed floats and doubles at their boundaries match RakNet",
          "[interop][bitstream][compressed]")
{
    check_against_raknet({
        compressed(-1.0f),
        compressed(-0.0f),
        compressed(0.0f),
        compressed(0.5f),
        compressed(1.0f),
        compressed(-1.0),
        compressed(0.0),
        compressed(-0.75),
        compressed(1.0 - std::ldexp(1.0, -30)),
        compressed(1.0), // overflows the uint32; both sides wrap to 0
    });
}

TEST_CASE("BitStream: delta writes match RakNet", "[interop][bitstream][delta]")
{
    check_against_raknet({
        delta(std::uint8_t{5}, std::uint8_t{5}, true, false),
        delta(std::uint16_t{0x1234}, std::uint16_t{0}, true, false),
        delta(0.0f, -0.0f, true, false),
        delta(std::int32_t{-1}, std::int32_t{0}, false, false),
        delta(true, false, true, false),
        delta(false, false, true, true),
        delta(true, true, false, true),
        delta(std::uint32_t{0}, std::uint32_t{1}, true, true),
        delta(std::uint32_t{9}, std::uint32_t{9}, true, true),
        delta(std::uint8_t{3}, std::uint8_t{0}, false, true),
        delta(0.5, 0.25, true, true),
    });
}

TEST_CASE("BitStream: half-precision floats at their range ends match RakNet", "[interop][bitstream][float16]")
{
    check_against_raknet({
        float16(-1.0f, -1.0f, 1.0f),
        float16(0.0f, -1.0f, 1.0f),
        float16(1.0f, -1.0f, 1.0f),
        float16(10.0f, 0.0f, 100.0f),
        float16(0.0f, 0.0f, 100.0f),
        float16(100.0f, 0.0f, 100.0f),
        float16(-273.15f, -500.0f, 500.0f),
    });
}

TEST_CASE("BitStream: range-packed integers at their range ends match RakNet", "[interop][bitstream][ranged]")
{
    check_against_raknet({
        ranged<std::uint16_t>(300, 0, 1000, false),
        ranged<std::uint16_t>(0, 0, 1000, false),
        ranged<std::uint16_t>(1000, 0, 1000, false),
        ranged<std::int8_t>(-1, -2, 5, false),
        ranged<std::int8_t>(-128, -128, 127, false),
        ranged<std::int32_t>(0, INT32_MIN, INT32_MAX, false),
        ranged<std::int32_t>(-1, INT32_MIN, INT32_MAX, false),
        ranged<std::uint64_t>(UINT64_MAX, 0, UINT64_MAX, false),
        ranged<std::int64_t>(INT64_MIN, INT64_MIN, INT64_MAX, true),
        ranged<std::uint32_t>(1023, 512, 1023, false),
        ranged<std::int16_t>(-7, -7, -7, false),
        ranged<std::uint8_t>(4, 4, 4, true),
        ranged<std::uint8_t>(5, 0, 10, true),
        ranged<std::uint8_t>(200, 0, 10, true),
        ranged<std::uint16_t>(2000, 0, 1000, true),
        ranged<std::int32_t>(-1, 0, 100, true),
    });
}

TEST_CASE("BitStream: every compressed and packed encoding matches RakNet at every bit offset",
          "[interop][bitstream][compressed]")
{
    const auto offset = static_cast<std::size_t>(GENERATE(range(0, 8)));
    const std::vector<StepGenerator> generators =
        concat(concat(compressed_generators(), delta_generators()), packed_generators());
    const std::size_t kind = GENERATE_COPY(range(std::size_t{0}, generators.size()));
    Rng rng(offset * 977 + kind);
    for (int round = 0; round < 16; ++round)
    {
        std::vector<Step> steps;
        for (std::size_t i = 0; i < offset; ++i)
            steps.push_back(scalar(i % 2 == 0));
        steps.push_back(generators[kind](rng));
        steps.push_back(scalar(std::uint16_t{0xA55A})); // whatever follows must line up too
        INFO("offset " << offset << ", " << steps[offset].what);
        check_against_raknet(steps);
    }
}

TEST_CASE("BitStream: randomized sequences mixing compressed and packed encodings with scalars match RakNet",
          "[interop][bitstream][compressed]")
{
    const std::uint32_t seed = Catch::getSeed();
    INFO("seed " << seed << " (rerun with --rng-seed " << seed << ")");
    Rng rng(seed);
    const std::vector<StepGenerator> generators = concat(
        concat(concat(compressed_generators(), delta_generators()), packed_generators()), scalar_generators());
    for (int sequence = 0; sequence < 200; ++sequence)
    {
        INFO("sequence " << sequence);
        check_against_raknet(random_sequence(rng, generators, random_size(rng, 1, 60)));
    }
}
