// The Peer through its public API: two Peers talking over loopback UDP.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/wire.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
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

namespace
{

/// A UDP socket posing as a server: it answers a Peer's first connection request with `reply`.
void refuse_first_request(UdpSocket& server, const std::vector<std::uint8_t>& reply)
{
    std::vector<std::uint8_t> buffer(2048);
    Endpoint from;
    server.receive_from(buffer, from); // the Peer's ID_OPEN_CONNECTION_REQUEST_1
    server.send_to(reply, from);
}

} // namespace

TEST_CASE("Peer: each refusal from the server arrives as RakNet's id for it", "[rakpeer]")
{
    const std::uint64_t server_guid = 0x1122334455667788ull;
    std::vector<std::uint8_t> reply;
    std::uint8_t expected = 0;
    std::size_t expected_size = 1; // RakPeer synthesizes these as the id alone
    SECTION("no free incoming connections")
    {
        reply = wire::encode(wire::Rejection{id::no_free_incoming_connections, server_guid});
        expected = id::no_free_incoming_connections;
    }
    SECTION("banned")
    {
        reply = wire::encode(wire::Rejection{id::connection_banned, server_guid});
        expected = id::connection_banned;
    }
    SECTION("already connected")
    {
        reply = wire::encode(wire::Rejection{id::already_connected, server_guid});
        expected = id::already_connected;
    }
    SECTION("IP recently connected")
    {
        reply = wire::encode(wire::Rejection{id::ip_recently_connected, server_guid});
        expected = id::ip_recently_connected;
    }
    SECTION("connection attempt failed")
    {
        reply = wire::encode(wire::Rejection{id::connection_attempt_failed, server_guid});
        expected = id::connection_attempt_failed;
    }
    SECTION("incompatible protocol version")
    {
        wire::IncompatibleProtocolVersion incompatible;
        incompatible.protocol = 5;
        incompatible.guid = server_guid;
        reply = wire::encode(incompatible);
        expected = id::incompatible_protocol_version;
    }
    SECTION("security required")
    {
        wire::OpenConnectionReply1 secure;
        secure.guid = server_guid;
        secure.mtu = 1492;
        secure.padded = false;
        reply = wire::encode(secure);
        reply[25] = 1; // hasCookie: the server demands transport security
        expected = id::remote_system_requires_public_key;
        expected_size = 2; // RakPeer appends a 0 byte to this one
    }

    UdpSocket server;
    server.open(IpAddress::Family::v4);
    server.bind(Endpoint(IpAddress::loopback_v4(), 0));
    Peer client;
    client.start(loopback_config());

    client.connect(server.local_endpoint());
    refuse_first_request(server, reply);

    const std::optional<Packet> packet = wait_for_packet(client);
    REQUIRE(packet);
    CHECK(packet->id == expected);
    std::vector<std::uint8_t> expected_data(expected_size, 0);
    expected_data[0] = expected;
    CHECK(packet->data == expected_data);
    CHECK(packet->address == server.local_endpoint());
}

TEST_CASE("Peer: a server that never answers fails the attempt", "[rakpeer]")
{
    UdpSocket silent;
    silent.open(IpAddress::Family::v4);
    silent.bind(Endpoint(IpAddress::loopback_v4(), 0));
    PeerConfig config = loopback_config();
    config.engine.connect_attempts = 3;
    config.engine.connect_attempt_interval = 50ms;
    Peer client;
    client.start(config);

    client.connect(silent.local_endpoint());

    const std::optional<Packet> packet = wait_for_packet(client);
    REQUIRE(packet);
    CHECK(packet->id == id::connection_attempt_failed);
    CHECK(packet->address == silent.local_endpoint());
}

TEST_CASE("Peer: an incoming password admits the right one and refuses a wrong one", "[rakpeer]")
{
    const std::string secret = "open sesame";
    const std::vector<std::uint8_t> secret_bytes(secret.begin(), secret.end());
    PeerConfig server_config = loopback_config(4);
    server_config.engine.incoming_password = secret_bytes;
    Peer server;
    server.start(server_config);

    Peer right;
    right.start(loopback_config());
    right.connect(server.local_endpoint(), secret_bytes);
    const std::optional<Packet> accepted = wait_for_packet(right);
    REQUIRE(accepted);
    CHECK(accepted->id == id::connection_request_accepted);
    const std::optional<Packet> incoming = wait_for_packet(server);
    REQUIRE(incoming);
    CHECK(incoming->id == id::new_incoming_connection);

    Peer wrong;
    wrong.start(loopback_config());
    wrong.connect(server.local_endpoint(), std::vector<std::uint8_t>{'n', 'o'});
    const std::optional<Packet> refused = wait_for_packet(wrong);
    REQUIRE(refused);
    CHECK(refused->id == id::invalid_password);
    CHECK(refused->address == server.local_endpoint());
    CHECK_FALSE(wait_for_packet(server, 300ms));
}

TEST_CASE("Peer: a remote that goes silent arrives as a lost connection", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    PeerConfig config = loopback_config();
    config.engine.timeout = 500ms;
    Peer client;
    client.start(config);
    client.connect(server.local_endpoint());
    REQUIRE(wait_for_packet(client));
    REQUIRE(wait_for_packet(server));
    const Guid server_guid = server.guid();
    const Endpoint server_address = server.local_endpoint();

    server.shutdown(); // no notification reaches the client

    const std::optional<Packet> packet = wait_for_packet(client, 10s);
    REQUIRE(packet);
    CHECK(packet->id == id::connection_lost);
    CHECK(packet->guid == server_guid);
    CHECK(packet->address == server_address);
}

TEST_CASE("Peer: connecting by host name resolves to the first IPv4 address", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());

    client.connect("localhost", server.local_endpoint().port());

    const std::optional<Packet> accepted = wait_for_packet(client);
    REQUIRE(accepted);
    CHECK(accepted->id == id::connection_request_accepted);
    CHECK(accepted->address == server.local_endpoint());
}

TEST_CASE("Peer: a host name that does not resolve is an error from connect", "[rakpeer]")
{
    Peer client;
    client.start(loopback_config());
    const std::string name = "no-such-host.invalid";

    std::error_code ec;
    client.connect(name, 1234, ByteView(), ec);
    CHECK(ec == Errc::host_not_found);
    CHECK_THROWS_AS(client.connect(name, 1234), std::system_error);
    CHECK_FALSE(wait_for_packet(client, 300ms));
}
