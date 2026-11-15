#include "platform.hpp"

#include "pzynet/socket/tcp.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <climits>
#include <cstring>

namespace pzynet
{
namespace detail
{

void ensure_initialized() noexcept
{
#if defined(_WIN32)
    struct Winsock
    {
        Winsock() noexcept
        {
            WSADATA data;
            ::WSAStartup(MAKEWORD(2, 2), &data);
        }
        ~Winsock() { ::WSACleanup(); }
    };
    static const Winsock winsock;
#endif
}

std::error_code socket_error(int code) noexcept
{
#if defined(_WIN32)
    if (code == WSAEWOULDBLOCK)
        return std::make_error_code(std::errc::operation_would_block);
#else
    if (code == EAGAIN || code == EWOULDBLOCK)
        return std::make_error_code(std::errc::operation_would_block);
#endif
    return std::error_code(code, std::system_category());
}

socklen_type to_sockaddr(const Endpoint& endpoint, sockaddr_storage& storage) noexcept
{
    std::memset(&storage, 0, sizeof storage);
    if (endpoint.address().is_v4())
    {
        auto* in = reinterpret_cast<sockaddr_in*>(&storage);
        in->sin_family = AF_INET;
        in->sin_port = htons(endpoint.port());
        const auto bytes = endpoint.address().v4_bytes();
        std::memcpy(&in->sin_addr, bytes.data(), bytes.size());
        return static_cast<socklen_type>(sizeof(sockaddr_in));
    }
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&storage);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(endpoint.port());
    const auto& bytes = endpoint.address().v6_bytes();
    std::memcpy(&in6->sin6_addr, bytes.data(), bytes.size());
    in6->sin6_scope_id = endpoint.address().scope_id();
    return static_cast<socklen_type>(sizeof(sockaddr_in6));
}

Endpoint from_sockaddr(const sockaddr_storage& storage) noexcept
{
    if (storage.ss_family == AF_INET)
    {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&storage);
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &in->sin_addr, bytes.size());
        return Endpoint(IpAddress::v4(bytes), ntohs(in->sin_port));
    }
    if (storage.ss_family == AF_INET6)
    {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &in6->sin6_addr, bytes.size());
        return Endpoint(IpAddress::v6(bytes, static_cast<std::uint32_t>(in6->sin6_scope_id)),
                        ntohs(in6->sin6_port));
    }
    return Endpoint();
}

bool adapt_endpoint(IpAddress::Family family, const Endpoint& endpoint, Endpoint& out) noexcept
{
    if (family == IpAddress::Family::v6)
    {
        out = Endpoint(endpoint.address().to_v4_mapped(), endpoint.port());
        return true;
    }
    out = Endpoint(endpoint.address().unmapped(), endpoint.port());
    return out.address().is_v4();
}

} // namespace detail

namespace
{

using detail::last_socket_error;
using detail::native;
using detail::socklen_type;

#if defined(_WIN32)
using io_length = int;
using io_result = int;
constexpr int send_flags = 0;
#else
using io_length = std::size_t;
using io_result = ssize_t;
constexpr int send_flags = MSG_NOSIGNAL; // report EPIPE instead of raising SIGPIPE
#endif

io_length clamp_length(std::size_t size) noexcept
{
    return static_cast<io_length>(size > static_cast<std::size_t>(INT_MAX) ? static_cast<std::size_t>(INT_MAX) : size);
}

template <class T>
void set_option(NativeSocket handle, int level, int name, const T& value, std::error_code& ec) noexcept
{
    if (handle == invalid_socket)
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    const int result = ::setsockopt(native(handle), level, name, reinterpret_cast<const char*>(&value),
                                    static_cast<socklen_type>(sizeof value));
    ec = result == 0 ? std::error_code() : last_socket_error();
}

void set_bool_option(NativeSocket handle, int level, int name, bool enabled, std::error_code& ec) noexcept
{
    const int value = enabled ? 1 : 0;
    set_option(handle, level, name, value, ec);
}

Endpoint query_name(NativeSocket handle, bool peer, std::error_code& ec) noexcept
{
    if (handle == invalid_socket)
    {
        ec = make_error_code(Errc::not_open);
        return Endpoint();
    }
    sockaddr_storage storage{};
    socklen_type length = static_cast<socklen_type>(sizeof storage);
    auto* address = reinterpret_cast<sockaddr*>(&storage);
    const int result = peer ? ::getpeername(native(handle), address, &length)
                            : ::getsockname(native(handle), address, &length);
    if (result != 0)
    {
        ec = last_socket_error();
        return Endpoint();
    }
    ec.clear();
    return detail::from_sockaddr(storage);
}

} // namespace

