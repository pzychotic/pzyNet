#include "pzynet/raknet/raknet_engine.hpp"

#include "session.hpp"

#include "pzynet/core/error.hpp"
#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/wire.hpp"

#include <algorithm>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>

namespace pzynet
{
namespace raknet
{
namespace
{

using detail::Session;
using transport::ConnectFailure;
using transport::ConnectionId;
using transport::DisconnectReason;
using transport::Priority;
using transport::Reliability;
using wire::Bytes;

constexpr std::size_t maximum_early_messages = 256;
constexpr std::size_t maximum_out_of_band_size = 399;

enum class State
{
    requesting,       // outgoing: exchanging the offline open-connection messages
    awaiting_accept,  // outgoing: ID_CONNECTION_REQUEST sent
    unverified,       // incoming: reply 2 sent, waiting for ID_CONNECTION_REQUEST
    handling_request, // incoming: accepted, waiting for ID_NEW_INCOMING_CONNECTION
    connected,
    closing, // no longer visible to the application; flushing what is left, then removed
    dead,    // removed at the next sweep
};

struct Connection
{
    ConnectionId id = transport::invalid_connection;
    Endpoint remote;
    std::uint64_t remote_guid = 0;
    State state = State::requesting;
    bool incoming = false;
    bool announced = false; // a Connected event was emitted
    std::unique_ptr<Session> session;
    TimePoint created{};
    TimePoint close_deadline{};

    // outgoing handshake
    Bytes password;
    unsigned attempts = 0;
    TimePoint next_attempt = TimePoint::min();
    bool have_reply_1 = false;
    std::uint16_t mtu = 0;

    // messages that arrived before the handshake completed
    std::vector<transport::Message> early;

    std::optional<std::chrono::milliseconds> clock_offset;

    // application messages, for ConnectionStats
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
};

std::uint64_t random_guid()
{
    std::random_device device;
    std::uint64_t value = 0;
    while (value == 0)
        value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    return value;
}

/// Ids this layer interprets itself; an application payload must not start with one of them.
bool is_reserved(std::uint8_t message_id) noexcept
{
    switch (message_id)
    {
    case id::connected_ping:
    case id::connected_pong:
    case id::detect_lost_connections:
    case id::connection_request:
    case id::connection_request_accepted:
    case id::new_incoming_connection:
    case id::disconnection_notification:
    case id::invalid_password:
        return true;
    default:
        return false;
    }
}

std::uint16_t clamp_mtu(std::size_t mtu) noexcept
{
    return static_cast<std::uint16_t>(
        std::min<std::size_t>(std::max<std::size_t>(mtu, wire::minimum_mtu), wire::maximum_mtu));
}

constexpr std::size_t maximum_ban_length = 15; // a dotted IPv4 address, as RakNet allows

struct Ban
{
    std::string address;
    std::optional<TimePoint> expiry; // nullopt = permanent
};

/// RakNet's IsBanned matching: equal strings match, and a '*' in the ban matches whatever follows.
bool ban_matches(std::string_view ban, std::string_view address) noexcept
{
    for (std::size_t i = 0;; ++i)
    {
        const char b = i < ban.size() ? ban[i] : '\0';
        const char a = i < address.size() ? address[i] : '\0';
        if (b == a)
        {
            if (a == '\0')
                return true;
            continue;
        }
        return b == '*' && a != '\0';
    }
}

template <class T>
bool pop(std::deque<T>& queue, T& out)
{
    if (queue.empty())
        return false;
    out = std::move(queue.front());
    queue.pop_front();
    return true;
}

} // namespace

struct RakNetEngine::Impl
{
    Config config;
    TimePoint now{};
    ConnectionId next_id = 1;
    std::unordered_map<Endpoint, std::unique_ptr<Connection>> by_endpoint;
    std::unordered_map<ConnectionId, Connection*> by_id;
    std::deque<transport::Transmit> transmits;
    std::deque<transport::Event> events;
    std::vector<Bytes> scratch;
    // Pings are stamped with the send time, so they wait for the next advance() to learn it.
    std::vector<std::pair<Endpoint, bool>> pending_pings;
    std::vector<Ban> bans;
    // Our address as the first completed handshake reported it (RakNet's firstExternalID).
    std::optional<IpAddress> external_ip;

