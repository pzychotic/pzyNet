#pragma once

#include "pzynet/core/error.hpp"
#include "pzynet/socket/poller.hpp"
#include "pzynet/socket/udp_socket.hpp"
#include "pzynet/transport/engine.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pzynet
{
namespace transport
{

/// Drives an Engine over a UDP socket.
///
/// Two modes, same API:
///  * app-driven: call update() regularly from your own loop; nothing runs in the background.
///  * threaded:   call start_thread(); a background thread pumps the socket and the engine.
/// connect/disconnect/send/poll_event/wait_event/with_engine are thread-safe in both modes.
class UdpHost
{
public:
    /// The engine must outlive the host. `clock` supplies the time fed to the engine; empty means
    /// Clock::now(). A different source (e.g. a skewed clock in tests) changes only the engine's view of
    /// time, never how long the host's own calls wait.
    explicit UdpHost(Engine& engine, ClockSource clock = ClockSource())
        : engine_(engine), clock_(std::move(clock))
    {
    }
    ~UdpHost() { stop(); }
    UdpHost(const UdpHost&) = delete;
    UdpHost& operator=(const UdpHost&) = delete;

    /// Opens a non-blocking UDP socket bound to `local` (port 0 = ephemeral). An IPv6 local address
    /// yields a dual-stack socket.
    void bind(const Endpoint& local, std::error_code& ec);
    void bind(const Endpoint& local)
    {
        std::error_code ec;
        bind(local, ec);
        detail::throw_if(ec, "UdpHost::bind");
    }

    Endpoint local_endpoint() const { return socket_.local_endpoint(); }
    UdpSocket& socket() noexcept { return socket_; }

    /// App-driven mode: receives pending datagrams, advances the engine and sends its output. With a
    /// positive `max_wait`, first sleeps until input arrives, the engine's next deadline, or `max_wait`.
    void update(std::chrono::milliseconds max_wait, std::error_code& ec);
    void update(std::chrono::milliseconds max_wait = std::chrono::milliseconds(0))
    {
        std::error_code ec;
        update(max_wait, ec);
        detail::throw_if(ec, "UdpHost::update");
    }

    /// Threaded mode: starts the background thread. update() is not allowed until stop().
    void start_thread(std::error_code& ec);
    void start_thread()
    {
        std::error_code ec;
        start_thread(ec);
        detail::throw_if(ec, "UdpHost::start_thread");
    }
    /// Stops and joins the background thread. No-op if it is not running.
    void stop() noexcept;
    bool threaded() const noexcept { return thread_.joinable(); }

    ConnectionId connect(const Endpoint& to, ByteView user_data, std::error_code& ec);
    ConnectionId connect(const Endpoint& to, ByteView user_data = ByteView())
    {
        std::error_code ec;
        const ConnectionId id = connect(to, user_data, ec);
        detail::throw_if(ec, "UdpHost::connect");
        return id;
    }

    void disconnect(ConnectionId connection, DisconnectMode mode = DisconnectMode::graceful);

    void send(ConnectionId connection, ByteView data, const SendOptions& options, std::error_code& ec);
    void send(ConnectionId connection, ByteView data, const SendOptions& options = SendOptions())
    {
        std::error_code ec;
        send(connection, data, options, ec);
        detail::throw_if(ec, "UdpHost::send");
    }

    /// Pops the next event without blocking.
    bool poll_event(Event& out);

    /// Waits up to `timeout` for an event. In app-driven mode this runs update() while waiting.
    bool wait_event(Event& out, std::chrono::milliseconds timeout);

    /// Runs `f(engine)` with the engine locked, for protocol-specific calls that are not part of the
    /// Engine interface. Returns what `f` returns.
    template <class F>
    auto with_engine(F&& f) -> decltype(f(std::declval<Engine&>()))
    {
        struct Notify
        {
            UdpHost& host;
            ~Notify() { host.notify(); }
        };
        std::lock_guard<std::mutex> lock(mutex_);
        Notify notify_on_exit{*this}; // destroyed before the lock is released
        return f(engine_);
    }

private:
    bool pump();               // requires mutex_; returns true if any datagram was received
    void notify() noexcept;    // makes the background thread re-evaluate the engine
    std::chrono::milliseconds wait_time(std::chrono::milliseconds cap, bool busy) const; // requires mutex_
    void run();
    TimePoint now() const { return clock_ ? clock_() : Clock::now(); }

    Engine& engine_;
    ClockSource clock_;
    UdpSocket socket_;
    std::unique_ptr<Poller> poller_;
    std::vector<PollEvent> ready_;
    std::vector<std::uint8_t> buffer_;
    bool dont_fragment_ = false;

    mutable std::mutex mutex_; // guards engine_, events_, and the socket's send side
    std::condition_variable event_ready_;
    std::deque<Event> events_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
};

} // namespace transport
} // namespace pzynet
