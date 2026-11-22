// The Peer through its public API: Peers talking over loopback UDP, in both threading modes.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace pzynet::raknet;
using namespace std::chrono_literals;

namespace
{

using SteadyClock = std::chrono::steady_clock;

PeerConfig loopback_config(Threading threading, std::size_t max_incoming = 0)
{
    PeerConfig config;
    config.bind = Endpoint(IpAddress::loopback_v4(), 0);
    config.engine.max_incoming_connections = max_incoming;
    config.threading = threading;
    return config;
}

/// Peers under test. Waiting runs the network of every app-driven Peer that is not paused, so tests
/// read the same in both threading modes.
class Network
{
public:
    Peer& add(PeerConfig config)
    {
        peers_.push_back(std::make_unique<Peer>());
        peers_.back()->start(std::move(config));
        return *peers_.back();
    }

    void destroy(Peer& peer)
    {
        peers_.erase(std::find_if(peers_.begin(), peers_.end(), [&](const auto& p) { return p.get() == &peer; }));
    }

    /// Stops running the network of an app-driven Peer, so it falls silent.
    void pause(Peer& peer) { paused_.push_back(&peer); }

    void pump()
    {
        bool pumped = false;
        for (const auto& peer : peers_)
        {
            if (!peer->running() || std::find(paused_.begin(), paused_.end(), peer.get()) != paused_.end())
                continue;
            std::error_code ec;
            peer->update(1ms, ec); // Errc::running for background Peers
            pumped = pumped || !ec;
        }
        if (!pumped)
            std::this_thread::sleep_for(1ms);
    }

    /// The next Packet, or nullopt if none arrives within `limit`.
    std::optional<Packet> wait_for_packet(Peer& peer, std::chrono::milliseconds limit = 5s)
    {
        const auto give_up = SteadyClock::now() + limit;
        for (;;)
        {
            if (std::optional<Packet> packet = peer.receive())
                return packet;
            if (SteadyClock::now() >= give_up)
                return std::nullopt;
            pump();
        }
    }

    /// Adds a server and a client Peer and connects them; the connection Packets are consumed.
    std::pair<Peer&, Peer&> connected_pair(PeerConfig server_config, PeerConfig client_config)
    {
        Peer& server = add(std::move(server_config));
        Peer& client = add(std::move(client_config));
        client.connect(server.local_endpoint());
        REQUIRE(wait_for_packet(client));
        REQUIRE(wait_for_packet(server));
        return {server, client};
    }

    std::pair<Peer&, Peer&> connected_pair(Threading threading)
    {
        return connected_pair(loopback_config(threading, 4), loopback_config(threading));
    }

private:
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<Peer*> paused_;
};

} // namespace

TEST_CASE("Peer: connecting yields an accepted Packet on one side and a new incoming one on the other",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 4));
    Peer& client = net.add(loopback_config(threading));

    client.connect(server.local_endpoint());

    const std::optional<Packet> accepted = net.wait_for_packet(client);
    REQUIRE(accepted);
    CHECK(accepted->id == id::connection_request_accepted);
    CHECK(accepted->guid == server.guid());
    CHECK(accepted->address == server.local_endpoint());

    const std::optional<Packet> incoming = net.wait_for_packet(server);
    REQUIRE(incoming);
    CHECK(incoming->id == id::new_incoming_connection);
    CHECK(incoming->guid == client.guid());
    CHECK(incoming->address == client.local_endpoint());
}

TEST_CASE("Peer: a BitStream message arrives as a Packet whose reader starts after the message id",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);

    BitStreamWriter message;
    message.write(id::user_packet_enum);
    message.write(true);
    message.write(std::uint32_t{0xDEADBEEF});
    message.write(false);
    message.write(std::int16_t{-1234});
    CHECK(client.send(server.guid(), message) != 0);

    const std::optional<Packet> packet = net.wait_for_packet(server);
    REQUIRE(packet);
    CHECK(packet->id == id::user_packet_enum);
    CHECK(packet->guid == client.guid());
    CHECK(packet->address == client.local_endpoint());
    CHECK(packet->data == std::vector<std::uint8_t>(message.data().begin(), message.data().end()));
    CHECK_FALSE(packet->timestamp);

    BitStreamReader reader = packet->reader();
    CHECK(reader.read<bool>() == true);
    CHECK(reader.read<std::uint32_t>() == 0xDEADBEEF);
    CHECK(reader.read<bool>() == false);
    CHECK(reader.read<std::int16_t>() == -1234);
    CHECK(reader.ok());
}

