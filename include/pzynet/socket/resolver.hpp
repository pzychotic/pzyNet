#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/error.hpp"

#include <cstdint>
#include <string_view>
#include <system_error>
#include <vector>

namespace pzynet
{

enum class ResolveFamily
{
    any,
    v4,
    v6
};

/// Resolves a host name (or address literal) with the system resolver. Blocks.
/// An unknown host reports Errc::host_not_found and yields an empty list.
std::vector<Endpoint> resolve(std::string_view host, std::uint16_t port, ResolveFamily family,
                              std::error_code& ec);

inline std::vector<Endpoint> resolve(std::string_view host, std::uint16_t port,
                                     ResolveFamily family = ResolveFamily::any)
{
    std::error_code ec;
    std::vector<Endpoint> endpoints = resolve(host, port, family, ec);
    detail::throw_if(ec, "resolve");
    return endpoints;
}

/// This system's own IPv4 addresses: what the local host name resolves to, as RakNet's GetMyIP finds
/// them (gethostbyname(gethostname())). Blocks.
std::vector<IpAddress> host_addresses(std::error_code& ec);

inline std::vector<IpAddress> host_addresses()
{
    std::error_code ec;
    std::vector<IpAddress> addresses = host_addresses(ec);
    detail::throw_if(ec, "host_addresses");
    return addresses;
}

} // namespace pzynet
