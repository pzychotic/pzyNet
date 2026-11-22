#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/core/clock.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/transport/engine.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pzynet
{
namespace raknet
{

struct Config
{
    /// Identifies this peer to others (RakNetGUID). 0 = choose a random one.
    std::uint64_t guid = 0;

    /// Upper bound on simultaneous connections, incoming and outgoing together.
    std::size_t max_connections = 64;
    /// How many of those may be initiated by peers. 0 = do not accept connections (pure client).
    std::size_t max_incoming_connections = 0;
    /// Peers must present exactly these bytes to connect. Empty = no password.
    std::vector<std::uint8_t> incoming_password;

    /// A connection is dropped when reliable data stays unacknowledged this long.
    std::chrono::milliseconds timeout{10000};
    /// How long a connection may take from first contact to established.
    std::chrono::milliseconds handshake_timeout{10000};

    /// MTU sizes probed when connecting, largest first. Values are clamped to 576..1492.
    std::vector<std::uint16_t> mtu_sizes{1492, 1200, 576};
    /// Connection attempts in total, spread evenly over mtu_sizes.
    unsigned connect_attempts = 12;
    std::chrono::milliseconds connect_attempt_interval{500};

    /// Payload returned to unconnected pings (server discovery).
    std::vector<std::uint8_t> offline_ping_response;

    /// Largest message accepted for sending and for reassembly on receipt.
    std::size_t max_message_size = 16u * 1024 * 1024;
};

struct ConnectionInfo
{
    Endpoint remote;
    std::uint64_t remote_guid = 0;
    std::uint16_t mtu = 0;
    bool incoming = false;
    /// Smoothed round-trip time; nullopt until the first acknowledgement arrived.
    std::optional<Duration> rtt;
    /// Estimated (remote clock - local clock) in RakNet time; nullopt until measured.
    /// Subtract it from a remote ID_TIMESTAMP value to express it in local time (see wire_time()).
    std::optional<std::chrono::milliseconds> clock_offset;
    /// How long reliable data may stay unacknowledged before the connection is dropped.
    std::chrono::milliseconds timeout{0};
    /// This side's address as the remote system reported it during the handshake (RakNet's
    /// GetExternalID); nullopt until the handshake delivered it.
    std::optional<Endpoint> external_address;
};

/// Basic diagnostics of one connection, counted since its handshake reached the reliability layer.
struct ConnectionStats
{
    /// UDP payload bytes of the connection's datagrams (data, acknowledgements, retransmissions).
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    /// Application messages: accepted by send(), and delivered as Message events. The engine's own
    /// messages (handshake, pings, disconnection notice) are not counted.
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    /// Datagrams sent that carried at least one retransmitted message.
    std::uint64_t datagrams_resent = 0;
    /// Data datagrams the remote system reported missing (NAK) or that went unacknowledged until a
    /// retransmission time-out; each is counted once, even if it turns up later.
    std::uint64_t datagrams_lost = 0;
    /// Messages waiting in the send queue, and their payload bytes. A split message counts once until
    /// its last piece leaves; the engine's own queued messages are included.
    std::size_t queued_messages = 0;
    std::size_t queued_bytes = 0;
};

/// The RakNet 4.x wire protocol (protocol version 6) as a transport::Engine.
///
/// Wire-compatible with stock RakNet peers built without security and without IPv6 support.
/// Messages are passed through untouched, so when talking to a RakNet application the payload must
/// start with a message id that application understands (>= id::user_packet_enum for user messages).
/// A payload must never start with one of the ids this layer consumes itself; send() rejects those
/// with Errc::reserved_message_id.
///
/// connect()'s `user_data` is the password. SendOptions map 1:1 onto RakNet reliabilities, priorities
/// and ordering channels (0..31); a non-zero receipt_token corresponds to the *_WITH_ACK_RECEIPT
/// variants (not available for sequenced messages).
///
/// Unconnected events carry raw offline datagrams: decode them with wire::decode(UnconnectedPong) or
/// wire::decode(OutOfBand) from pzynet/raknet/wire.hpp.
class RakNetEngine final : public transport::Engine
{
public:
    explicit RakNetEngine(Config config = Config());
    ~RakNetEngine() override;
    RakNetEngine(const RakNetEngine&) = delete;
    RakNetEngine& operator=(const RakNetEngine&) = delete;

    // transport::Engine
    void handle_datagram(const Endpoint& from, ByteView data, TimePoint now) override;
    void advance(TimePoint now) override;
    bool poll_transmit(transport::Transmit& out) override;
    bool poll_event(transport::Event& out) override;
    std::optional<TimePoint> next_deadline() const override;
    transport::ConnectionId connect(const Endpoint& to, ByteView password, std::error_code& ec) override;
    void disconnect(transport::ConnectionId connection, transport::DisconnectMode mode) override;
    void send(transport::ConnectionId connection, ByteView data, const transport::SendOptions& options,
              std::error_code& ec) override;

    // RakNet specifics
    std::uint64_t guid() const noexcept;
    std::size_t connection_count() const noexcept;
    std::optional<ConnectionInfo> connection_info(transport::ConnectionId connection) const;
    /// nullopt for a connection the application cannot see (unknown, closed or failed).
    std::optional<ConnectionStats> connection_stats(transport::ConnectionId connection) const;
    /// Every connection visible to the application (connected or still connecting), in no particular order.
    std::vector<transport::ConnectionId> connections() const;
    /// Connections closed gracefully that are still getting their last data out; each goes once the
    /// remote has acknowledged everything, or after the time-out.
    std::size_t closing_count() const noexcept;

    void set_max_incoming_connections(std::size_t count);
    void set_incoming_password(ByteView password);
    void set_offline_ping_response(ByteView data);

    /// Sets the time-out of one connection, overriding Config::timeout (RakNet's SetTimeoutTime for
    /// one system). Errc::unknown_connection for an unknown id, Errc::invalid_argument unless positive.
    void set_timeout(transport::ConnectionId connection, std::chrono::milliseconds timeout, std::error_code& ec);
    void set_timeout(transport::ConnectionId connection, std::chrono::milliseconds timeout)
    {
        std::error_code ec;
        set_timeout(connection, timeout, ec);
        detail::throw_if(ec, "RakNetEngine::set_timeout");
    }
    /// Sets the time-out for new connections and for every existing one, replacing per-connection
    /// values (RakNet's SetTimeoutTime with UNASSIGNED_SYSTEM_ADDRESS). Non-positive values are ignored.
    void set_timeout(std::chrono::milliseconds timeout);

    /// Sends an unconnected ping; a reply arrives as an Unconnected event holding an UnconnectedPong.
    /// With `only_if_accepting`, peers that have no free incoming slot stay silent.
    void ping(const Endpoint& to, bool only_if_accepting = false);

    /// Sends ID_ADVERTISE_SYSTEM with `data` to a peer without connecting (at most 399 bytes).
    void advertise(const Endpoint& to, ByteView data, std::error_code& ec);

    /// The value RakNet timestamps carry for `time`: milliseconds on this process's monotonic clock.
    static std::uint64_t wire_time(TimePoint time) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace raknet
} // namespace pzynet