TEST_CASE("Peer: a remote close arrives as a disconnection notification; a local close yields nothing",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);

    client.disconnect(server.guid());

    const std::optional<Packet> packet = net.wait_for_packet(server);
    REQUIRE(packet);
    CHECK(packet->id == id::disconnection_notification);
    CHECK(packet->guid == client.guid());
    CHECK(packet->address == client.local_endpoint());

    CHECK_FALSE(net.wait_for_packet(client, 300ms));
    std::error_code ec;
    client.send(server.guid(), std::vector<std::uint8_t>{id::user_packet_enum}, SendOptions(), ec);
    CHECK(ec == Errc::not_connected);
}

TEST_CASE("Peer: an app-driven Peer runs its network only when updated", "[rakpeer]")
{
    Network net;
    Peer& server = net.add(loopback_config(Threading::app_driven, 4));
    Peer& client = net.add(loopback_config(Threading::app_driven));
    net.pause(server);

    client.connect(server.local_endpoint());
    CHECK_FALSE(net.wait_for_packet(client, 300ms)); // the server never answers

    std::error_code ec;
    server.update(0ms, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("Peer: update is refused while the background thread runs the network", "[rakpeer]")
{
    Peer peer;
    peer.start(loopback_config(Threading::background));
    std::error_code ec;
    peer.update(0ms, ec);
    CHECK(ec == Errc::running);
}

TEST_CASE("Peer: shutdown, and destruction, stop the network and release the socket", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    const bool destroy = GENERATE(false, true);
    auto peer = std::make_unique<Peer>();
    peer->start(loopback_config(threading));
    const Endpoint bound = peer->local_endpoint();
    REQUIRE(peer->running());

    if (destroy)
        peer.reset();
    else
    {
        peer->shutdown();
        CHECK_FALSE(peer->running());
        std::error_code ec;
        peer->connect(Endpoint(IpAddress::loopback_v4(), 1), ByteView(), ec);
        CHECK(ec == Errc::not_open);
    }

    UdpSocket socket;
    std::error_code ec;
    socket.open(IpAddress::Family::v4, ec);
    REQUIRE_FALSE(ec);
    socket.bind(bound, ec); // fails while another socket holds the port
    CHECK_FALSE(ec);
}

TEST_CASE("Peer: after shutdown with a linger, the remote receives every reliable message queued before it, "
          "then a disconnection notification",
          "[rakpeer]")
{
    // The remote runs on its own thread, so that it keeps working while shutdown() blocks.
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] =
        net.connected_pair(loopback_config(Threading::background, 4), loopback_config(threading));

    constexpr int message_count = 200;
    for (int i = 0; i < message_count; ++i)
    {
        std::vector<std::uint8_t> message(600, 0xAB);
        message[0] = id::user_packet_enum;
        message[1] = static_cast<std::uint8_t>(i);
        message[2] = static_cast<std::uint8_t>(i >> 8);
        client.send(server.guid(), message);
    }
    const Guid client_guid = client.guid();

    const auto started = SteadyClock::now();
    client.shutdown(5000ms);
    CHECK(SteadyClock::now() - started < 4s); // returns once the remote acknowledged everything
    CHECK_FALSE(client.running());

    for (int i = 0; i < message_count; ++i)
    {
        const std::optional<Packet> packet = net.wait_for_packet(server);
        REQUIRE(packet);
        REQUIRE(packet->id == id::user_packet_enum);
        REQUIRE(packet->data.size() == 600);
        CHECK(packet->data[1] + (packet->data[2] << 8) == i);
    }
    const std::optional<Packet> notification = net.wait_for_packet(server);
    REQUIRE(notification);
    CHECK(notification->id == id::disconnection_notification);
    CHECK(notification->guid == client_guid);
}

TEST_CASE("Peer: shutdown with zero linger, and destruction, return at once; the remote loses the connection",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    const bool destroy = GENERATE(false, true);
    Network net;
    PeerConfig server_config = loopback_config(Threading::background, 4);
    server_config.engine.timeout = 500ms;
    Peer& server = net.add(server_config);
    Peer& client = net.add(loopback_config(threading));
    client.connect(server.local_endpoint());
    REQUIRE(net.wait_for_packet(client));
    REQUIRE(net.wait_for_packet(server));
    const Guid client_guid = client.guid();

    for (int i = 0; i < 50; ++i)
        client.send(server.guid(), std::vector<std::uint8_t>(600, id::user_packet_enum));

    const auto started = SteadyClock::now();
    if (destroy)
        net.destroy(client);
    else
        client.shutdown(0ms);
    CHECK(SteadyClock::now() - started < 200ms);

    std::optional<Packet> packet;
    do
        packet = net.wait_for_packet(server);
    while (packet && packet->id == id::user_packet_enum); // whatever got out before the shutdown
    REQUIRE(packet);
    CHECK(packet->id == id::connection_lost);
    CHECK(packet->guid == client_guid);
}

TEST_CASE("Peer: shutdown never blocks longer than the linger", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] =
        net.connected_pair(loopback_config(Threading::app_driven, 4), loopback_config(threading));
    net.pause(server); // never acknowledges the disconnection notification

    client.send(server.guid(), std::vector<std::uint8_t>{id::user_packet_enum});
    const auto started = SteadyClock::now();
    client.shutdown(300ms);
    const auto elapsed = SteadyClock::now() - started;
    CHECK(elapsed >= 300ms);
    CHECK(elapsed < 300ms + 150ms);
    CHECK_FALSE(client.running());
}