// --- Socket ------------------------------------------------------------------------------------------

void Socket::close() noexcept
{
    if (handle_ == invalid_socket)
        return;
#if defined(_WIN32)
    ::closesocket(native(handle_));
#else
    ::close(handle_);
#endif
    handle_ = invalid_socket;
}

void Socket::open_native(IpAddress::Family family, Kind kind, std::error_code& ec) noexcept
{
    if (is_open())
    {
        ec = make_error_code(Errc::already_open);
        return;
    }
    detail::ensure_initialized();

    const int domain = family == IpAddress::Family::v4 ? AF_INET : AF_INET6;
    int type = kind == Kind::datagram ? SOCK_DGRAM : SOCK_STREAM;
    const int protocol = kind == Kind::datagram ? IPPROTO_UDP : IPPROTO_TCP;
#if defined(SOCK_CLOEXEC)
    type |= SOCK_CLOEXEC;
#endif
    const detail::native_type handle = ::socket(domain, type, protocol);
#if defined(_WIN32)
    if (handle == INVALID_SOCKET)
#else
    if (handle < 0)
#endif
    {
        ec = last_socket_error();
        return;
    }
    handle_ = static_cast<NativeSocket>(handle);
    family_ = family;
    ec.clear();

#if defined(_WIN32)
    if (kind == Kind::datagram)
    {
        // Without this, an ICMP "port unreachable" triggered by an earlier send makes a later
        // recvfrom fail with WSAECONNRESET, which is meaningless for a connectionless socket.
        BOOL report_reset = FALSE;
        DWORD returned = 0;
        ::WSAIoctl(handle, SIO_UDP_CONNRESET, &report_reset, sizeof report_reset, nullptr, 0, &returned,
                   nullptr, nullptr);
    }
#endif
}

void Socket::bind_native(const Endpoint& local, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    Endpoint adapted;
    if (!detail::adapt_endpoint(family_, local, adapted))
    {
        ec = make_error_code(Errc::address_family_mismatch);
        return;
    }
    sockaddr_storage storage{};
    const socklen_type length = detail::to_sockaddr(adapted, storage);
    const int result = ::bind(native(handle_), reinterpret_cast<const sockaddr*>(&storage), length);
    ec = result == 0 ? std::error_code() : last_socket_error();
}

Endpoint Socket::local_endpoint(std::error_code& ec) const noexcept
{
    return query_name(handle_, false, ec);
}

void Socket::set_non_blocking(bool enabled, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
#if defined(_WIN32)
    u_long mode = enabled ? 1 : 0;
    const int result = ::ioctlsocket(native(handle_), FIONBIO, &mode);
    ec = result == 0 ? std::error_code() : last_socket_error();
#else
    const int flags = ::fcntl(handle_, F_GETFL, 0);
    if (flags < 0)
    {
        ec = last_socket_error();
        return;
    }
    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ec = ::fcntl(handle_, F_SETFL, updated) == 0 ? std::error_code() : last_socket_error();
#endif
}

void Socket::set_reuse_address(bool enabled, std::error_code& ec) noexcept
{
    set_bool_option(handle_, SOL_SOCKET, SO_REUSEADDR, enabled, ec);
}

void Socket::set_send_buffer_size(std::size_t bytes, std::error_code& ec) noexcept
{
    set_option(handle_, SOL_SOCKET, SO_SNDBUF, static_cast<int>(clamp_length(bytes)), ec);
}

