#pragma once

#include "pzynet/socket/socket.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace pzynet
{

enum class PollEvents : std::uint8_t
{
    none = 0,
    readable = 1, ///< data (or a pending connection, or end-of-stream) can be read without blocking
    writable = 2, ///< data can be written; also signals completion of a non-blocking connect
    error = 4,    ///< reported even if not requested
    hang_up = 8,  ///< reported even if not requested
};
constexpr PollEvents operator|(PollEvents a, PollEvents b) noexcept
{
    return static_cast<PollEvents>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr PollEvents operator&(PollEvents a, PollEvents b) noexcept
{
    return static_cast<PollEvents>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr bool has(PollEvents set, PollEvents flag) noexcept
{
    return (set & flag) != PollEvents::none;
}

struct PollEvent
{
    NativeSocket socket = invalid_socket;
    PollEvents events = PollEvents::none;
};

/// Waits for readiness on a set of sockets (WSAPoll on Windows, epoll on Linux).
///
/// add/modify/remove/wait must be called from one thread at a time; wake() may be called from any
/// thread and makes a concurrent or the next wait() return early.
class Poller
{
public:
    Poller();
    explicit Poller(std::error_code& ec) noexcept;
    ~Poller();
    Poller(Poller&&) noexcept;
    Poller& operator=(Poller&&) noexcept;

    void add(NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept;
    void add(const Socket& socket, PollEvents interest, std::error_code& ec) noexcept
    {
        add(socket.native_handle(), interest, ec);
    }
    void add(const Socket& socket, PollEvents interest)
    {
        std::error_code ec;
        add(socket.native_handle(), interest, ec);
        detail::throw_if(ec, "Poller::add");
    }

    void modify(NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept;
    void modify(const Socket& socket, PollEvents interest, std::error_code& ec) noexcept
    {
        modify(socket.native_handle(), interest, ec);
    }
    void modify(const Socket& socket, PollEvents interest)
    {
        std::error_code ec;
        modify(socket.native_handle(), interest, ec);
        detail::throw_if(ec, "Poller::modify");
    }

    /// Must be called before the socket is closed.
    void remove(NativeSocket socket, std::error_code& ec) noexcept;
    void remove(const Socket& socket, std::error_code& ec) noexcept { remove(socket.native_handle(), ec); }
    void remove(const Socket& socket)
    {
        std::error_code ec;
        remove(socket.native_handle(), ec);
        detail::throw_if(ec, "Poller::remove");
    }

    /// Blocks until a socket is ready, `timeout` elapses, or wake() is called. A negative timeout waits
    /// indefinitely. `out` is overwritten with the ready sockets; returns out.size() (0 on timeout/wake).
    std::size_t wait(std::vector<PollEvent>& out, std::chrono::milliseconds timeout,
                     std::error_code& ec) noexcept;
    std::size_t wait(std::vector<PollEvent>& out, std::chrono::milliseconds timeout)
    {
        std::error_code ec;
        const std::size_t n = wait(out, timeout, ec);
        detail::throw_if(ec, "Poller::wait");
        return n;
    }

    void wake() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pzynet
