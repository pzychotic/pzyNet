#pragma once

// Internal: the one place that includes operating-system socket headers.

#include "pzynet/core/address.hpp"
#include "pzynet/socket/socket.hpp"

#include <system_error>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pzynet
{
namespace detail
{

#if defined(_WIN32)
using socklen_type = int;
using native_type = SOCKET;
inline int last_error_code() noexcept { return ::WSAGetLastError(); }
#else
using socklen_type = socklen_t;
using native_type = int;
inline int last_error_code() noexcept { return errno; }
#endif

/// Starts Winsock on first use (no-op elsewhere).
void ensure_initialized() noexcept;

/// Wraps a native error code; "would block" is normalised to std::errc::operation_would_block.
std::error_code socket_error(int code) noexcept;
inline std::error_code last_socket_error() noexcept { return socket_error(last_error_code()); }

socklen_type to_sockaddr(const Endpoint& endpoint, sockaddr_storage& storage) noexcept;
Endpoint from_sockaddr(const sockaddr_storage& storage) noexcept;

/// Converts `endpoint` for use on a socket of `family`: IPv4 endpoints are v4-mapped for IPv6
/// (dual-stack) sockets, v4-mapped endpoints are unmapped for IPv4 sockets.
bool adapt_endpoint(IpAddress::Family family, const Endpoint& endpoint, Endpoint& out) noexcept;

inline native_type native(NativeSocket handle) noexcept { return static_cast<native_type>(handle); }

} // namespace detail
} // namespace pzynet
