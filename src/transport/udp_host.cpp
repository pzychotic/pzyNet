#include "pzynet/transport/udp_host.hpp"

#include <algorithm>

namespace pzynet
{
namespace transport
{
namespace
{
constexpr std::size_t receive_buffer_size = 65536; // any UDP datagram fits
constexpr int max_datagrams_per_pump = 1024;       // keeps one busy socket from starving the timers
} // namespace

void UdpHost::bind(const Endpoint& local, std::error_code& ec)
{
    if (socket_.is_open())
    {
        ec = make_error_code(Errc::already_open);
        return;
    }
    auto poller = std::make_unique<Poller>(ec);
    if (ec)
        return;

    socket_.open(local.address().family(), ec);
    if (!ec && local.address().is_v6())
        socket_.set_v6_only(false, ec);
    if (!ec)
        socket_.bind(local, ec);
    if (!ec)
        socket_.set_non_blocking(true, ec);
    if (!ec)
        poller->add(socket_, PollEvents::readable, ec);
    if (ec)
    {
        socket_.close();
        return;
    }
    std::error_code ignored; // larger buffers are an optimisation; the OS may clamp or refuse them
    socket_.set_receive_buffer_size(512 * 1024, ignored);
    socket_.set_send_buffer_size(256 * 1024, ignored);

    poller_ = std::move(poller);
    buffer_.resize(receive_buffer_size);
}

bool UdpHost::pump()
{
    const TimePoint now = Clock::now();
    bool received_any = false;

    for (int i = 0; i < max_datagrams_per_pump; ++i)
    {
        Endpoint from;
        std::error_code ec;
        const std::size_t size = socket_.receive_from(buffer_, from, ec);
        if (ec)
            break; // would-block: drained. Anything else is transient for UDP; retry on the next pump.
        received_any = true;
        engine_.handle_datagram(from, ByteView(buffer_.data(), size), now);
    }

    engine_.advance(now);

    Transmit transmit;
    while (engine_.poll_transmit(transmit))
    {
        std::error_code ec;
        if (transmit.dont_fragment != dont_fragment_)
        {
            socket_.set_dont_fragment(transmit.dont_fragment, ec);
            dont_fragment_ = transmit.dont_fragment;
        }
        // A failed send is a lost datagram, which the protocol has to cope with anyway.
        socket_.send_to(transmit.data, transmit.to, ec);
    }

    bool new_events = false;
    Event event;
    while (engine_.poll_event(event))
    {
        events_.push_back(std::move(event));
        new_events = true;
    }
    if (new_events)
        event_ready_.notify_all();
    return received_any;
}

std::chrono::milliseconds UdpHost::wait_time(std::chrono::milliseconds cap, bool busy) const
{
    using std::chrono::milliseconds;
    const std::optional<TimePoint> deadline = engine_.next_deadline();
    if (!deadline)
        return cap;
    const TimePoint now = Clock::now();
    milliseconds wait(0);
    if (*deadline > now)
        wait = std::chrono::duration_cast<milliseconds>(*deadline - now) + milliseconds(1); // round up
    else if (!busy)
        wait = milliseconds(1); // an overdue deadline right after advance(): don't spin on it
    return cap.count() < 0 ? wait : std::min(wait, cap);
}

void UdpHost::update(std::chrono::milliseconds max_wait, std::error_code& ec)
{
    ec.clear();
    if (!socket_.is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    if (threaded())
    {
        ec = make_error_code(Errc::running);
        return;
    }

    std::chrono::milliseconds wait(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool busy = pump();
        if (max_wait.count() <= 0 || busy || !events_.empty())
            return;
        wait = wait_time(max_wait, false);
    }
    poller_->wait(ready_, wait, ec);
    if (ec)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    pump();
}

void UdpHost::start_thread(std::error_code& ec)
{
    ec.clear();
    if (!socket_.is_open())
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    if (threaded())
    {
        ec = make_error_code(Errc::running);
        return;
    }
    stop_requested_ = false;
    try
    {
        thread_ = std::thread([this] { run(); });
    }
    catch (const std::system_error& error)
    {
        ec = error.code();
    }
}

void UdpHost::stop() noexcept
{
    if (!thread_.joinable())
        return;
    stop_requested_ = true;
    poller_->wake();
    thread_.join();
}

void UdpHost::run()
{
    while (!stop_requested_)
    {
        std::chrono::milliseconds wait(0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool busy = pump();
            wait = busy ? std::chrono::milliseconds(0) : wait_time(std::chrono::milliseconds(-1), false);
        }
        std::error_code ec;
        poller_->wait(ready_, wait, ec);
        if (ec) // should not happen; avoid a hot loop if it does
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void UdpHost::notify() noexcept
{
    if (threaded() && poller_)
        poller_->wake();
}

ConnectionId UdpHost::connect(const Endpoint& to, ByteView user_data, std::error_code& ec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const ConnectionId id = engine_.connect(to, user_data, ec);
    notify();
    return id;
}

void UdpHost::disconnect(ConnectionId connection, DisconnectMode mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.disconnect(connection, mode);
    notify();
}

void UdpHost::send(ConnectionId connection, ByteView data, const SendOptions& options, std::error_code& ec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.send(connection, data, options, ec);
    notify();
}

bool UdpHost::poll_event(Event& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty())
    {
        // Commands can produce events without any I/O (e.g. disconnect); collect those too.
        Event event;
        while (engine_.poll_event(event))
            events_.push_back(std::move(event));
        if (events_.empty())
            return false;
    }
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool UdpHost::wait_event(Event& out, std::chrono::milliseconds timeout)
{
    const TimePoint give_up = Clock::now() + timeout;
    if (threaded())
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!event_ready_.wait_until(lock, give_up, [this] { return !events_.empty(); }))
            return false;
        out = std::move(events_.front());
        events_.pop_front();
        return true;
    }
    for (;;)
    {
        if (poll_event(out))
            return true;
        const TimePoint now = Clock::now();
        if (now >= give_up)
            return false;
        std::error_code ec;
        update(std::chrono::duration_cast<std::chrono::milliseconds>(give_up - now) + std::chrono::milliseconds(1),
               ec);
        if (ec)
            return false;
    }
}

} // namespace transport
} // namespace pzynet
