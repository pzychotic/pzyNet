#pragma once

#include <cstdint>

namespace pzynet
{

/// Arithmetic on 24-bit wrapping sequence numbers.
namespace seq24
{
constexpr std::uint32_t mask = 0x00FFFFFFu;
constexpr std::uint32_t half = 0x00800000u;

constexpr std::uint32_t wrap(std::uint64_t v) noexcept
{
    return static_cast<std::uint32_t>(v) & mask;
}
/// Forward distance from `from` to `to`, in [0, 2^24).
constexpr std::uint32_t distance(std::uint32_t from, std::uint32_t to) noexcept
{
    return (to - from) & mask;
}
/// True if `a` precedes `b` (b is less than half the number space ahead of a).
constexpr bool less(std::uint32_t a, std::uint32_t b) noexcept
{
    const std::uint32_t d = distance(a, b);
    return d != 0 && d < half;
}
} // namespace seq24

} // namespace pzynet