void Socket::set_receive_buffer_size(std::size_t bytes, std::error_code& ec) noexcept
{
    set_option(handle_, SOL_SOCKET, SO_RCVBUF, static_cast<int>(clamp_length(bytes)), ec);
}

void Socket::set_v6_only(bool enabled, std::error_code& ec) noexcept
{
    if (family_ != IpAddress::Family::v6)
    {
        ec = make_error_code(is_open() ? Errc::address_family_mismatch : Errc::not_open);
        return;
    }
    set_bool_option(handle_, IPPROTO_IPV6, IPV6_V6ONLY, enabled, ec);
}

// --- UdpSocket ---------------------------------------------------------------------------------------

void UdpSocket::open(IpAddress::Family family, std::error_code& ec) noexcept
{
    open_native(family, Kind::datagram, ec);
}

void UdpSocket::bind(const Endpoint& local, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        open(local.address().family(), ec);
        if (ec)
            return;
    }
    bind_native(local, ec);
}

std::size_t UdpSocket::send_to(ByteView data, const Endpoint& to, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return 0;
    }
    Endpoint adapted;
    if (!detail::adapt_endpoint(family_, to, adapted))
    {
        ec = make_error_code(Errc::address_family_mismatch);
        return 0;
    }
    sockaddr_storage storage{};
    const socklen_type length = detail::to_sockaddr(adapted, storage);
    const io_result sent = ::sendto(native(handle_), reinterpret_cast<const char*>(data.data()),
                                    clamp_length(data.size()), send_flags,
                                    reinterpret_cast<const sockaddr*>(&storage), length);
    if (sent < 0)
    {
        ec = last_socket_error();
        return 0;
    }
    ec.clear();
    return static_cast<std::size_t>(sent);
}

std::size_t UdpSocket::receive_from(MutableByteView buffer, Endpoint& from, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return 0;
    }
    sockaddr_storage storage{};
    socklen_type length = static_cast<socklen_type>(sizeof storage);
    const io_result received = ::recvfrom(native(handle_), reinterpret_cast<char*>(buffer.data()),
                                          clamp_length(buffer.size()), 0,
                                          reinterpret_cast<sockaddr*>(&storage), &length);
    if (received < 0)
    {
#if defined(_WIN32)
        if (detail::last_error_code() == WSAEMSGSIZE) // truncated: the buffer was filled completely
        {
            ec.clear();
            const Endpoint sender = detail::from_sockaddr(storage);
            from = Endpoint(sender.address().unmapped(), sender.port());
            return buffer.size();
        }
#endif
        ec = last_socket_error();
        return 0;
    }
    ec.clear();
    const Endpoint sender = detail::from_sockaddr(storage);
    from = Endpoint(sender.address().unmapped(), sender.port());
    return static_cast<std::size_t>(received);
}

void UdpSocket::set_broadcast(bool enabled, std::error_code& ec) noexcept
{
    set_bool_option(handle_, SOL_SOCKET, SO_BROADCAST, enabled, ec);
}

void UdpSocket::set_dont_fragment(bool enabled, std::error_code& ec) noexcept
{
    const bool v6 = family_ == IpAddress::Family::v6;
#if defined(_WIN32)
    const DWORD value = enabled ? 1 : 0;
    if (v6)
    {
        set_option(handle_, IPPROTO_IPV6, IPV6_DONTFRAG, value, ec);
        std::error_code ignored; // also covers IPv4 traffic of a dual-stack socket; fails harmlessly otherwise
        set_option(handle_, IPPROTO_IP, IP_DONTFRAGMENT, value, ignored);
    }
    else
        set_option(handle_, IPPROTO_IP, IP_DONTFRAGMENT, value, ec);
#else
    if (v6)
    {
        const int value6 = enabled ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT;
        set_option(handle_, IPPROTO_IPV6, IPV6_MTU_DISCOVER, value6, ec);
        std::error_code ignored;
        const int value4 = enabled ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        set_option(handle_, IPPROTO_IP, IP_MTU_DISCOVER, value4, ignored);
    }
    else
    {
        const int value = enabled ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        set_option(handle_, IPPROTO_IP, IP_MTU_DISCOVER, value, ec);
    }
#endif
}

