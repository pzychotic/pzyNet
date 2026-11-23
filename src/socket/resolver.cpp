#include "platform.hpp"

#include "pzynet/socket/resolver.hpp"

#include <algorithm>
#include <string>

namespace pzynet
{

std::vector<Endpoint> resolve(std::string_view host, std::uint16_t port, ResolveFamily family,
                              std::error_code& ec)
{
    detail::ensure_initialized();
    ec.clear();

    addrinfo hints{};
    hints.ai_family = family == ResolveFamily::v4 ? AF_INET : family == ResolveFamily::v6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM; // one result per address instead of one per socket type

    const std::string node(host);
    addrinfo* results = nullptr;
    const int status = ::getaddrinfo(node.c_str(), nullptr, &hints, &results);
    if (status != 0)
    {
#if defined(_WIN32)
        ec = status == WSAHOST_NOT_FOUND || status == WSANO_DATA ? make_error_code(Errc::host_not_found)
                                                                 : detail::socket_error(status);
#else
        ec = status == EAI_SYSTEM ? detail::last_socket_error() : make_error_code(Errc::host_not_found);
#endif
        return {};
    }

    std::vector<Endpoint> endpoints;
    for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next)
    {
        if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
            continue;
        sockaddr_storage storage{};
        std::copy_n(reinterpret_cast<const char*>(entry->ai_addr),
                    std::min<std::size_t>(static_cast<std::size_t>(entry->ai_addrlen), sizeof storage),
                    reinterpret_cast<char*>(&storage));
        const Endpoint resolved(detail::from_sockaddr(storage).address(), port);
        if (std::find(endpoints.begin(), endpoints.end(), resolved) == endpoints.end())
            endpoints.push_back(resolved);
    }
    ::freeaddrinfo(results);

    if (endpoints.empty())
        ec = make_error_code(Errc::host_not_found);
    return endpoints;
}

std::vector<IpAddress> host_addresses(std::error_code& ec)
{
    detail::ensure_initialized();
    ec.clear();
    char name[256] = {};
    if (::gethostname(name, sizeof name - 1) != 0)
    {
        ec = detail::last_socket_error();
        return {};
    }
    std::vector<IpAddress> addresses;
    for (const Endpoint& endpoint : resolve(name, 0, ResolveFamily::v4, ec))
        addresses.push_back(endpoint.address());
    return addresses;
}

} // namespace pzynet
