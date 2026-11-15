#pragma once

#include "pzynet/socket/socket.hpp"

namespace pzynet
{

/// A UDP socket.
class UdpSocket : public Socket
{
public:
    UdpSocket() noexcept = default;

    /// Creates the socket without binding it (e.g. to set options first).
    void open(IpAddress::Family family, std::error_code& ec) noexcept;
    void open(IpAddress::Family family)
    {
        std::error_code ec;
        open(family, ec);
        detail::throw_if(ec, "UdpSocket::open");
    }

    /// Binds to `local`; opens the socket first (in the endpoint's family) if necessary. Port 0 = ephemeral.
    void bind(const Endpoint& local, std::error_code& ec) noexcept;
    void bind(const Endpoint& local)
    {
        std::error_code ec;
        bind(local, ec);
        detail::throw_if(ec, "UdpSocket::bind");
    }

    /// Sends one datagram. Returns the number of bytes sent.
    std::size_t send_to(ByteView data, const Endpoint& to, std::error_code& ec) noexcept;
    std::size_t send_to(ByteView data, const Endpoint& to)
    {
        std::error_code ec;
        const std::size_t n = send_to(data, to, ec);
        detail::throw_if(ec, "UdpSocket::send_to");
        return n;
    }

    /// Receives one datagram into `buffer` and reports its sender. A datagram larger than the buffer is
    /// truncated. Returns the number of bytes stored (0 is a valid, empty datagram when `ec` is clear).
    std::size_t receive_from(MutableByteView buffer, Endpoint& from, std::error_code& ec) noexcept;
    std::size_t receive_from(MutableByteView buffer, Endpoint& from)
    {
        std::error_code ec;
        const std::size_t n = receive_from(buffer, from, ec);
        detail::throw_if(ec, "UdpSocket::receive_from");
        return n;
    }

    void set_broadcast(bool enabled, std::error_code& ec) noexcept;
    void set_broadcast(bool enabled)
    {
        std::error_code ec;
        set_broadcast(enabled, ec);
        detail::throw_if(ec, "set_broadcast");
    }

    /// Sets the IP "don't fragment" behaviour for outgoing datagrams (used for path-MTU probing).
    void set_dont_fragment(bool enabled, std::error_code& ec) noexcept;
    void set_dont_fragment(bool enabled)
    {
        std::error_code ec;
        set_dont_fragment(enabled, ec);
        detail::throw_if(ec, "set_dont_fragment");
    }

    /// Joins a multicast group. IPv4: `local_interface` selects the interface by address (unspecified =
    /// default). IPv6: the group's scope id selects the interface by index (0 = default).
    void join_multicast_group(const IpAddress& group, const IpAddress& local_interface,
                              std::error_code& ec) noexcept;
    void join_multicast_group(const IpAddress& group, const IpAddress& local_interface = IpAddress())
    {
        std::error_code ec;
        join_multicast_group(group, local_interface, ec);
        detail::throw_if(ec, "join_multicast_group");
    }

    void leave_multicast_group(const IpAddress& group, const IpAddress& local_interface,
                               std::error_code& ec) noexcept;
    void leave_multicast_group(const IpAddress& group, const IpAddress& local_interface = IpAddress())
    {
        std::error_code ec;
        leave_multicast_group(group, local_interface, ec);
        detail::throw_if(ec, "leave_multicast_group");
    }

    /// Hop limit for outgoing multicast datagrams.
    void set_multicast_ttl(unsigned ttl, std::error_code& ec) noexcept;
    void set_multicast_ttl(unsigned ttl)
    {
        std::error_code ec;
        set_multicast_ttl(ttl, ec);
        detail::throw_if(ec, "set_multicast_ttl");
    }

    /// Whether multicast datagrams sent from this socket are looped back to local receivers.
    void set_multicast_loopback(bool enabled, std::error_code& ec) noexcept;
    void set_multicast_loopback(bool enabled)
    {
        std::error_code ec;
        set_multicast_loopback(enabled, ec);
        detail::throw_if(ec, "set_multicast_loopback");
    }

private:
    void change_membership(bool join, const IpAddress& group, const IpAddress& local_interface,
                           std::error_code& ec) noexcept;
};

} // namespace pzynet
