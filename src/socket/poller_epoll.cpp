#include "platform.hpp"

#include "pzynet/socket/poller.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>

namespace pzynet
{

struct Poller::Impl
{
    int epoll_fd = -1;
    int wake_fd = -1;

    ~Impl()
    {
        if (wake_fd >= 0)
            ::close(wake_fd);
        if (epoll_fd >= 0)
            ::close(epoll_fd);
    }

    static std::uint32_t to_native(PollEvents interest) noexcept
    {
        std::uint32_t events = 0;
        if (has(interest, PollEvents::readable))
            events |= EPOLLIN;
        if (has(interest, PollEvents::writable))
            events |= EPOLLOUT;
        return events;
    }

    void control(int operation, NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept
    {
        epoll_event event{};
        event.events = to_native(interest);
        event.data.fd = socket;
        ec = ::epoll_ctl(epoll_fd, operation, socket, &event) == 0 ? std::error_code()
                                                                    : detail::last_socket_error();
    }
};

Poller::Poller(std::error_code& ec) noexcept
{
    ec.clear();
    impl_.reset(new (std::nothrow) Impl);
    if (!impl_)
    {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return;
    }
    impl_->epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    impl_->wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (impl_->epoll_fd < 0 || impl_->wake_fd < 0)
    {
        ec = detail::last_socket_error();
        impl_.reset();
        return;
    }
    impl_->control(EPOLL_CTL_ADD, impl_->wake_fd, PollEvents::readable, ec);
    if (ec)
        impl_.reset();
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
    if (!impl_ || socket == invalid_socket)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->control(EPOLL_CTL_ADD, socket, interest, ec);
}

void Poller::modify(NativeSocket socket, PollEvents interest, std::error_code& ec) noexcept
{
    if (!impl_ || socket == invalid_socket)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->control(EPOLL_CTL_MOD, socket, interest, ec);
}

void Poller::remove(NativeSocket socket, std::error_code& ec) noexcept
{
    if (!impl_ || socket == invalid_socket)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    impl_->control(EPOLL_CTL_DEL, socket, PollEvents::none, ec);
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
    const auto count = timeout.count();
    const int native_timeout = count < 0 ? -1 : count > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(count);

    epoll_event events[64];
    int ready;
    do
        ready = ::epoll_wait(impl_->epoll_fd, events, 64, native_timeout);
    while (ready < 0 && errno == EINTR && native_timeout < 0);
    if (ready < 0)
    {
        ec = errno == EINTR ? std::error_code() : detail::last_socket_error();
        return 0;
    }
    ec.clear();

    for (int i = 0; i < ready; ++i)
    {
        if (events[i].data.fd == impl_->wake_fd)
        {
            std::uint64_t counter;
            while (::read(impl_->wake_fd, &counter, sizeof counter) > 0)
            {
            }
            continue;
        }
        PollEvents flags = PollEvents::none;
        if (events[i].events & EPOLLIN)
            flags = flags | PollEvents::readable;
        if (events[i].events & EPOLLOUT)
            flags = flags | PollEvents::writable;
        if (events[i].events & EPOLLERR)
            flags = flags | PollEvents::error;
        if (events[i].events & (EPOLLHUP | EPOLLRDHUP))
            flags = flags | PollEvents::hang_up;
        try
        {
            out.push_back(PollEvent{events[i].data.fd, flags});
        }
        catch (...)
        {
            ec = std::make_error_code(std::errc::not_enough_memory);
            break;
        }
    }
    return out.size();
}

void Poller::wake() noexcept
{
    if (!impl_)
        return;
    const std::uint64_t one = 1;
    const ssize_t written = ::write(impl_->wake_fd, &one, sizeof one);
    (void)written; // EAGAIN means a wake-up is already pending
}

} // namespace pzynet
