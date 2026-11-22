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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

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
    /// Report the outcome as a receipt Packet: id::snd_receipt_acked once the message is acknowledged,
    /// or, for unreliable messages only, id::snd_receipt_loss once it is presumed lost. Not available
    /// for the sequenced reliabilities.
    bool ack_receipt = false;
};

/// One connected system, as Peer::connections() lists it.
struct RemoteSystem
{
    Guid guid;
    Endpoint address;
    /// Smoothed round-trip time; nullopt until the first acknowledgement arrived.
    std::optional<Duration> rtt;
    std::uint16_t mtu = 0;
    /// true if the remote system initiated the connection.
    bool incoming = false;
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
    /// Shuts down as shutdown() with zero linger does.
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

    /// Stops the Peer: stops the network thread, closes the socket and discards pending Packets.
    /// No-op if not running.
    ///
    /// With a positive linger, first closes every connection gracefully, as disconnect() does, and
    /// keeps the network running (in either threading mode) until every remote system has acknowledged
    /// the data queued before and its disconnection notification, but never longer than linger.
    /// No new connections are accepted meanwhile, and nothing more is reported through receive().
    /// With zero linger, connections are dropped at once without notifying the remote systems, which
    /// see them as lost after their time-out.
    void shutdown(std::chrono::milliseconds linger = std::chrono::milliseconds(0)) noexcept;

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
    /// Connects by host name (or address literal), resolved synchronously to its first IPv4 address.
    /// A name that does not resolve fails the call with Errc::host_not_found; no Packet follows.
    void connect(std::string_view host, std::uint16_t port, ByteView password, std::error_code& ec);
    void connect(std::string_view host, std::uint16_t port, ByteView password = ByteView())
    {
        std::error_code ec;
        connect(host, port, password, ec);
        detail::throw_if(ec, "Peer::connect");
    }

    /// Closes a connection; with DisconnectMode::graceful, the remote system is notified once pending
    /// reliable data is delivered. Produces no Packet. An unknown remote yields Errc::not_connected.
    void disconnect(const Remote& remote, transport::DisconnectMode mode, std::error_code& ec);
    /// As above, but unknown remotes are ignored.
    void disconnect(const Remote& remote, transport::DisconnectMode mode = transport::DisconnectMode::graceful)
    {
        std::error_code ec;
        disconnect(remote, mode, ec);
    }

    /// Sends a message, which must start with its message id; ids the protocol reserves for itself
    /// (see is_reserved()) are rejected with Errc::reserved_message_id. Returns the send's receipt
    /// number, which is never 0: every accepted send takes the next one, whether or not acknowledgement
    /// was requested. With SendOptions::ack_receipt, a receipt Packet carrying that number reports the
    /// outcome: [id::snd_receipt_acked or id::snd_receipt_loss][uint32 number], the number in host byte
    /// order as RakNet writes it (receipt_number() reads it).
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

    /// Diagnostics of the connection to `remote`; Errc::not_connected if there is none.
    ConnectionStats statistics(const Remote& remote, std::error_code& ec) const;
    ConnectionStats statistics(const Remote& remote) const
    {
        std::error_code ec;
        ConnectionStats stats = statistics(remote, ec);
        detail::throw_if(ec, "Peer::statistics");
        return stats;
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

    /// The connected systems: those whose connection Packet was produced and that have not
    /// disconnected since. Empty if not running.
    std::vector<RemoteSystem> connections() const;
    /// The GUID of the system connected from `address`, if any.
    std::optional<Guid> guid_of(const Endpoint& address) const;
    /// The address of the connected system `guid`, if any.
    std::optional<Endpoint> address_of(const Guid& guid) const;

    /// Sends an offline ping (ID_UNCONNECTED_PING); no connection is needed. A reply arrives as a
    /// Packet id::unconnected_pong: `[id][u32 ping send time in RakNet time][the responder's offline
    /// ping response]`. With `only_if_accepting` (ID_UNCONNECTED_PING_OPEN_CONNECTIONS), systems without
    /// a free incoming slot stay silent.
    void ping(const Endpoint& to, bool only_if_accepting, std::error_code& ec);
    void ping(const Endpoint& to, bool only_if_accepting = false)
    {
        std::error_code ec;
        ping(to, only_if_accepting, ec);
        detail::throw_if(ec, "Peer::ping");
    }

    /// Sends ID_ADVERTISE_SYSTEM with `data` (at most 399 bytes) to a system without connecting. It
    /// arrives as a Packet id::advertise_system: `[id][data]`.
    void advertise(const Endpoint& to, ByteView data, std::error_code& ec);
    void advertise(const Endpoint& to, ByteView data = ByteView())
    {
        std::error_code ec;
        advertise(to, data, ec);
        detail::throw_if(ec, "Peer::advertise");
    }

    /// The payload returned to offline pings (at most 399 bytes; empty for none).
    void set_offline_ping_response(ByteView data, std::error_code& ec);
    void set_offline_ping_response(ByteView data)
    {
        std::error_code ec;
        set_offline_ping_response(data, ec);
        detail::throw_if(ec, "Peer::set_offline_ping_response");
    }
    /// How many systems may connect to this Peer. Existing connections are kept when lowering it.
    void set_max_incoming_connections(std::size_t count, std::error_code& ec);
    void set_max_incoming_connections(std::size_t count)
    {
        std::error_code ec;
        set_max_incoming_connections(count, ec);
        detail::throw_if(ec, "Peer::set_max_incoming_connections");
    }
    /// The password connecting systems must present (at most 255 bytes); empty for none.
    void set_incoming_password(ByteView password, std::error_code& ec);
    void set_incoming_password(ByteView password)
    {
        std::error_code ec;
        set_incoming_password(password, ec);
        detail::throw_if(ec, "Peer::set_incoming_password");
    }

    /// Sends a message to every connected system except `exclude`, as send() does. All copies share one
    /// receipt number; with acknowledgement, each system's copy reports on its own receipt Packet.
    std::uint32_t broadcast(ByteView data, const SendOptions& options, const std::optional<Remote>& exclude,
                            std::error_code& ec);
    std::uint32_t broadcast(ByteView data, const SendOptions& options = SendOptions(),
                            const std::optional<Remote>& exclude = std::nullopt)
    {
        std::error_code ec;
        const std::uint32_t receipt = broadcast(data, options, exclude, ec);
        detail::throw_if(ec, "Peer::broadcast");
        return receipt;
    }
    std::uint32_t broadcast(const BitStreamWriter& message, const SendOptions& options,
                            const std::optional<Remote>& exclude, std::error_code& ec)
    {
        return broadcast(message.data(), options, exclude, ec);
    }
    std::uint32_t broadcast(const BitStreamWriter& message, const SendOptions& options = SendOptions(),
                            const std::optional<Remote>& exclude = std::nullopt)
    {
        return broadcast(message.data(), options, exclude);
    }

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
