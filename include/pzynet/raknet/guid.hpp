#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace pzynet
{
namespace raknet
{

/// The 64-bit identifier a RakNet system chooses for itself (RakNetGUID).
struct Guid
{
    std::uint64_t value = 0;

    friend constexpr bool operator==(Guid a, Guid b) noexcept { return a.value == b.value; }
    friend constexpr bool operator!=(Guid a, Guid b) noexcept { return a.value != b.value; }
    friend constexpr bool operator<(Guid a, Guid b) noexcept { return a.value < b.value; }
};

} // namespace raknet
} // namespace pzynet

template <>
struct std::hash<pzynet::raknet::Guid>
{
    std::size_t operator()(pzynet::raknet::Guid guid) const noexcept
    {
        return std::hash<std::uint64_t>()(guid.value);
    }
};
