#pragma once

#include "pzynet/socket/socket.hpp"

namespace pzynet
{

enum class Shutdown
{
    receive,
    send,
    both
};

/// A connected (or connecting) TCP socket.
class TcpStream : public Socket
{
public:
    TcpStream() noexcept = default;

    /// Creates the socket without connecting (e.g. to make it non-blocking first).
    void open(IpAddress::Family family, std::error_code& ec) noexcept;
    void open(IpAddress::Family family)
    {
        std::error_code ec;
        open(family, ec);
        detail::throw_if(ec, "TcpStream::open");
    }

    /// Connects to `remote`; opens the socket first if necessary. On a non-blocking socket this reports
    /// std::errc::operation_in_progress: wait until the socket is writable, then call finish_connect().
    void connect(const Endpoint& remote, std::error_code& ec) noexcept;
    void connect(const Endpoint& remote)
    {
        std::error_code ec;
        connect(remote, ec);
        detail::throw_if(ec, "TcpStream::connect");
    }

    /// Reports the outcome of a non-blocking connect once the socket became writable.
    void finish_connect(std::error_code& ec) noexcept;
    void finish_connect()
    {
        std::error_code ec;
        finish_connect(ec);
        detail::throw_if(ec, "TcpStream::finish_connect");
    }

    /// Sends as much of `data` as the socket accepts right now. Returns the number of bytes taken.
    std::size_t send(ByteView data, std::error_code& ec) noexcept;
    std::size_t send(ByteView data)
    {
        std::error_code ec;
        const std::size_t n = send(data, ec);
        detail::throw_if(ec, "TcpStream::send");
        return n;
    }

    /// Blocking sockets: loops until all of `data` is sent or an error occurs.
    void send_all(ByteView data, std::error_code& ec) noexcept;
    void send_all(ByteView data)
    {
        std::error_code ec;
        send_all(data, ec);
        detail::throw_if(ec, "TcpStream::send_all");
    }

    /// Returns the number of bytes read. 0 with a clear `ec` means the peer closed its sending side.
    std::size_t receive(MutableByteView buffer, std::error_code& ec) noexcept;
    std::size_t receive(MutableByteView buffer)
    {
        std::error_code ec;
        const std::size_t n = receive(buffer, ec);
        detail::throw_if(ec, "TcpStream::receive");
        return n;
    }

    void shutdown(Shutdown how, std::error_code& ec) noexcept;
    void shutdown(Shutdown how)
    {
        std::error_code ec;
        shutdown(how, ec);
        detail::throw_if(ec, "TcpStream::shutdown");
    }

    Endpoint remote_endpoint(std::error_code& ec) const noexcept;
    Endpoint remote_endpoint() const
    {
        std::error_code ec;
        Endpoint ep = remote_endpoint(ec);
        detail::throw_if(ec, "TcpStream::remote_endpoint");
        return ep;
    }

    /// Disables Nagle's algorithm when enabled.
    void set_no_delay(bool enabled, std::error_code& ec) noexcept;
    void set_no_delay(bool enabled)
    {
        std::error_code ec;
        set_no_delay(enabled, ec);
        detail::throw_if(ec, "set_no_delay");
    }

    void set_keep_alive(bool enabled, std::error_code& ec) noexcept;
    void set_keep_alive(bool enabled)
    {
        std::error_code ec;
        set_keep_alive(enabled, ec);
        detail::throw_if(ec, "set_keep_alive");
    }

private:
    friend class TcpListener;
};

/// A listening TCP socket.
class TcpListener : public Socket
{
public:
    static constexpr int default_backlog = 128;

    TcpListener() noexcept = default;

    /// Opens, binds and listens in one step. Port 0 = ephemeral (see local_endpoint()).
    void listen(const Endpoint& local, int backlog, std::error_code& ec) noexcept;
    void listen(const Endpoint& local, std::error_code& ec) noexcept { listen(local, default_backlog, ec); }
    void listen(const Endpoint& local, int backlog = default_backlog)
    {
        std::error_code ec;
        listen(local, backlog, ec);
        detail::throw_if(ec, "TcpListener::listen");
    }

    /// Accepts one pending connection. On a non-blocking listener with nothing pending this reports
    /// std::errc::operation_would_block and returns a closed stream.
    TcpStream accept(Endpoint& peer, std::error_code& ec) noexcept;
    TcpStream accept(std::error_code& ec) noexcept
    {
        Endpoint peer;
        return accept(peer, ec);
    }
    TcpStream accept(Endpoint& peer)
    {
        std::error_code ec;
        TcpStream stream = accept(peer, ec);
        detail::throw_if(ec, "TcpListener::accept");
        return stream;
    }
    TcpStream accept()
    {
        Endpoint peer;
        return accept(peer);
    }
};

} // namespace pzynet