void UdpSocket::change_membership(bool join, const IpAddress& group, const IpAddress& local_interface,
                                  std::error_code& ec) noexcept
{
    if (group.is_v4() != (family_ == IpAddress::Family::v4) || !group.is_multicast())
    {
        ec = make_error_code(is_open() ? Errc::invalid_argument : Errc::not_open);
        return;
    }
    if (group.is_v4())
    {
        ip_mreq request{};
        const auto group_bytes = group.v4_bytes();
        const auto interface_bytes = local_interface.is_v4() ? local_interface.v4_bytes()
                                                              : std::array<std::uint8_t, 4>{};
        std::memcpy(&request.imr_multiaddr, group_bytes.data(), 4);
        std::memcpy(&request.imr_interface, interface_bytes.data(), 4);
        set_option(handle_, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, request, ec);
    }
    else
    {
        ipv6_mreq request{};
        std::memcpy(&request.ipv6mr_multiaddr, group.v6_bytes().data(), 16);
        request.ipv6mr_interface = group.scope_id();
#if defined(IPV6_JOIN_GROUP)
        set_option(handle_, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, request, ec);
#else
        set_option(handle_, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, request, ec);
#endif
    }
}

void UdpSocket::join_multicast_group(const IpAddress& group, const IpAddress& local_interface,
                                     std::error_code& ec) noexcept
{
    change_membership(true, group, local_interface, ec);
}

void UdpSocket::leave_multicast_group(const IpAddress& group, const IpAddress& local_interface,
                                      std::error_code& ec) noexcept
{
    change_membership(false, group, local_interface, ec);
}

void UdpSocket::set_multicast_ttl(unsigned ttl, std::error_code& ec) noexcept
{
    const int value = static_cast<int>(ttl > 255 ? 255 : ttl);
    if (family_ == IpAddress::Family::v6)
        set_option(handle_, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, value, ec);
    else
        set_option(handle_, IPPROTO_IP, IP_MULTICAST_TTL, value, ec);
}

void UdpSocket::set_multicast_loopback(bool enabled, std::error_code& ec) noexcept
{
    if (family_ == IpAddress::Family::v6)
        set_bool_option(handle_, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, enabled, ec);
    else
        set_bool_option(handle_, IPPROTO_IP, IP_MULTICAST_LOOP, enabled, ec);
}

// --- TcpStream ---------------------------------------------------------------------------------------

void TcpStream::open(IpAddress::Family family, std::error_code& ec) noexcept
{
    open_native(family, Kind::stream, ec);
}

void TcpStream::connect(const Endpoint& remote, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        open(remote.address().family(), ec);
        if (ec)
            return;
    }
    Endpoint adapted;
    if (!detail::adapt_endpoint(family_, remote, adapted))
    {
        ec = make_error_code(Errc::address_family_mismatch);
        return;
    }
    sockaddr_storage storage{};
    const socklen_type length = detail::to_sockaddr(adapted, storage);
    if (::connect(native(handle_), reinterpret_cast<const sockaddr*>(&storage), length) == 0)
    {
        ec.clear();
        return;
    }
    const int code = detail::last_error_code();
#if defined(_WIN32)
    const bool in_progress = code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    const bool in_progress = code == EINPROGRESS;
#endif
    ec = in_progress ? std::make_error_code(std::errc::operation_in_progress) : detail::socket_error(code);
}

