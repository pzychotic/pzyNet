#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/transport/udp_host.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace pzynet
{
namespace raknet
{
namespace
{

std::uint8_t failure_id(transport::ConnectFailure reason) noexcept
{
    using transport::ConnectFailure;
    switch (reason)
    {
    case ConnectFailure::no_response: return id::connection_attempt_failed;
    case ConnectFailure::incompatible_protocol: return id::incompatible_protocol_version;
    case ConnectFailure::connection_refused: return id::no_free_incoming_connections;
    case ConnectFailure::already_connected: return id::already_connected;
    case ConnectFailure::banned: return id::connection_banned;
    case ConnectFailure::invalid_credentials: return id::invalid_password;
    case ConnectFailure::unsupported: return id::remote_system_requires_public_key;
    }
    return id::connection_attempt_failed;
}

} // namespace

struct Peer::Impl
{
    struct RemoteSystem
    {
        Guid guid;
        Endpoint address;
    };

    explicit Impl(PeerConfig config_)
        : config(std::move(config_)), engine(config.engine), host(engine, config.clock)
    {
    }

    PeerConfig config;
    RakNetEngine engine;
    transport::UdpHost host;

    std::mutex mutex; // guards everything below
    std::map<transport::ConnectionId, RemoteSystem> connections;
    std::uint32_t next_receipt = 1;

    /// Requires `mutex`.
    transport::ConnectionId find(const Remote& remote) const
    {
        for (const auto& entry : connections)
        {
            const bool match = std::holds_alternative<Guid>(remote)
                                   ? entry.second.guid == std::get<Guid>(remote)
                                   : entry.second.address == std::get<Endpoint>(remote);
            if (match)
                return entry.first;
        }
        return transport::invalid_connection;
    }

    std::optional<ConnectionInfo> info(const Remote& remote)
    {
        transport::ConnectionId connection = transport::invalid_connection;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connection = find(remote);
        }
        if (connection == transport::invalid_connection)
            return std::nullopt;
        return host.with_engine([&](transport::Engine&) { return engine.connection_info(connection); });
    }

    Packet make_packet(std::uint8_t message_id, const RemoteSystem& from) const
    {
        Packet packet;
        packet.id = message_id;
        packet.guid = from.guid;
        packet.address = from.address;
        packet.data = {message_id};
        packet.byte_order = config.byte_order;
        return packet;
    }

    std::uint64_t now() const { return RakNetEngine::wire_time(config.clock ? config.clock() : Clock::now()); }

    /// Shifts a timestamped message's time from the sender's clock into ours, in place, as RakPeer's
    /// ShiftIncomingTimestamp does (unsigned wrap-around; no measured offset means no shift).
    void to_local_time(Packet& packet, transport::ConnectionId connection)
    {
        const std::optional<ConnectionInfo> info =
            host.with_engine([&](transport::Engine&) { return engine.connection_info(connection); });
        const std::uint64_t offset =
            info && info->clock_offset ? static_cast<std::uint64_t>(info->clock_offset->count()) : 0;

        const ByteView header = ByteView(packet.data.data(), packet.data.size()).subspan(1, 8);
        BitStreamReader reader(header, config.byte_order);
        const std::uint64_t local = reader.read<std::uint64_t>() - offset;

        BitStreamWriter writer(config.byte_order);
        writer.write(local);
        std::copy(writer.data().begin(), writer.data().end(), packet.data.begin() + 1);
        packet.timestamp = local;
    }

    /// Turns a transport event into a Packet; nullopt for events RakNet does not report.
    std::optional<Packet> translate(transport::Event& event)
    {
        if (const auto* connected = std::get_if<transport::Connected>(&event))
        {
            const std::optional<ConnectionInfo> info = host.with_engine(
                [&](transport::Engine&) { return engine.connection_info(connected->connection); });
            const RemoteSystem remote{Guid{info ? info->remote_guid : 0}, connected->remote};
            std::lock_guard<std::mutex> lock(mutex);
            connections[connected->connection] = remote;
            return make_packet(connected->incoming ? id::new_incoming_connection : id::connection_request_accepted,
                               remote);
        }
        if (const auto* failed = std::get_if<transport::ConnectFailed>(&event))
            return make_packet(failure_id(failed->reason), RemoteSystem{Guid{}, failed->remote});
        if (const auto* disconnected = std::get_if<transport::Disconnected>(&event))
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = connections.find(disconnected->connection);
            if (it == connections.end()) // closed by this Peer, which RakNet does not report
                return std::nullopt;
            const RemoteSystem remote = it->second;
            connections.erase(it);
            if (disconnected->reason == transport::DisconnectReason::closed_locally)
                return std::nullopt;
            return make_packet(disconnected->reason == transport::DisconnectReason::closed_by_peer
                                   ? id::disconnection_notification
                                   : id::connection_lost,
                               remote);
        }
        if (auto* message = std::get_if<transport::Message>(&event))
        {
            if (message->data.empty())
                return std::nullopt;
            RemoteSystem remote;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = connections.find(message->connection);
                if (it != connections.end())
                    remote = it->second;
            }
            Packet packet = make_packet(message->data[0], remote);
            packet.data = std::move(message->data);
            if (packet.id == id::timestamp && packet.data.size() >= Packet::timestamp_header_size)
                to_local_time(packet, message->connection);
            return packet;
        }
        return std::nullopt; // receipts and offline datagrams are not reported yet
    }
};