    void learn_external_address(const Endpoint& address)
    {
        if (!external_ip)
            external_ip = address.address();
    }

    /// RakNet's reconnect limit: a connection from this IP was set up less than 100 ms ago.
    bool connected_recently(const IpAddress& address) const
    {
        if (!config.limit_ip_connection_frequency || address == external_ip)
            return false;
        constexpr std::chrono::milliseconds window{100};
        return std::any_of(by_endpoint.begin(), by_endpoint.end(), [&](const auto& entry) {
            const Connection& other = *entry.second;
            return other.state != State::dead && other.state != State::requesting &&
                   other.remote.address() == address && now >= other.created && now - other.created < window;
        });
    }

    std::uint64_t now_ms() const noexcept { return RakNetEngine::wire_time(now); }

    bool is_banned(std::string_view address, TimePoint at) const
    {
        return std::any_of(bans.begin(), bans.end(), [&](const Ban& ban) {
            return (!ban.expiry || *ban.expiry >= at) && ban_matches(ban.address, address);
        });
    }

    /// RakNet's ban check, which runs before anything else for every datagram.
    bool refuse_if_banned(const Endpoint& from)
    {
        if (bans.empty())
            return false;
        bans.erase(std::remove_if(bans.begin(), bans.end(),
                                  [&](const Ban& ban) { return ban.expiry && *ban.expiry < now; }),
                   bans.end());
        if (!is_banned(from.address().to_string(), now))
            return false;
        wire::Rejection rejection;
        rejection.id = id::connection_banned;
        rejection.guid = config.guid;
        transmit(from, wire::encode(rejection));
        return true;
    }

    void transmit(const Endpoint& to, Bytes data, bool dont_fragment = false)
    {
        transmits.push_back(transport::Transmit{to, std::move(data), dont_fragment});
    }

    Connection* find(const Endpoint& endpoint)
    {
        const auto it = by_endpoint.find(endpoint);
        return it == by_endpoint.end() || it->second->state == State::dead ? nullptr : it->second.get();
    }

    Connection* find(ConnectionId connection) const
    {
        const auto it = by_id.find(connection);
        return it == by_id.end() ? nullptr : it->second;
    }

    Connection& create(const Endpoint& remote, bool incoming)
    {
        auto connection = std::make_unique<Connection>();
        connection->id = next_id++;
        connection->remote = remote;
        connection->incoming = incoming;
        connection->created = now;
        Connection& reference = *connection;
        by_id[reference.id] = &reference;
        by_endpoint[remote] = std::move(connection);
        return reference;
    }

    std::size_t incoming_count() const
    {
        std::size_t count = 0;
        for (const auto& entry : by_endpoint)
            if (entry.second->incoming && entry.second->state != State::dead && entry.second->state != State::closing)
                ++count;
        return count;
    }

    void start_session(Connection& connection, std::uint16_t mtu)
    {
        detail::SessionConfig session_config;
        session_config.mtu = clamp_mtu(mtu);
        session_config.timeout = config.timeout;
        session_config.max_message_size = config.max_message_size;
        connection.mtu = static_cast<std::uint16_t>(session_config.mtu);
        connection.session = std::make_unique<Session>(session_config, now);
        connection.created = now;
    }

    void send_internal(Connection& connection, const Bytes& payload, Reliability reliability, Priority priority)
    {
        transport::SendOptions options;
        options.reliability = reliability;
        options.priority = priority;
        options.channel = 0;
        connection.session->send(payload, options, now);
    }

    /// Hides the connection from the application; it is removed at the next sweep.
    void kill(Connection& connection)
    {
        by_id.erase(connection.id);
        connection.state = State::dead;
    }

    /// Hides the connection from the application but keeps it until its session is flushed, so that
    /// final messages get out and the peer's last datagrams are still acknowledged.
    void begin_close(Connection& connection)
    {
        by_id.erase(connection.id);
        connection.state = State::closing;
        connection.close_deadline = now + config.timeout;
    }