void TcpStream::finish_connect(std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    int pending = 0;
    socklen_type length = static_cast<socklen_type>(sizeof pending);
    if (::getsockopt(native(handle_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) != 0)
    {
        ec = last_socket_error();
        return;
    }
    if (pending != 0)
    {
        ec = detail::socket_error(pending);
        return;
    }
    // SO_ERROR is also 0 while the connect is still under way; getpeername tells the two apart.
    std::error_code peer_error;
    (void)query_name(handle_, true, peer_error);
    ec = peer_error ? std::make_error_code(std::errc::operation_in_progress) : std::error_code();
}

std::size_t TcpStream::send(ByteView data, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return 0;
    }
    const io_result sent = ::send(native(handle_), reinterpret_cast<const char*>(data.data()),
                                  clamp_length(data.size()), send_flags);
    if (sent < 0)
    {
        ec = last_socket_error();
        return 0;
    }
    ec.clear();
    return static_cast<std::size_t>(sent);
}

void TcpStream::send_all(ByteView data, std::error_code& ec) noexcept
{
    ec.clear();
    while (!data.empty())
    {
        const std::size_t sent = send(data, ec);
        if (ec)
            return;
        data = data.subspan(sent);
    }
}

std::size_t TcpStream::receive(MutableByteView buffer, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return 0;
    }
    const io_result received =
        ::recv(native(handle_), reinterpret_cast<char*>(buffer.data()), clamp_length(buffer.size()), 0);
    if (received < 0)
    {
        ec = last_socket_error();
        return 0;
    }
    ec.clear();
    return static_cast<std::size_t>(received);
}

void TcpStream::shutdown(Shutdown how, std::error_code& ec) noexcept
{
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
#if defined(_WIN32)
    const int mode = how == Shutdown::receive ? SD_RECEIVE : how == Shutdown::send ? SD_SEND : SD_BOTH;
#else
    const int mode = how == Shutdown::receive ? SHUT_RD : how == Shutdown::send ? SHUT_WR : SHUT_RDWR;
#endif
    ec = ::shutdown(native(handle_), mode) == 0 ? std::error_code() : last_socket_error();
}

Endpoint TcpStream::remote_endpoint(std::error_code& ec) const noexcept
{
    const Endpoint peer = query_name(handle_, true, ec);
    return Endpoint(peer.address().unmapped(), peer.port());
}

void TcpStream::set_no_delay(bool enabled, std::error_code& ec) noexcept
{
    set_bool_option(handle_, IPPROTO_TCP, TCP_NODELAY, enabled, ec);
}

void TcpStream::set_keep_alive(bool enabled, std::error_code& ec) noexcept
{
    set_bool_option(handle_, SOL_SOCKET, SO_KEEPALIVE, enabled, ec);
}

// --- TcpListener -------------------------------------------------------------------------------------

void TcpListener::listen(const Endpoint& local, int backlog, std::error_code& ec) noexcept
{
    open_native(local.address().family(), Kind::stream, ec);
    if (ec)
        return;
#if !defined(_WIN32)
    // Lets a restarted server rebind while old connections linger in TIME_WAIT. Not on Windows, where
    // SO_REUSEADDR means something else: it would let another process steal the port.
    std::error_code ignored;
    set_reuse_address(true, ignored);
#endif
    bind_native(local, ec);
    if (!ec && ::listen(native(handle_), backlog) != 0)
        ec = last_socket_error();
    if (ec)
        close();
}

TcpStream TcpListener::accept(Endpoint& peer, std::error_code& ec) noexcept
{
    TcpStream stream;
    if (!is_open())
    {
        ec = make_error_code(Errc::not_open);
        return stream;
    }
    sockaddr_storage storage{};
    socklen_type length = static_cast<socklen_type>(sizeof storage);
    const detail::native_type handle = ::accept(native(handle_), reinterpret_cast<sockaddr*>(&storage), &length);
#if defined(_WIN32)
    if (handle == INVALID_SOCKET)
#else
    if (handle < 0)
#endif
    {
        ec = last_socket_error();
        return stream;
    }
    ec.clear();
    stream.adopt(static_cast<NativeSocket>(handle), family_);
    const Endpoint remote = detail::from_sockaddr(storage);
    peer = Endpoint(remote.address().unmapped(), remote.port());
    return stream;
}

} // namespace pzynet