Peer::Peer() noexcept = default;

Peer::~Peer()
{
    shutdown();
}

void Peer::start(PeerConfig config, std::error_code& ec)
{
    ec.clear();
    if (impl_)
    {
        ec = make_error_code(Errc::already_open);
        return;
    }
    const Threading threading = config.threading;
    const Endpoint bind = config.bind;
    auto impl = std::make_unique<Impl>(std::move(config));
    impl->host.bind(bind, ec);
    if (!ec && threading == Threading::background)
        impl->host.start_thread(ec);
    if (!ec)
        impl_ = std::move(impl);
}

void Peer::shutdown() noexcept
{
    impl_.reset(); // the host stops its thread and closes the socket; the engine goes silently
}

Guid Peer::guid() const
{
    if (!impl_)
        return Guid{};
    return impl_->host.with_engine([this](transport::Engine&) { return Guid{impl_->engine.guid()}; });
}

Endpoint Peer::local_endpoint() const
{
    return impl_ ? impl_->host.local_endpoint() : Endpoint();
}

std::uint64_t Peer::time() const
{
    return impl_ ? impl_->now() : RakNetEngine::wire_time(Clock::now());
}

void Peer::connect(const Endpoint& to, ByteView password, std::error_code& ec)
{
    ec.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    impl_->host.connect(to, password, ec);
}

void Peer::disconnect(const Remote& remote, transport::DisconnectMode mode)
{
    if (!impl_)
        return;
    transport::ConnectionId connection = transport::invalid_connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        connection = impl_->find(remote);
        impl_->connections.erase(connection);
    }
    if (connection != transport::invalid_connection)
        impl_->host.disconnect(connection, mode);
}

std::uint32_t Peer::send(const Remote& to, ByteView data, const SendOptions& options, std::error_code& ec)
{
    ec.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::not_open);
        return 0;
    }
    transport::ConnectionId connection = transport::invalid_connection;
    std::uint32_t receipt = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        connection = impl_->find(to);
        if (connection != transport::invalid_connection)
        {
            receipt = impl_->next_receipt++;
            if (impl_->next_receipt == 0)
                impl_->next_receipt = 1;
        }
    }
    if (connection == transport::invalid_connection)
    {
        ec = make_error_code(Errc::not_connected);
        return 0;
    }
    transport::SendOptions transport_options;
    transport_options.reliability = options.reliability;
    transport_options.channel = options.channel;
    transport_options.priority = options.priority;
    impl_->host.send(connection, data, transport_options, ec);
    return ec ? 0 : receipt;
}

ConnectionStats Peer::statistics(const Remote& remote, std::error_code& ec) const
{
    ec.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::not_open);
        return ConnectionStats();
    }
    transport::ConnectionId connection = transport::invalid_connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        connection = impl_->find(remote);
    }
    const std::optional<ConnectionStats> stats =
        connection == transport::invalid_connection
            ? std::nullopt
            : impl_->host.with_engine([&](transport::Engine&) { return impl_->engine.connection_stats(connection); });
    if (!stats)
    {
        ec = make_error_code(Errc::not_connected);
        return ConnectionStats();
    }
    return *stats;
}

void Peer::set_timeout(const Remote& remote, std::chrono::milliseconds timeout, std::error_code& ec)
{
    ec.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    transport::ConnectionId connection = transport::invalid_connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        connection = impl_->find(remote);
    }
    if (connection == transport::invalid_connection)
    {
        ec = make_error_code(Errc::not_connected);
        return;
    }
    impl_->host.with_engine([&](transport::Engine&) { impl_->engine.set_timeout(connection, timeout, ec); });
    if (ec == Errc::unknown_connection) // gone in the meantime
        ec = make_error_code(Errc::not_connected);
}

void Peer::set_timeout(std::chrono::milliseconds timeout)
{
    if (impl_)
        impl_->host.with_engine([&](transport::Engine&) { impl_->engine.set_timeout(timeout); });
}

std::optional<std::chrono::milliseconds> Peer::timeout(const Remote& remote) const
{
    if (!impl_)
        return std::nullopt;
    const std::optional<ConnectionInfo> info = impl_->info(remote);
    if (!info)
        return std::nullopt;
    return info->timeout;
}

std::optional<Endpoint> Peer::external_address(const Remote& remote) const
{
    if (!impl_)
        return std::nullopt;
    const std::optional<ConnectionInfo> info = impl_->info(remote);
    if (!info)
        return std::nullopt;
    return info->external_address;
}

std::optional<Packet> Peer::receive()
{
    if (!impl_)
        return std::nullopt;
    transport::Event event;
    while (impl_->host.poll_event(event))
        if (std::optional<Packet> packet = impl_->translate(event))
            return packet;
    return std::nullopt;
}

void Peer::update(std::chrono::milliseconds max_wait, std::error_code& ec)
{
    ec.clear();
    if (!impl_)
    {
        ec = make_error_code(Errc::not_open);
        return;
    }
    impl_->host.update(max_wait, ec);
}

} // namespace raknet
} // namespace pzynet