TEST_CASE("Peer: a shut-down Peer can be started again", "[rakpeer]")
{
    Peer peer;
    peer.start(loopback_config(Threading::background));
    peer.shutdown();
    peer.start(loopback_config(Threading::app_driven));
    CHECK(peer.running());
    std::error_code ec;
    peer.start(loopback_config(Threading::background), ec);
    CHECK(ec == Errc::already_open);
}

namespace
{

/// Signed difference of two RakNet times (which wrap around like RakNet's unsigned arithmetic).
std::int64_t time_difference(std::uint64_t a, std::uint64_t b)
{
    return static_cast<std::int64_t>(a - b);
}

/// The big-endian 8-byte time in a timestamped message's header.
std::uint64_t header_time(const std::vector<std::uint8_t>& data)
{
    std::uint64_t value = 0;
    for (std::size_t i = 1; i < 9; ++i)
        value = (value << 8) | data[i];
    return value;
}

} // namespace

TEST_CASE("Peer: a timestamped message arrives converted into the receiver's clock", "[rakpeer]")
{
    // One side's clock runs an hour ahead, so an unconverted time would be an hour off.
    const bool skew_sender = GENERATE(true, false);
    INFO((skew_sender ? "sender's clock is ahead" : "receiver's clock is ahead"));
    const ClockSource ahead = [] { return Clock::now() + 1h; };

    PeerConfig server_config = loopback_config(4);
    PeerConfig client_config = loopback_config();
    (skew_sender ? client_config : server_config).clock = ahead;
    Peer server;
    server.start(server_config);
    Peer client;
    client.start(client_config);
    client.connect(server.local_endpoint());
    REQUIRE(wait_for_packet(client));
    REQUIRE(wait_for_packet(server));

    const std::int64_t skew = time_difference(client.time(), server.time());
    CHECK(std::abs(skew - (skew_sender ? 3'600'000 : -3'600'000)) <= 50);

    const std::uint64_t sent_at = client.time();
    BitStreamWriter message;
    message.write(id::timestamp);
    message.write(sent_at);
    message.write(id::user_packet_enum);
    message.write(std::uint32_t{0xC0FFEE});
    client.send(server.guid(), message);

    const std::optional<Packet> packet = wait_for_packet(server);
    REQUIRE(packet);
    const std::uint64_t received_at = server.time();
    REQUIRE(packet->timestamp);
    // Sent moments ago in the receiver's clock: within the clock offset's error of loopback round trips.
    CHECK(time_difference(received_at, *packet->timestamp) >= -20);
    CHECK(time_difference(received_at, *packet->timestamp) <= 100);

    CHECK(packet->id == id::timestamp);
    CHECK(packet->guid == client.guid());
    REQUIRE(packet->data.size() == 14);
    CHECK(header_time(packet->data) == *packet->timestamp);
    CHECK(std::vector<std::uint8_t>(packet->data.begin() + 9, packet->data.end()) ==
          std::vector<std::uint8_t>{id::user_packet_enum, 0x00, 0xC0, 0xFF, 0xEE});

    BitStreamReader reader = packet->reader();
    CHECK(reader.read<std::uint8_t>() == id::user_packet_enum);
    CHECK(reader.read<std::uint32_t>() == 0xC0FFEE);
    CHECK(reader.ok());
}

TEST_CASE("Peer: a timestamped message with a truncated header is delivered unmodified", "[rakpeer]")
{
    PeerConfig client_config = loopback_config();
    client_config.clock = [] { return Clock::now() + 1h; }; // any conversion would change the bytes
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(client_config);
    client.connect(server.local_endpoint());
    REQUIRE(wait_for_packet(client));
    REQUIRE(wait_for_packet(server));

    const std::vector<std::uint8_t> truncated{id::timestamp, 1, 2, 3, 4, 5, 6, 7};
    client.send(server.guid(), truncated);

    const std::optional<Packet> packet = wait_for_packet(server);
    REQUIRE(packet);
    CHECK(packet->id == id::timestamp);
    CHECK(packet->data == truncated);
    CHECK_FALSE(packet->timestamp);
    BitStreamReader reader = packet->reader();
    CHECK(reader.read<std::uint8_t>() == 1);
}
