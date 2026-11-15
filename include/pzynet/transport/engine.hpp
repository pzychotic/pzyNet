#pragma once

#include "pzynet/core/clock.hpp"
#include "pzynet/core/span.hpp"
#include "pzynet/transport/types.hpp"

#include <optional>
#include <system_error>

namespace pzynet
{
namespace transport
{

/// A datagram protocol as a pure state machine ("sans I/O").
///
/// An engine owns no socket, no thread and no clock. Whoever drives it
///   1. feeds received datagrams to handle_datagram(),
///   2. calls advance() when next_deadline() is reached (and after feeding input or issuing commands),
///   3. puts everything from poll_transmit() on the wire, and
///   4. hands everything from poll_event() to the application.
/// UdpHost does exactly that over a UDP socket; tests do it in memory with a fake clock.
///
/// Engines are not thread-safe; all calls must be serialised by the driver.
class Engine
{
public:
    virtual ~Engine() = default;

    virtual void handle_datagram(const Endpoint& from, ByteView data, TimePoint now) = 0;

    /// Runs timers: retransmissions, acknowledgements, keep-alives, time-outs, queued sends.
    virtual void advance(TimePoint now) = 0;

    /// Pops the next datagram to send. Returns false when there is none.
    virtual bool poll_transmit(Transmit& out) = 0;

    /// Pops the next application event. Returns false when there is none.
    virtual bool poll_event(Event& out) = 0;

    /// When advance() needs to run next; nullopt if only input or a command can create work.
    /// A value in the past means "as soon as possible".
    virtual std::optional<TimePoint> next_deadline() const = 0;

    /// Starts connecting to `to`. The outcome arrives as a Connected or ConnectFailed event carrying
    /// the returned id. `user_data` is protocol-defined connection data (e.g. a password).
    virtual ConnectionId connect(const Endpoint& to, ByteView user_data, std::error_code& ec) = 0;

    /// Closes a connection. A Disconnected event (closed_locally) follows. Unknown ids are ignored.
    virtual void disconnect(ConnectionId connection, DisconnectMode mode) = 0;

    /// Queues a message on an established connection.
    virtual void send(ConnectionId connection, ByteView data, const SendOptions& options,
                      std::error_code& ec) = 0;
};

} // namespace transport
} // namespace pzynet
