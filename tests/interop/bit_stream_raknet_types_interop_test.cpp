// RakNet's own types and strings in pzyNet's BitStream, compared byte for byte with RakNet's
// BitStream in both directions (stock and __BITSTREAM_NATIVE_END builds); see
// bit_stream_oracle_raknet_types.hpp for where the fork differs from upstream.

#include "bit_stream_oracle_raknet_types.hpp"

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

using namespace oracle;

TEST_CASE("BitStream: RakNet types and strings at their extremes match RakNet", "[interop][bitstream]")
{
    check_against_raknet({
        uint24(0),
        scalar(true),
        uint24(0xFFFFFF),
        guid(0),
        guid(~std::uint64_t{0}),
        scalar(true),
        guid(0x0102030405060708u),
        ipv4_address({0, 0, 0, 0}, 0),
        ipv4_address({255, 255, 255, 255}, 65535),
        scalar(false),
        ipv4_address({192, 168, 1, 2}, 0x1234),
        string(""),
        scalar(true),
        string(""),
        string("hi"),
        scalar(true),
        string(std::string(300, '\xFF')),
        wide_string(u""),
        scalar(true),
        wide_string(u"hi☃\xFFFF"),
    });
}

TEST_CASE("BitStream: RakNet types and strings match RakNet at every bit offset", "[interop][bitstream]")
{
    const auto offset = static_cast<std::size_t>(GENERATE(range(0, 8)));
    const std::vector<StepGenerator> generators = raknet_type_generators();
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

TEST_CASE("BitStream: randomized sequences with RakNet types and strings match RakNet in both directions",
          "[interop][bitstream]")
{
    const std::uint32_t seed = Catch::getSeed();
    INFO("seed " << seed << " (rerun with --rng-seed " << seed << ")");
    Rng rng(seed);
    const std::vector<StepGenerator> generators =
        concat(concat(scalar_generators(), raw_generators()), raknet_type_generators());
    for (int sequence = 0; sequence < 200; ++sequence)
    {
        INFO("sequence " << sequence);
        check_against_raknet(random_sequence(rng, generators, random_size(rng, 1, 60)));
    }
}
