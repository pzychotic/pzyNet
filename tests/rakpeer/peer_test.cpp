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

TEST_CASE("Peer: the connection list and the GUID/address lookups follow connects and disconnects",
          "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    CHECK(server.connections().empty());

    Peer first;
    first.start(loopback_config());
    first.connect(server.local_endpoint());
    REQUIRE(wait_for_packet(first));
    REQUIRE(wait_for_packet(server));
    const Endpoint client_address = first.local_endpoint();
    const Guid first_guid = first.guid();

    const std::vector<RemoteSystem> listed = server.connections();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].guid == first_guid);
    CHECK(listed[0].address == client_address);
    CHECK(listed[0].incoming);
    CHECK(listed[0].mtu >= 576);
    CHECK(listed[0].mtu <= 1492);
    CHECK(server.guid_of(client_address) == first_guid);
    CHECK(server.address_of(first_guid) == client_address);

    const std::vector<RemoteSystem> client_side = first.connections();
    REQUIRE(client_side.size() == 1);
    CHECK(client_side[0].guid == server.guid());
    CHECK(client_side[0].address == server.local_endpoint());
    CHECK_FALSE(client_side[0].incoming);

    // The round-trip time shows up once something was acknowledged.
    first.send(server.guid(), std::vector<std::uint8_t>{id::user_packet_enum});
    REQUIRE(wait_for_packet(server));
    const auto give_up = std::chrono::steady_clock::now() + 5s;
    while (!first.connections().at(0).rtt && std::chrono::steady_clock::now() < give_up)
        std::this_thread::sleep_for(1ms);
    CHECK(first.connections().at(0).rtt);

    // A remote close removes the entry.
    first.disconnect(server.guid());
    const std::optional<Packet> closed = wait_for_packet(server);
    REQUIRE(closed);
    REQUIRE(closed->id == id::disconnection_notification);
    CHECK(server.connections().empty());
    CHECK_FALSE(server.guid_of(client_address));
    CHECK_FALSE(server.address_of(first_guid));
    CHECK(first.connections().empty()); // so does a local one

    // A new system on the same address, with a new GUID.
    first.shutdown();
    Peer second;
    PeerConfig config = loopback_config();
    config.bind = client_address;
    second.start(config);
    REQUIRE(second.guid() != first_guid);
    second.connect(server.local_endpoint());
    REQUIRE(wait_for_packet(second));
    const std::optional<Packet> incoming = wait_for_packet(server);
    REQUIRE(incoming);
    REQUIRE(incoming->id == id::new_incoming_connection);

    const std::vector<RemoteSystem> relisted = server.connections();
    REQUIRE(relisted.size() == 1);
    CHECK(relisted[0].guid == second.guid());
    CHECK(relisted[0].address == client_address);
    CHECK(server.guid_of(client_address) == second.guid());
    CHECK(server.address_of(second.guid()) == client_address);
    CHECK_FALSE(server.address_of(first_guid));
}

TEST_CASE("Peer: addressing an unknown GUID or endpoint reports an error and sends nothing", "[rakpeer]")
{
    Pair pair;
    const Remote unknown = GENERATE(Remote(Guid{0x1234}), Remote(Guid{}),
                                    Remote(Endpoint(IpAddress::loopback_v4(), 1)));
    const std::vector<std::uint8_t> message{id::user_packet_enum};

    std::error_code ec;
    CHECK(pair.client.send(unknown, message, SendOptions(), ec) == 0);
    CHECK(ec == Errc::not_connected);
    pair.client.disconnect(unknown, transport::DisconnectMode::graceful, ec);
    CHECK(ec == Errc::not_connected);
    CHECK_FALSE(pair.client.address_of(Guid{}));

    CHECK_FALSE(wait_for_packet(pair.server, 300ms));
    CHECK(pair.client.connections().size() == 1);
    CHECK(pair.server.connections().size() == 1);
}

TEST_CASE("Peer: an offline ping returns the configured response as a pong Packet", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(1));
    const std::vector<std::uint8_t> response{'h', 'e', 'l', 'l', 'o'};
    server.set_offline_ping_response(response);
    Peer pinger;
    pinger.start(loopback_config());

    const auto before = static_cast<std::uint32_t>(RakNetEngine::wire_time(Clock::now()));
    pinger.ping(server.local_endpoint());
    const std::optional<Packet> pong = wait_for_packet(pinger);
    const auto after = static_cast<std::uint32_t>(RakNetEngine::wire_time(Clock::now()));
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);
    CHECK(pong->guid == server.guid());
    CHECK(pong->address == server.local_endpoint());
    REQUIRE(pong->data.size() == 1 + 4 + response.size());
    BitStreamReader reader = pong->reader();
    const auto sent = reader.read<std::uint32_t>();
    CHECK(sent - before <= after - before); // the ping's send time, in RakNet time
    CHECK(std::vector<std::uint8_t>(pong->data.begin() + 5, pong->data.end()) == response);
    CHECK(pinger.connections().empty());

    std::error_code ec;
    server.set_offline_ping_response(std::vector<std::uint8_t>(400), ec);
    CHECK(ec == Errc::message_too_large);
}

TEST_CASE("Peer: a ping for accepting systems only is answered while an incoming slot is free",
          "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(1));
    Peer pinger;
    pinger.start(loopback_config());

    pinger.ping(server.local_endpoint(), true);
    std::optional<Packet> pong = wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);

    pinger.connect(server.local_endpoint()); // takes the only slot
    REQUIRE(wait_for_packet(pinger));
    REQUIRE(wait_for_packet(server));
    pinger.ping(server.local_endpoint(), true);
    CHECK_FALSE(wait_for_packet(pinger, 300ms));
    pinger.ping(server.local_endpoint(), false);
    pong = wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);

    server.set_max_incoming_connections(2); // a slot is free again
    pinger.ping(server.local_endpoint(), true);
    pong = wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);
}

TEST_CASE("Peer: the incoming connection limit and password can change while running", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(0));
    Peer client;
    client.start(loopback_config());

    client.connect(server.local_endpoint());
    std::optional<Packet> outcome = wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::no_free_incoming_connections);

    server.set_max_incoming_connections(1);
    const std::vector<std::uint8_t> password{0x70, 0x77};
    server.set_incoming_password(password);
    client.connect(server.local_endpoint(), std::vector<std::uint8_t>{0x70});
    outcome = wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::invalid_password);

    client.connect(server.local_endpoint(), password);
    outcome = wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::connection_request_accepted);

    std::error_code ec;
    server.set_incoming_password(std::vector<std::uint8_t>(256), ec);
    CHECK(ec == Errc::invalid_argument);
}

TEST_CASE("Peer: an advertisement arrives as a Packet without a connection", "[rakpeer]")
{
    Peer receiver;
    receiver.start(loopback_config());
    Peer advertiser;
    advertiser.start(loopback_config());

    const std::vector<std::uint8_t> data{1, 2, 3};
    advertiser.advertise(receiver.local_endpoint(), data);
    const std::optional<Packet> packet = wait_for_packet(receiver);
    REQUIRE(packet);
    CHECK(packet->id == id::advertise_system);
    CHECK(packet->guid == advertiser.guid());
    CHECK(packet->address == advertiser.local_endpoint());
    CHECK(packet->data == std::vector<std::uint8_t>{id::advertise_system, 1, 2, 3});
    CHECK(receiver.connections().empty());

    std::error_code ec;
    advertiser.advertise(receiver.local_endpoint(), std::vector<std::uint8_t>(400), ec);
    CHECK(ec == Errc::message_too_large);
}
