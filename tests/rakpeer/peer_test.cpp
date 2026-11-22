// The Peer through its public API: two Peers talking over loopback UDP.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
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

TEST_CASE("Peer: a connection with a shortened time-out is reported lost soon after the remote vanishes",
          "[rakpeer]")
{
    Pair pair;
    pair.client.set_timeout(pair.server.guid(), 1s);
    CHECK(pair.client.timeout(pair.server.guid()) == 1s);

    const auto vanished = std::chrono::steady_clock::now();
    pair.server.shutdown(); // silently: the client is not told
    const std::optional<Packet> lost = wait_for_packet(pair.client, 5s); // the default would be 10 s
    REQUIRE(lost);
    CHECK(lost->id == id::connection_lost);
    CHECK(std::chrono::steady_clock::now() - vanished < 4s);
}

TEST_CASE("Peer: time-out calls name connections like send does", "[rakpeer]")
{
    Pair pair;
    std::error_code ec;
    pair.client.set_timeout(Guid{12345}, 1s, ec);
    CHECK(ec == Errc::not_connected);
    pair.client.set_timeout(pair.server.guid(), 0ms, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK_FALSE(pair.client.timeout(Guid{12345}));

    pair.server.set_timeout(3s); // every connection, and the default for new ones
    CHECK(pair.server.timeout(pair.client.guid()) == 3s);
}

TEST_CASE("Peer: each connection reports this Peer's address as the remote side saw it", "[rakpeer]")
{
    Pair pair;
    CHECK(pair.client.external_address(pair.server.guid()) == pair.client.local_endpoint());
    CHECK(pair.server.external_address(pair.client.local_endpoint()) == pair.server.local_endpoint());
    CHECK_FALSE(pair.client.external_address(Guid{12345}));
}
