// Byte-for-byte comparison of pzyNet's BitStream game-math helpers with RakNet's own BitStream, in
// both directions. Built against stock RakNet and against a __BITSTREAM_NATIVE_END build; see
// bit_stream_oracle.hpp.

#include "bit_stream_geometry_oracle.hpp"

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

using namespace oracle;

TEST_CASE("BitStream: game-math helpers match RakNet at boundary values", "[interop][bitstream]")
{
    using V = rn::Vector3<float>;
    using Q = rn::Quaternion<float>;
    using M = rn::Matrix3<float>;
    check_against_raknet({
        normalized_vector(V{0.0f, 1.0f, -1.0f}),
        normalized_vector(V{-0.0f, 0.5f, 0.999999f}),
        normalized_vector(rn::Vector3<double>{1.0, -1.0, 1e-9}),
        vector(V{0.0f, 0.0f, 0.0f}),
        vector(V{0.0f, 0.0f, -1e-6f}),
        vector(rn::Vector3<double>{3.0, 0.0, -4.0}),
        vector(V{1e4f, -1e4f, 1e4f}),
        vector(V{0.0f, 2.0f, 0.0f}),
        normalized_quaternion(Q{1.0f, 0.0f, 0.0f, 0.0f}),
        normalized_quaternion(Q{-1.0f, 0.0f, 0.0f, 0.0f}),
        normalized_quaternion(Q{0.0f, 1.0f, 0.0f, 0.0f}),
        normalized_quaternion(Q{0.0f, 0.0f, -1.0f, 0.0f}),
        normalized_quaternion(rn::Quaternion<double>{-0.5, 0.5, -0.5, 0.5}),
        orthogonal_matrix(M{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}}),
        orthogonal_matrix(M{{{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}}),
        orthogonal_matrix(rn::Matrix3<double>{{{0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.0}}}),
        orthogonal_matrix(M{{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}}}),
    });
}

TEST_CASE("BitStream: game-math helpers match RakNet at every bit offset", "[interop][bitstream]")
{
    const auto offset = static_cast<std::size_t>(GENERATE(range(0, 8)));
    const std::vector<StepGenerator> generators = geometry_generators();
    const std::size_t kind = GENERATE_COPY(range(std::size_t{0}, generators.size()));
    Rng rng(offset * 131 + kind);
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

TEST_CASE("BitStream: randomized game-math sequences match RakNet in both directions", "[interop][bitstream]")
{
    const std::uint32_t seed = Catch::getSeed();
    INFO("seed " << seed << " (rerun with --rng-seed " << seed << ")");
    Rng rng(seed);
    const std::vector<StepGenerator> generators = concat(geometry_generators(), scalar_generators());
    for (int sequence = 0; sequence < 200; ++sequence)
    {
        INFO("sequence " << sequence);
        check_against_raknet(random_sequence(rng, generators, random_size(rng, 1, 40)));
    }
}
