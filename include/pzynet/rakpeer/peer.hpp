#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/clock.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/raknet/bit_stream.hpp"
#include "pzynet/raknet/guid.hpp"
#include "pzynet/raknet/raknet_engine.hpp"
#include "pzynet/rakpeer/packet.hpp"
#include "pzynet/transport/types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <variant>

namespace pzynet
{
namespace raknet
{

/// Who runs the Peer's networking.
enum class Threading
{
    background, ///< a background thread, as in RakNet
    app_driven, ///< the application, by calling Peer::update()
};

struct PeerConfig
{
    /// The RakNet engine: GUID, connection limits, password, time-outs, MTUs, ping response.
    Config engine;
    /// Local address to bind; port 0 picks an ephemeral port.
    Endpoint bind{IpAddress::any_v4(), 0};
    Threading threading = Threading::background;
    /// Byte order of the BitStream readers handed out with Packets.
    ByteOrder byte_order = ByteOrder::big_endian;
    /// Where the engine takes the current time from; empty means the steady clock.
    ClockSource clock;
};

/// A remote system, named by its GUID or by its address.
using Remote = std::variant<Guid, Endpoint>;

struct SendOptions
{
    transport::Reliability reliability = transport::Reliability::reliable_ordered;
    /// Ordering channel, 0..31.
    std::uint8_t channel = 0;
    transport::Priority priority = transport::Priority::medium;
};

/// A RakNet-style endpoint: owns a UDP socket and a RakNet engine and offers RakNet's application-level
/// behaviour. Connection events and received messages arrive through receive() as Packets, named by
/// RakNet message ids.
///
/// Connection events map onto RakNet's ids: an outgoing connection that succeeds yields
/// id::connection_request_accepted, an incoming one id::new_incoming_connection; a remote close yields
/// id::disconnection_notification, a time-out id::connection_lost. Connections this Peer closes itself
/// produce no Packet.
///
/// Timestamped messages (id::timestamp, an 8-byte time, then the real message id) are converted as
/// RakPeer does: on receipt the time is shifted from the sender's clock into this Peer's clock by the
/// connection's measured clock offset, rewritten in place in the Packet's data (in PeerConfig::byte_order)
/// and exposed as Packet::timestamp. A message whose header is shorter than 9 bytes is delivered
/// unmodified, without a timestamp. Outgoing messages are never altered.
///
/// All calls except start() and shutdown() may be made from any thread.
class Peer
{
public:
    Peer() noexcept;
    /// Shuts down as shutdown() does.
    ~Peer();
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    /// Binds the socket and, with Threading::background, starts the network thread.
    void start(PeerConfig config, std::error_code& ec);
    void start(PeerConfig config = PeerConfig())
    {
        std::error_code ec;
        start(std::move(config), ec);
        detail::throw_if(ec, "Peer::start");
    }

    /// Stops the network thread and closes the socket at once: connections are dropped without
    /// notifying the remote systems, and pending Packets are discarded. No-op if not running.
    void shutdown() noexcept;

    bool running() const noexcept { return impl_ != nullptr; }

    /// This Peer's GUID. Only meaningful while running.
    Guid guid() const;
    /// The bound local address. Only meaningful while running.
    Endpoint local_endpoint() const;

    /// The current time on this Peer's clock (PeerConfig::clock) in RakNet time: milliseconds, as
    /// RakNet's GetTime() returns. Write it after id::timestamp to send a timestamped message; Packet
    /// timestamps are expressed in it. While not running, the steady clock's time.
    std::uint64_t time() const;

    /// Starts connecting. The outcome arrives as a Packet: id::connection_request_accepted, or one of
    /// RakNet's connect-failure ids.
    void connect(const Endpoint& to, ByteView password, std::error_code& ec);
    void connect(const Endpoint& to, ByteView password = ByteView())
    {
        std::error_code ec;
        connect(to, password, ec);
        detail::throw_if(ec, "Peer::connect");
    }

    /// Closes a connection; with DisconnectMode::graceful, the remote system is notified once pending
    /// reliable data is delivered. Produces no Packet. Unknown remotes are ignored.
    void disconnect(const Remote& remote, transport::DisconnectMode mode = transport::DisconnectMode::graceful);

    /// Sends a message, which must start with its message id. Returns the send's receipt number,
    /// which is never 0.
    std::uint32_t send(const Remote& to, ByteView data, const SendOptions& options, std::error_code& ec);
    std::uint32_t send(const Remote& to, ByteView data, const SendOptions& options = SendOptions())
    {
        std::error_code ec;
        const std::uint32_t receipt = send(to, data, options, ec);
        detail::throw_if(ec, "Peer::send");
        return receipt;
    }
    std::uint32_t send(const Remote& to, const BitStreamWriter& message, const SendOptions& options,
                       std::error_code& ec)
    {
        return send(to, message.data(), options, ec);
    }
    std::uint32_t send(const Remote& to, const BitStreamWriter& message, const SendOptions& options = SendOptions())
    {
        return send(to, message.data(), options);
    }

    /// Sets how long reliable data to `remote` may stay unacknowledged before the connection is
    /// reported lost, overriding PeerConfig::engine.timeout (RakNet's SetTimeoutTime for one system).
    /// Errc::not_connected for an unknown remote, Errc::invalid_argument unless positive.
    void set_timeout(const Remote& remote, std::chrono::milliseconds timeout, std::error_code& ec);
    void set_timeout(const Remote& remote, std::chrono::milliseconds timeout)
    {
        std::error_code ec;
        set_timeout(remote, timeout, ec);
        detail::throw_if(ec, "Peer::set_timeout");
    }
    /// Sets the time-out of every connection and of future ones, replacing per-connection values
    /// (RakNet's SetTimeoutTime with UNASSIGNED_SYSTEM_ADDRESS). Non-positive values are ignored.
    void set_timeout(std::chrono::milliseconds timeout);
    /// The time-out of the connection to `remote`; nullopt if not connected.
    std::optional<std::chrono::milliseconds> timeout(const Remote& remote) const;

    /// This Peer's address as `remote` reported it during the handshake (RakNet's GetExternalID);
    /// behind a NAT it differs from local_endpoint(). nullopt if not connected.
    std::optional<Endpoint> external_address(const Remote& remote) const;

    /// The next Packet, if any. Never blocks.
    std::optional<Packet> receive();

    /// Threading::app_driven: runs the network once; with a positive `max_wait`, first waits up to
    /// that long for something to do.
    void update(std::chrono::milliseconds max_wait, std::error_code& ec);
    void update(std::chrono::milliseconds max_wait = std::chrono::milliseconds(0))
    {
        std::error_code ec;
        update(max_wait, ec);
        detail::throw_if(ec, "Peer::update");
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raknet
} // namespace pzynet