    void fail(Connection& connection, ConnectFailure reason, bool linger = false)
    {
        events.push_back(transport::ConnectFailed{connection.id, connection.remote, reason});
        if (linger && connection.session)
            begin_close(connection);
        else
            kill(connection);
    }

    void announce(Connection& connection)
    {
        connection.state = State::connected;
        connection.announced = true;
        events.push_back(transport::Connected{connection.id, connection.remote, connection.incoming});
        for (transport::Message& message : connection.early)
        {
            ++connection.messages_received;
            events.push_back(std::move(message));
        }
        connection.early.clear();
    }

    void sweep()
    {
        for (auto it = by_endpoint.begin(); it != by_endpoint.end();)
        {
            if (it->second->state == State::dead)
                it = by_endpoint.erase(it);
            else
                ++it;
        }
    }

    void record_clock(Connection& connection, std::uint64_t sent, std::uint64_t remote_time)
    {
        // The remote clock was read halfway between our sending and now.
        const std::int64_t midpoint = static_cast<std::int64_t>(now_ms() / 2 + sent / 2);
        connection.clock_offset = std::chrono::milliseconds(static_cast<std::int64_t>(remote_time) - midpoint);
    }

    void handle_offline(std::uint8_t message_id, const Endpoint& from, ByteView data);
    void handle_request_2(const Endpoint& from, ByteView data);
    void on_message(Connection& connection, detail::Delivered& message);
    void drain(Connection& connection);
    void advance_connection(Connection& connection);
};

// --- offline messages --------------------------------------------------------------------------------

void RakNetEngine::Impl::handle_offline(std::uint8_t message_id, const Endpoint& from, ByteView data)
{
    switch (message_id)
    {
    case id::unconnected_ping:
    case id::unconnected_ping_open_connections:
    {
        wire::UnconnectedPing ping;
        if (!wire::decode(data, ping))
            return;
        if (ping.open_connections_only && incoming_count() >= config.max_incoming_connections)
            return;
        wire::UnconnectedPong pong;
        pong.time = ping.time;
        pong.guid = config.guid;
        pong.data = config.offline_ping_response;
        transmit(from, wire::encode(pong));
        return;
    }
    case id::unconnected_pong:
    case id::out_of_band_internal:
        events.push_back(transport::Unconnected{from, Bytes(data.begin(), data.end())});
        return;

    case id::open_connection_request_1:
    {
        wire::OpenConnectionRequest1 request;
        if (!wire::decode(data, request))
            return;
        if (request.protocol != wire::protocol_version)
        {
            wire::IncompatibleProtocolVersion reply;
            reply.guid = config.guid;
            transmit(from, wire::encode(reply));
            return;
        }
        // Stateless on purpose: nothing is allocated until the peer proved it receives our replies.
        wire::OpenConnectionReply1 reply;
        reply.guid = config.guid;
        reply.mtu = std::min<std::uint16_t>(request.mtu, wire::maximum_mtu);
        transmit(from, wire::encode(reply), true);
        return;
    }
    case id::open_connection_request_2:
        handle_request_2(from, data);
        return;
    default:
        break;
    }

    // Everything else answers a connection attempt of ours.
    Connection* connection = find(from);
    if (connection == nullptr || connection->state != State::requesting)
        return;

    switch (message_id)
    {
    case id::open_connection_reply_1:
    {
        wire::OpenConnectionReply1 reply;
        if (!wire::decode(data, reply))
            return;
        if (reply.has_security)
        {
            fail(*connection, ConnectFailure::unsupported);
            return;
        }
        connection->remote_guid = reply.guid;
        connection->mtu = clamp_mtu(reply.mtu);
        connection->have_reply_1 = true;
        connection->next_attempt = TimePoint::min(); // continue with request 2 right away
        return;
    }
    case id::open_connection_reply_2:
    {
        wire::OpenConnectionReply2 reply;
        if (!wire::decode(data, reply))
            return;
        if (reply.requires_security)
        {
            fail(*connection, ConnectFailure::unsupported);
            return;
        }
        connection->remote_guid = reply.guid;
        start_session(*connection, reply.mtu);
        connection->state = State::awaiting_accept;

        wire::ConnectionRequest request;
        request.guid = config.guid;
        request.time = now_ms();
        request.password = connection->password;
        send_internal(*connection, wire::encode(request), Reliability::reliable, Priority::immediate);
        return;
    }
    case id::incompatible_protocol_version:
        fail(*connection, ConnectFailure::incompatible_protocol);
        return;
    case id::already_connected:
        fail(*connection, ConnectFailure::already_connected);
        return;
    case id::connection_banned:
        fail(*connection, ConnectFailure::banned);
        return;
    case id::ip_recently_connected:
        fail(*connection, ConnectFailure::ip_recently_connected);
        return;
    case id::no_free_incoming_connections:
    case id::connection_attempt_failed:
        fail(*connection, ConnectFailure::connection_refused);
        return;
    default:
        return;
    }
}

void RakNetEngine::Impl::handle_request_2(const Endpoint& from, ByteView data)
{
    wire::OpenConnectionRequest2 request;
    if (!wire::decode(data, request))
        return;

    const auto reply_2 = [&](const Connection& connection) {
        wire::OpenConnectionReply2 reply;
        reply.guid = config.guid;
        reply.client_address = from;
        reply.mtu = connection.mtu;
        transmit(from, wire::encode(reply));
    };
    const auto reject = [&](std::uint8_t reason) {
        wire::Rejection rejection;
        rejection.id = reason;
        rejection.guid = config.guid;
        transmit(from, wire::encode(rejection));
    };

    if (Connection* existing = find(from))
    {
        if (existing->state == State::closing)
            by_endpoint.erase(from); // a finished connection must not block a reconnect
        else if (existing->incoming && existing->remote_guid == request.guid &&
                 (existing->state == State::unverified || existing->state == State::handling_request))
        {
            reply_2(*existing); // our reply was lost; the peer is asking again
            return;
        }
        else
        {
            reject(id::already_connected);
            return;
        }
    }
    for (const auto& entry : by_endpoint)
    {
        const Connection& other = *entry.second;
        if (other.remote_guid == request.guid && other.state != State::dead && other.state != State::closing)
        {
            reject(id::already_connected);
            return;
        }
    }
    if (incoming_count() >= config.max_incoming_connections || by_endpoint.size() >= config.max_connections)
    {
        reject(id::no_free_incoming_connections);
        return;
    }
    if (connected_recently(from.address()))
    {
        reject(id::ip_recently_connected);
        return;
    }

    Connection& connection = create(from, true);
    connection.remote_guid = request.guid;
    connection.state = State::unverified;
    start_session(connection, request.mtu);
    reply_2(connection);
}

// --- connected messages ------------------------------------------------------------------------------

void RakNetEngine::Impl::on_message(Connection& connection, detail::Delivered& message)
{
    if (message.data.empty())
        return;
    const std::uint8_t message_id = message.data[0];
    const ByteView payload(message.data);

    const auto answer_ping = [&] {
        wire::ConnectedPing ping;
        if (!wire::decode(payload, ping))
            return;
        wire::ConnectedPong pong;
        pong.ping_time = ping.time;
        pong.pong_time = now_ms();
        send_internal(connection, wire::encode(pong), Reliability::unreliable, Priority::immediate);
    };

    switch (connection.state)
    {
    case State::unverified:
    {
        // Until the peer has presented its connection request, nothing else is acceptable.
        wire::ConnectionRequest request;
        if (message_id != id::connection_request || !wire::decode(payload, request))
        {
            kill(connection);
            return;
        }
        if (request.password != config.incoming_password)
        {
            send_internal(connection, wire::encode_invalid_password(config.guid), Reliability::reliable,
                          Priority::immediate);
            begin_close(connection);
            return;
        }
        wire::ConnectionRequestAccepted accepted;
        accepted.client_address = connection.remote;
        accepted.request_time = request.time;
        accepted.accept_time = now_ms();
        send_internal(connection, wire::encode(accepted), Reliability::reliable_ordered, Priority::immediate);
        connection.state = State::handling_request;
        return;
    }
    case State::handling_request:
        if (message_id == id::new_incoming_connection)
        {
            wire::NewIncomingConnection incoming;
            if (wire::decode(payload, incoming))
            {
                record_clock(connection, incoming.accept_time, incoming.time);
                learn_external_address(incoming.server_address);
            }
            announce(connection);
        }
        else if (message_id == id::connected_ping)
            answer_ping();
        else if (message_id == id::disconnection_notification)
            begin_close(connection);
        else if (!is_reserved(message_id) && connection.early.size() < maximum_early_messages)
            // The peer considers itself connected already and may send before we saw its final
            // handshake message; hold such messages back until the Connected event is out.
            connection.early.push_back(
                transport::Message{connection.id, message.reliability, message.channel, std::move(message.data)});
        return;

    case State::awaiting_accept:
        if (message_id == id::connection_request_accepted)
        {
            wire::ConnectionRequestAccepted accepted;
            if (!wire::decode(payload, accepted))
                return;
            record_clock(connection, accepted.request_time, accepted.accept_time);
            learn_external_address(accepted.client_address);

            wire::NewIncomingConnection reply;
            reply.server_address = connection.remote;
            reply.accept_time = accepted.accept_time;
            reply.time = now_ms();
            send_internal(connection, wire::encode(reply), Reliability::reliable_ordered, Priority::immediate);
            announce(connection);
        }
        else if (message_id == id::invalid_password)
            fail(connection, ConnectFailure::invalid_credentials, true);
        else if (message_id == id::disconnection_notification)
            fail(connection, ConnectFailure::connection_refused, true);
        return;

    case State::connected:
        switch (message_id)
        {
        case id::connected_ping:
            answer_ping();
            return;
        case id::connected_pong:
        {
            wire::ConnectedPong pong;
            if (wire::decode(payload, pong))
                record_clock(connection, pong.ping_time, pong.pong_time);
            return;
        }
        case id::disconnection_notification:
            events.push_back(
                transport::Disconnected{connection.id, connection.remote, DisconnectReason::closed_by_peer});
            begin_close(connection);
            return;
        case id::detect_lost_connections:
        case id::connection_request:
        case id::connection_request_accepted:
        case id::new_incoming_connection:
        case id::invalid_password:
            return; // stale handshake traffic
        default:
            ++connection.messages_received;
            events.push_back(
                transport::Message{connection.id, message.reliability, message.channel, std::move(message.data)});
            return;
        }

    case State::requesting:
    case State::closing:
    case State::dead:
        return;
    }
}

void RakNetEngine::Impl::drain(Connection& connection)
{
    detail::Delivered message;
    while (connection.session->poll_delivered(message))
        on_message(connection, message);

    detail::Receipt receipt;
    while (connection.session->poll_receipt(receipt))
        if (connection.state == State::connected)
            events.push_back(transport::SendReceipt{connection.id, receipt.token, receipt.delivered});
}

// --- timers ------------------------------------------------------------------------------------------

void RakNetEngine::Impl::advance_connection(Connection& connection)
{
    if (connection.state == State::dead)
        return;

    if (connection.state == State::requesting)
    {
        if (now < connection.next_attempt)
            return;
        if (connection.attempts >= config.connect_attempts)
        {
            fail(connection, ConnectFailure::no_response);
            return;
        }
        if (connection.have_reply_1)
        {
            wire::OpenConnectionRequest2 request;
            request.server_address = connection.remote;
            request.mtu = connection.mtu;
            request.guid = config.guid;
            transmit(connection.remote, wire::encode(request));
        }
        else
        {
            // Walk down the MTU ladder: the request is padded to the size it probes and must not be
            // fragmented, so only a path that carries that size delivers it.
            const std::size_t sizes = config.mtu_sizes.size();
            const std::size_t per_size = std::max<std::size_t>(1, config.connect_attempts / sizes);
            const std::size_t index = std::min(connection.attempts / per_size, sizes - 1);
            wire::OpenConnectionRequest1 request;
            request.mtu = config.mtu_sizes[index];
            transmit(connection.remote, wire::encode(request), true);
        }
        ++connection.attempts;
        connection.next_attempt = now + config.connect_attempt_interval;
        return;
    }

    Session& session = *connection.session;
    const bool half_open = connection.state == State::awaiting_accept || connection.state == State::unverified ||
                           connection.state == State::handling_request;
    if (half_open && now - connection.created > config.handshake_timeout)
    {
        if (connection.incoming)
            kill(connection);
        else
            fail(connection, ConnectFailure::no_response);
        return;
    }

    // Keep-alive: an idle connection gets a reliable ping, so that a dead peer shows up as
    // unacknowledged data and trips the session's time-out.
    if (connection.state == State::connected && !session.has_unacked() &&
        now - session.last_reliable_send() >= config.timeout / 2)
    {
        wire::ConnectedPing ping;
        ping.time = now_ms();
        send_internal(connection, wire::encode(ping), Reliability::reliable, Priority::immediate);
    }

    scratch.clear();
    session.advance(now, scratch);
    for (Bytes& datagram : scratch)
        transmit(connection.remote, std::move(datagram));
    drain(connection);
    if (connection.state == State::dead)
        return;

    if (session.timed_out() || session.violated())
    {
        if (connection.announced && connection.state == State::connected)
            events.push_back(transport::Disconnected{connection.id, connection.remote,
                                                     session.timed_out() ? DisconnectReason::timed_out
                                                                         : DisconnectReason::protocol_violation});
        else if (!connection.incoming && connection.state == State::awaiting_accept)
            events.push_back(transport::ConnectFailed{connection.id, connection.remote, ConnectFailure::no_response});
        kill(connection);
    }
    else if (connection.state == State::closing &&
             ((session.flushed() && !session.acks_pending()) || now >= connection.close_deadline))
        kill(connection);
}

// --- RakNetEngine ------------------------------------------------------------------------------------

RakNetEngine::RakNetEngine(Config config) : impl_(std::make_unique<Impl>())
{
    if (config.guid == 0)
        config.guid = random_guid();
    if (config.mtu_sizes.empty())
        config.mtu_sizes = Config().mtu_sizes;
    for (std::uint16_t& mtu : config.mtu_sizes)
        mtu = clamp_mtu(mtu);
    config.connect_attempts = std::max(config.connect_attempts, 1u);
    impl_->config = std::move(config);
}

RakNetEngine::~RakNetEngine() = default;

std::uint64_t RakNetEngine::wire_time(TimePoint time) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}

void RakNetEngine::handle_datagram(const Endpoint& from, ByteView data, TimePoint now)
{
    Impl& impl = *impl_;
    impl.now = now;
    if (data.size() <= 2) // RakNet ignores these too; too short for any message
        return;
    if (impl.refuse_if_banned(from))
        return;

    if (const std::optional<std::uint8_t> offline = wire::offline_message_id(data))
    {
        impl.handle_offline(*offline, from, data);
        impl.sweep();
        return;
    }

    Connection* connection = impl.find(from);
    if (connection == nullptr || !connection->session)
        return;
    connection->session->handle_datagram(data, now);
    impl.drain(*connection);
    impl.sweep();
}

void RakNetEngine::advance(TimePoint now)
{
    Impl& impl = *impl_;
    impl.now = now;

    for (const auto& pending : impl.pending_pings)
    {
        wire::UnconnectedPing ping;
        ping.time = impl.now_ms();
        ping.guid = impl.config.guid;
        ping.open_connections_only = pending.second;
        impl.transmit(pending.first, wire::encode(ping));
    }
    impl.pending_pings.clear();

    std::vector<Connection*> connections;
    connections.reserve(impl.by_endpoint.size());
    for (const auto& entry : impl.by_endpoint)
        connections.push_back(entry.second.get());
    for (Connection* connection : connections)
        impl.advance_connection(*connection);
    impl.sweep();
}

bool RakNetEngine::poll_transmit(transport::Transmit& out)
{
    return pop(impl_->transmits, out);
}

bool RakNetEngine::poll_event(transport::Event& out)
{
    return pop(impl_->events, out);
}

std::optional<TimePoint> RakNetEngine::next_deadline() const
{
    const Impl& impl = *impl_;
    std::optional<TimePoint> deadline;
    const auto consider = [&deadline](TimePoint t) {
        if (!deadline || t < *deadline)
            deadline = t;
    };

    if (!impl.pending_pings.empty())
        consider(TimePoint::min());
    for (const auto& entry : impl.by_endpoint)
    {
        const Connection& connection = *entry.second;
        if (connection.state == State::dead)
            continue;
        if (connection.state == State::requesting)
        {
            consider(connection.next_attempt);
            continue;
        }
        const Session& session = *connection.session;
        if (const std::optional<TimePoint> next = session.next_deadline())
            consider(*next);
        if (connection.state == State::connected)
        {
            if (!session.has_unacked())
                consider(session.last_reliable_send() + impl.config.timeout / 2);
        }
        else if (connection.state == State::closing)
            consider(session.flushed() && !session.acks_pending() ? TimePoint::min() : connection.close_deadline);
        else
            consider(connection.created + impl.config.handshake_timeout);
    }
    return deadline;
}

ConnectionId RakNetEngine::connect(const Endpoint& to, ByteView password, std::error_code& ec)
{
    Impl& impl = *impl_;
    if (to.port() == 0 || to.address().is_unspecified())
    {
        ec = make_error_code(Errc::invalid_argument);
        return transport::invalid_connection;
    }
    if (Connection* existing = impl.find(to))
    {
        if (existing->state != State::closing)
        {
            ec = make_error_code(Errc::already_connected);
            return transport::invalid_connection;
        }
        // The previous connection to this peer is still saying goodbye. Give its last datagrams
        // (the disconnect notification) one chance to leave, then make room for the new attempt.
        impl.scratch.clear();
        existing->session->advance(impl.now, impl.scratch);
        for (Bytes& datagram : impl.scratch)
            impl.transmit(to, std::move(datagram));
        impl.by_endpoint.erase(to);
    }
    if (impl.by_endpoint.size() >= impl.config.max_connections)
    {
        ec = make_error_code(Errc::connection_limit_reached);
        return transport::invalid_connection;
    }

    ec.clear();
    Connection& connection = impl.create(to, false);
    connection.password.assign(password.begin(), password.end());
    return connection.id;
}

void RakNetEngine::disconnect(ConnectionId id, transport::DisconnectMode mode)
{
    Impl& impl = *impl_;
    Connection* connection = impl.find(id);
    if (connection == nullptr)
        return;

    impl.events.push_back(
        transport::Disconnected{connection->id, connection->remote, DisconnectReason::closed_locally});
    if (mode == transport::DisconnectMode::graceful && connection->state == State::connected)
    {
        // Low priority, so that everything queued before it is sent first.
        impl.send_internal(*connection, Bytes{id::disconnection_notification}, Reliability::reliable_ordered,
                           Priority::low);
        impl.begin_close(*connection);
    }
    else
        impl.kill(*connection);
}

void RakNetEngine::send(ConnectionId id, ByteView data, const transport::SendOptions& options,
                        std::error_code& ec)
{
    Impl& impl = *impl_;
    Connection* connection = impl.find(id);
    if (connection == nullptr)
        ec = make_error_code(Errc::unknown_connection);
    else if (connection->state != State::connected)
        ec = make_error_code(Errc::not_connected);
    else if (data.empty())
        ec = make_error_code(Errc::invalid_argument);
    else if (is_reserved(data[0]))
        ec = make_error_code(Errc::reserved_message_id);
    else if (options.receipt_token != 0 && (options.reliability == Reliability::unreliable_sequenced ||
                                            options.reliability == Reliability::reliable_sequenced))
        ec = make_error_code(Errc::invalid_argument); // RakNet has no sequenced + receipt combination
    else
    {
        ec = connection->session->send(data, options, impl.now);
        if (!ec)
            ++connection->messages_sent;
    }
}

std::uint64_t RakNetEngine::guid() const noexcept
{
    return impl_->config.guid;
}

std::size_t RakNetEngine::connection_count() const noexcept
{
    return impl_->by_id.size();
}

std::optional<ConnectionInfo> RakNetEngine::connection_info(ConnectionId id) const
{
    const Connection* connection = impl_->find(id);
    if (connection == nullptr)
        return std::nullopt;
    ConnectionInfo info;
    info.remote = connection->remote;
    info.remote_guid = connection->remote_guid;
    info.mtu = connection->mtu;
    info.incoming = connection->incoming;
    info.clock_offset = connection->clock_offset;
    if (connection->session)
        info.rtt = connection->session->rtt();
    return info;
}

std::optional<ConnectionStats> RakNetEngine::connection_stats(ConnectionId id) const
{
    const Connection* connection = impl_->find(id);
    if (connection == nullptr)
        return std::nullopt;
    ConnectionStats stats;
    stats.messages_sent = connection->messages_sent;
    stats.messages_received = connection->messages_received;
    if (const Session* session = connection->session.get())
    {
        const detail::SessionCounters& counters = session->counters();
        stats.bytes_sent = counters.bytes_sent;
        stats.bytes_received = counters.bytes_received;
        stats.datagrams_resent = counters.datagrams_resent;
        stats.datagrams_lost = counters.datagrams_lost;
        stats.queued_messages = session->queued_messages();
        stats.queued_bytes = session->queued_bytes();
    }
    return stats;
}

void RakNetEngine::set_max_incoming_connections(std::size_t count)
{
    impl_->config.max_incoming_connections = count;
}

void RakNetEngine::set_incoming_password(ByteView password)
{
    impl_->config.incoming_password.assign(password.begin(), password.end());
}

void RakNetEngine::set_offline_ping_response(ByteView data)
{
    impl_->config.offline_ping_response.assign(data.begin(), data.end());
}

bool RakNetEngine::ban(std::string_view address, std::chrono::milliseconds duration, TimePoint now)
{
    if (address.empty() || address.size() > maximum_ban_length)
        return false;
    std::optional<TimePoint> expiry;
    if (duration.count() > 0)
        expiry = now + duration;
    std::vector<Ban>& bans = impl_->bans;
    const auto existing =
        std::find_if(bans.begin(), bans.end(), [&](const Ban& ban) { return ban.address == address; });
    if (existing != bans.end())
        existing->expiry = expiry;
    else
        bans.push_back(Ban{std::string(address), expiry});
    return true;
}

void RakNetEngine::unban(std::string_view address)
{
    std::vector<Ban>& bans = impl_->bans;
    bans.erase(std::remove_if(bans.begin(), bans.end(), [&](const Ban& ban) { return ban.address == address; }),
               bans.end());
}

void RakNetEngine::clear_bans()
{
    impl_->bans.clear();
}

bool RakNetEngine::is_banned(std::string_view address, TimePoint now) const
{
    return impl_->is_banned(address, now);
}

void RakNetEngine::set_limit_ip_connection_frequency(bool limit)
{
    impl_->config.limit_ip_connection_frequency = limit;
}

void RakNetEngine::ping(const Endpoint& to, bool only_if_accepting)
{
    impl_->pending_pings.emplace_back(to, only_if_accepting);
}

void RakNetEngine::advertise(const Endpoint& to, ByteView data, std::error_code& ec)
{
    if (data.size() > maximum_out_of_band_size)
    {
        ec = make_error_code(Errc::message_too_large);
        return;
    }
    ec.clear();
    wire::OutOfBand message;
    message.guid = impl_->config.guid;
    message.data.reserve(data.size() + 1);
    message.data.push_back(id::advertise_system);
    message.data.insert(message.data.end(), data.begin(), data.end());
    impl_->transmit(to, wire::encode(message));
}

} // namespace raknet
} // namespace pzynet
