// Byte-for-byte comparison of pzyNet's BitStream with RakNet's own BitStream, in both directions.
// Built twice: against stock RakNet (big-endian) and against a RakNet BitStream compiled with
// __BITSTREAM_NATIVE_END (little-endian); see bit_stream_oracle.hpp.

#include "bit_stream_oracle.hpp"

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <limits>

using namespace oracle;

TEST_CASE("BitStream oracle: RakNet uses the byte order the comparison assumes", "[interop][bitstream]")
{
    RakNet::BitStream theirs;
    theirs.Write(std::uint16_t{0x0102});
    const Bytes expected = byte_order == rn::ByteOrder::big_endian ? Bytes{0x01, 0x02} : Bytes{0x02, 0x01};
    CHECK(bytes_of(theirs) == expected);
}

TEST_CASE("BitStream: extreme scalar values match RakNet", "[interop][bitstream]")
{
    const double inf = std::numeric_limits<double>::infinity();
    check_against_raknet({
        scalar(true),
        scalar(std::numeric_limits<signed char>::lowest()),
        scalar((std::numeric_limits<unsigned char>::max)()),
        scalar(std::numeric_limits<short>::lowest()),
        scalar((std::numeric_limits<unsigned short>::max)()),
        scalar(std::numeric_limits<int>::lowest()),
        scalar((std::numeric_limits<unsigned int>::max)()),
        scalar(std::numeric_limits<long>::lowest()),
        scalar(std::numeric_limits<long long>::lowest()),
        scalar((std::numeric_limits<unsigned long long>::max)()),
        scalar(plain_min),
        scalar(plain_max),
        scalar(static_cast<WideEnum>(-1)),
        scalar(static_cast<ByteEnum>(0x80)),
        scalar(false),
        scalar(-0.0f),
        scalar(std::numeric_limits<float>::denorm_min()),
        scalar(-inf),
        scalar((std::numeric_limits<double>::max)()),
        scalar(static_cast<wchar_t>(0x2603)),
    });
}

TEST_CASE("BitStream: raw bits, alignment, aligned bytes and skipping match RakNet at every bit offset",
          "[interop][bitstream]")
{
    const auto offset = static_cast<std::size_t>(GENERATE(range(0, 8)));
    const std::vector<StepGenerator> generators = raw_generators();
    const std::size_t kind = GENERATE_COPY(range(std::size_t{0}, generators.size()));
    Rng rng(offset * 131 + kind);
    for (int round = 0; round < 8; ++round)
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

TEST_CASE("BitStream: randomized mixed sequences match RakNet in both directions", "[interop][bitstream]")
{
    const std::uint32_t seed = Catch::getSeed();
    INFO("seed " << seed << " (rerun with --rng-seed " << seed << ")");
    Rng rng(seed);
    const std::vector<StepGenerator> generators = concat(scalar_generators(), raw_generators());
    for (int sequence = 0; sequence < 200; ++sequence)
    {
        INFO("sequence " << sequence);
        check_against_raknet(random_sequence(rng, generators, random_size(rng, 1, 60)));
    }
}
