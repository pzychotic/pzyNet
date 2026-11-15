#include "platform.hpp"

#include "pzynet/socket/poller.hpp"

#include <algorithm>

namespace pzynet
{

// WSAPoll over a flat array. Slot 0 is a loopback UDP socket that wake() sends a byte to.
struct Poller::Impl
{
    std::vector<WSAPOLLFD> fds;
    SOCKET wake_socket = INVALID_SOCKET;
    sockaddr_in wake_address{};

    ~Impl()
    {
        if (wake_socket != INVALID_SOCKET)
            ::closesocket(wake_socket);
    }

    static SHORT to_native(PollEvents interest) noexcept
    {
        SHORT events = 0;
        if (has(interest, PollEvents::readable))
            events |= POLLRDNORM;
        if (has(interest, PollEvents::writable))
            events |= POLLWRNORM;
        return events;
    }

    std::vector<WSAPOLLFD>::iterator find(NativeSocket socket) noexcept
    {
        return std::find_if(fds.begin() + 1, fds.end(),
                            [socket](const WSAPOLLFD& fd) { return fd.fd == static_cast<SOCKET>(socket); });
    }
};

Poller::Poller(std::error_code& ec) noexcept
{
    detail::ensure_initialized();
    ec.clear();
    impl_.reset(new (std::nothrow) Impl);
    if (!impl_)
    {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return;
    }

    Impl& impl = *impl_;
    impl.wake_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof address;
    u_long non_blocking = 1;
    if (impl.wake_socket == INVALID_SOCKET ||
        ::bind(impl.wake_socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
        ::getsockname(impl.wake_socket, reinterpret_cast<sockaddr*>(&impl.wake_address), &length) != 0 ||
        ::ioctlsocket(impl.wake_socket, FIONBIO, &non_blocking) != 0)
    {
        ec = detail::last_socket_error();
        impl_.reset();
        return;
    }
    impl.fds.push_back(WSAPOLLFD{impl.wake_socket, POLLRDNORM, 0});
}

Poller::Poller()
{
    std::error_code ec;
    *this = Poller(ec);
    detail::throw_if(ec, "Poller");
}

Poller::~Poller() = default;
Poller::Poller(Poller&&) noexcept = default;
Poller& Poller::operator=(Poller&&) noexcept = default;

void Poller::add(NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept
{
    if (!impl_ || socket == invalid_socket || impl_->find(socket) != impl_->fds.end())
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->fds.push_back(WSAPOLLFD{static_cast<SOCKET>(socket), Impl::to_native(interest), 0});
    ec.clear();
}

void Poller::modify(NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept
{
    if (!impl_ || impl_->find(socket) == impl_->fds.end())
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->find(socket)->events = Impl::to_native(interest);
    ec.clear();
}

void Poller::remove(NativeSocket socket, std::error_code& ec) noexcept
{
    if (!impl_ || impl_->find(socket) == impl_->fds.end())
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->fds.erase(impl_->find(socket));
    ec.clear();
}

std::size_t Poller::wait(std::vector<PollEvent>& out, std::chrono::milliseconds timeout,
                         std::error_code& ec) noexcept
{
    out.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::invalid_argument);
        return 0;
    }
    Impl& impl = *impl_;
    const auto count = timeout.count();
    const INT native_timeout = count < 0 ? -1 : count > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<INT>(count);
    const int ready = ::WSAPoll(impl.fds.data(), static_cast<ULONG>(impl.fds.size()), native_timeout);
    if (ready < 0)
    {
        ec = detail::last_socket_error();
        return 0;
    }
    ec.clear();

    if (impl.fds[0].revents != 0)
    {
        char drain[16];
        while (::recv(impl.wake_socket, drain, sizeof drain, 0) >= 0)
        {
        }
    }
    for (std::size_t i = 1; i < impl.fds.size(); ++i)
    {
        const SHORT revents = impl.fds[i].revents;
        if (revents == 0)
            continue;
        PollEvents events = PollEvents::none;
        if (revents & (POLLRDNORM | POLLRDBAND))
            events = events | PollEvents::readable;
        if (revents & POLLWRNORM)
            events = events | PollEvents::writable;
        if (revents & (POLLERR | POLLNVAL))
            events = events | PollEvents::error;
        if (revents & POLLHUP)
            events = events | PollEvents::hang_up;
        out.push_back(PollEvent{static_cast<NativeSocket>(impl.fds[i].fd), events});
    }
    return out.size();
}

void Poller::wake() noexcept
{
    if (!impl_)
        return;
    const char byte = 1;
    ::sendto(impl_->wake_socket, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&impl_->wake_address),
             sizeof impl_->wake_address);
}

} // namespace pzynet
