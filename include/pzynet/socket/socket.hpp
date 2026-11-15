#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/core/span.hpp"

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace pzynet
{

#if defined(_WIN32)
using NativeSocket = std::uintptr_t; // SOCKET
#else
using NativeSocket = int;
#endif
constexpr NativeSocket invalid_socket = static_cast<NativeSocket>(-1);

/// Move-only owner of an operating-system socket; base of UdpSocket, TcpStream and TcpListener.
///
/// Error reporting convention for the whole socket layer: every fallible operation has two overloads.
/// The one taking `std::error_code&` never throws; the other throws std::system_error.
/// A non-blocking operation that cannot complete reports std::errc::operation_would_block.
class Socket
{
public:
    Socket() noexcept = default;
    Socket(Socket&& other) noexcept : handle_(other.handle_), family_(other.family_)
    {
        other.handle_ = invalid_socket;
    }
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            handle_ = other.handle_;
            family_ = other.family_;
            other.handle_ = invalid_socket;
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    bool is_open() const noexcept { return handle_ != invalid_socket; }
    NativeSocket native_handle() const noexcept { return handle_; }
    /// Address family the socket was opened with.
    IpAddress::Family family() const noexcept { return family_; }
    void close() noexcept;

    Endpoint local_endpoint(std::error_code& ec) const noexcept;
    Endpoint local_endpoint() const
    {
        std::error_code ec;
        Endpoint ep = local_endpoint(ec);
        detail::throw_if(ec, "local_endpoint");
        return ep;
    }

    void set_non_blocking(bool enabled, std::error_code& ec) noexcept;
    void set_non_blocking(bool enabled)
    {
        std::error_code ec;
        set_non_blocking(enabled, ec);
        detail::throw_if(ec, "set_non_blocking");
    }

    void set_reuse_address(bool enabled, std::error_code& ec) noexcept;
    void set_reuse_address(bool enabled)
    {
        std::error_code ec;
        set_reuse_address(enabled, ec);
        detail::throw_if(ec, "set_reuse_address");
    }

    void set_send_buffer_size(std::size_t bytes, std::error_code& ec) noexcept;
    void set_send_buffer_size(std::size_t bytes)
    {
        std::error_code ec;
        set_send_buffer_size(bytes, ec);
        detail::throw_if(ec, "set_send_buffer_size");
    }

    void set_receive_buffer_size(std::size_t bytes, std::error_code& ec) noexcept;
    void set_receive_buffer_size(std::size_t bytes)
    {
        std::error_code ec;
        set_receive_buffer_size(bytes, ec);
        detail::throw_if(ec, "set_receive_buffer_size");
    }

    /// IPv6 sockets only, before bind: `false` makes the socket dual-stack, so it also carries IPv4
    /// (IPv4 peers then appear as plain IPv4 endpoints, and IPv4 endpoints can be sent to directly).
    void set_v6_only(bool enabled, std::error_code& ec) noexcept;
    void set_v6_only(bool enabled)
    {
        std::error_code ec;
        set_v6_only(enabled, ec);
        detail::throw_if(ec, "set_v6_only");
    }

protected:
    enum class Kind
    {
        datagram,
        stream
    };

    void open_native(IpAddress::Family family, Kind kind, std::error_code& ec) noexcept;
    void bind_native(const Endpoint& local, std::error_code& ec) noexcept;
    void adopt(NativeSocket handle, IpAddress::Family family) noexcept
    {
        close();
        handle_ = handle;
        family_ = family;
    }

    NativeSocket handle_ = invalid_socket;
    IpAddress::Family family_ = IpAddress::Family::v4;
};

} // namespace pzynet
