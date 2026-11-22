// The Peer through its public API: two Peers talking over loopback UDP.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

PeerConfig loopback_config(std::size_t max_incoming = 0)
{
    PeerConfig config;
    config.bind = Endpoint(IpAddress::loopback_v4(), 0);
    config.engine.max_incoming_connections = max_incoming;
    return config;
}

/// The next Packet, or nullopt if none arrives within `limit`.
std::optional<Packet> wait_for_packet(Peer& peer, std::chrono::milliseconds limit = 5s)
{
    const auto give_up = std::chrono::steady_clock::now() + limit;
    for (;;)
    {
        if (std::optional<Packet> packet = peer.receive())
            return packet;
        if (std::chrono::steady_clock::now() >= give_up)
            return std::nullopt;
        std::this_thread::sleep_for(1ms);
    }
}

/// A server and a client Peer with an established connection; the connection Packets are consumed.
struct Pair
{
    Peer server;
    Peer client;

    Pair()
    {
        server.start(loopback_config(4));
        client.start(loopback_config());
        client.connect(server.local_endpoint());
        REQUIRE(wait_for_packet(client));
        REQUIRE(wait_for_packet(server));
    }
};

} // namespace

TEST_CASE("Peer: connecting yields an accepted Packet on one side and a new incoming one on the other",
          "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());

    client.connect(server.local_endpoint());

    const std::optional<Packet> accepted = wait_for_packet(client);
    REQUIRE(accepted);
    CHECK(accepted->id == id::connection_request_accepted);
    CHECK(accepted->guid == server.guid());
    CHECK(accepted->address == server.local_endpoint());

    const std::optional<Packet> incoming = wait_for_packet(server);
    REQUIRE(incoming);
    CHECK(incoming->id == id::new_incoming_connection);
    CHECK(incoming->guid == client.guid());
    CHECK(incoming->address == client.local_endpoint());
}

TEST_CASE("Peer: a BitStream message arrives as a Packet whose reader starts after the message id",
          "[rakpeer]")
{
    Pair pair;

    BitStreamWriter message;
    message.write(id::user_packet_enum);
    message.write(true);
    message.write(std::uint32_t{0xDEADBEEF});
    message.write(false);
    message.write(std::int16_t{-1234});
    CHECK(pair.client.send(pair.server.guid(), message) != 0);

    const std::optional<Packet> packet = wait_for_packet(pair.server);
    REQUIRE(packet);
    CHECK(packet->id == id::user_packet_enum);
    CHECK(packet->guid == pair.client.guid());
    CHECK(packet->address == pair.client.local_endpoint());
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
    Pair pair;

    pair.client.disconnect(pair.server.guid());

    const std::optional<Packet> packet = wait_for_packet(pair.server);
    REQUIRE(packet);
    CHECK(packet->id == id::disconnection_notification);
    CHECK(packet->guid == pair.client.guid());
    CHECK(packet->address == pair.client.local_endpoint());

    CHECK_FALSE(wait_for_packet(pair.client, 300ms));
    std::error_code ec;
    pair.client.send(pair.server.guid(), std::vector<std::uint8_t>{id::user_packet_enum}, SendOptions(), ec);
    CHECK(ec == Errc::not_connected);
}

TEST_CASE("Peer: shutdown, and destruction, stop the network and release the socket", "[rakpeer]")
{
    const bool destroy = GENERATE(false, true);
    auto peer = std::make_unique<Peer>();
    peer->start(loopback_config());
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

TEST_CASE("Peer: a shut-down Peer can be started again", "[rakpeer]")
{
    Peer peer;
    peer.start(loopback_config());
    peer.shutdown();
    peer.start(loopback_config());
    CHECK(peer.running());
    std::error_code ec;
    peer.start(loopback_config(), ec);
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
