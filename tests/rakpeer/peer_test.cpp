// The Peer through its public API: Peers talking over loopback UDP, in both threading modes.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/wire.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include "rakpeer/peer_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace pzynet::raknet;
using namespace std::chrono_literals;

using namespace peer_test;

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

TEST_CASE("Peer: statistics are reported for a connected remote and refused for an unknown one", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);
    const std::vector<std::uint8_t> message{id::user_packet_enum, 1, 2, 3};
    client.send(server.guid(), message);
    client.send(server.local_endpoint(), message);
    REQUIRE(net.wait_for_packet(server));
    REQUIRE(net.wait_for_packet(server));

    const ConnectionStats by_guid = client.statistics(server.guid());
    CHECK(by_guid.messages_sent == 2);
    CHECK(by_guid.bytes_sent > 0);
    CHECK(by_guid.bytes_received > 0);
    const ConnectionStats at_server = server.statistics(client.local_endpoint());
    CHECK(at_server.messages_received == 2);

    std::error_code ec;
    client.statistics(Guid{0x1234}, ec);
    CHECK(ec == Errc::not_connected);
    client.statistics(Endpoint(IpAddress::loopback_v4(), 1), ec);
    CHECK(ec == Errc::not_connected);
    CHECK_THROWS_AS(client.statistics(Guid{0x1234}), std::system_error);

    Peer stopped;
    stopped.statistics(Guid{0x1234}, ec);
    CHECK(ec == Errc::not_open);
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

TEST_CASE("Peer: a connection with a shortened time-out is reported lost soon after the remote vanishes",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);
    client.set_timeout(server.guid(), 1s);
    CHECK(client.timeout(server.guid()) == 1s);

    const auto vanished = std::chrono::steady_clock::now();
    server.shutdown(); // silently: the client is not told
    const std::optional<Packet> lost = net.wait_for_packet(client, 5s); // the default would be 10 s
    REQUIRE(lost);
    CHECK(lost->id == id::connection_lost);
    CHECK(std::chrono::steady_clock::now() - vanished < 4s);
}

TEST_CASE("Peer: time-out calls name connections like send does", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);
    std::error_code ec;
    client.set_timeout(Guid{12345}, 1s, ec);
    CHECK(ec == Errc::not_connected);
    client.set_timeout(server.guid(), 0ms, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK_FALSE(client.timeout(Guid{12345}));

    server.set_timeout(3s); // every connection, and the default for new ones
    CHECK(server.timeout(client.guid()) == 3s);
}

TEST_CASE("Peer: each connection reports this Peer's address as the remote side saw it", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);
    CHECK(client.external_address(server.guid()) == client.local_endpoint());
    CHECK(server.external_address(client.local_endpoint()) == server.local_endpoint());
    CHECK_FALSE(client.external_address(Guid{12345}));
}

TEST_CASE("Peer: the connection list and the GUID/address lookups follow connects and disconnects",
          "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 4));
    CHECK(server.connections().empty());

    Peer& first = net.add(loopback_config(threading));
    first.connect(server.local_endpoint());
    REQUIRE(net.wait_for_packet(first));
    REQUIRE(net.wait_for_packet(server));
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
    REQUIRE(net.wait_for_packet(server));
    const auto give_up = std::chrono::steady_clock::now() + 5s;
    while (!first.connections().at(0).rtt && std::chrono::steady_clock::now() < give_up)
        net.pump();
    CHECK(first.connections().at(0).rtt);

    // A remote close removes the entry.
    first.disconnect(server.guid());
    const std::optional<Packet> closed = net.wait_for_packet(server);
    REQUIRE(closed);
    REQUIRE(closed->id == id::disconnection_notification);
    CHECK(server.connections().empty());
    CHECK_FALSE(server.guid_of(client_address));
    CHECK_FALSE(server.address_of(first_guid));
    CHECK(first.connections().empty()); // so does a local one

    // A new system on the same address, with a new GUID.
    net.destroy(first);
    PeerConfig config = loopback_config(threading);
    config.bind = client_address;
    Peer& second = net.add(config);
    REQUIRE(second.guid() != first_guid);
    second.connect(server.local_endpoint());
    REQUIRE(net.wait_for_packet(second));
    const std::optional<Packet> incoming = net.wait_for_packet(server);
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
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    auto [server, client] = net.connected_pair(threading);
    const Remote unknown = GENERATE(Remote(Guid{0x1234}), Remote(Guid{}),
                                    Remote(Endpoint(IpAddress::loopback_v4(), 1)));
    const std::vector<std::uint8_t> message{id::user_packet_enum};

    std::error_code ec;
    CHECK(client.send(unknown, message, SendOptions(), ec) == 0);
    CHECK(ec == Errc::not_connected);
    client.disconnect(unknown, transport::DisconnectMode::graceful, ec);
    CHECK(ec == Errc::not_connected);
    CHECK_FALSE(client.address_of(Guid{}));

    CHECK_FALSE(net.wait_for_packet(server, 300ms));
    CHECK(client.connections().size() == 1);
    CHECK(server.connections().size() == 1);
}

TEST_CASE("Peer: an offline ping returns the configured response as a pong Packet", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 1));
    const std::vector<std::uint8_t> response{'h', 'e', 'l', 'l', 'o'};
    server.set_offline_ping_response(response);
    Peer& pinger = net.add(loopback_config(threading));

    const auto before = static_cast<std::uint32_t>(RakNetEngine::wire_time(Clock::now()));
    pinger.ping(server.local_endpoint());
    const std::optional<Packet> pong = net.wait_for_packet(pinger);
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
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 1));
    Peer& pinger = net.add(loopback_config(threading));

    pinger.ping(server.local_endpoint(), true);
    std::optional<Packet> pong = net.wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);

    pinger.connect(server.local_endpoint()); // takes the only slot
    REQUIRE(net.wait_for_packet(pinger));
    REQUIRE(net.wait_for_packet(server));
    pinger.ping(server.local_endpoint(), true);
    CHECK_FALSE(net.wait_for_packet(pinger, 300ms));
    pinger.ping(server.local_endpoint(), false);
    pong = net.wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);

    server.set_max_incoming_connections(2); // a slot is free again
    pinger.ping(server.local_endpoint(), true);
    pong = net.wait_for_packet(pinger);
    REQUIRE(pong);
    CHECK(pong->id == id::unconnected_pong);
}

TEST_CASE("Peer: the incoming connection limit and password can change while running", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 0));
    Peer& client = net.add(loopback_config(threading));

    client.connect(server.local_endpoint());
    std::optional<Packet> outcome = net.wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::no_free_incoming_connections);

    server.set_max_incoming_connections(1);
    const std::vector<std::uint8_t> password{0x70, 0x77};
    server.set_incoming_password(password);
    client.connect(server.local_endpoint(), std::vector<std::uint8_t>{0x70});
    outcome = net.wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::invalid_password);

    client.connect(server.local_endpoint(), password);
    outcome = net.wait_for_packet(client);
    REQUIRE(outcome);
    CHECK(outcome->id == id::connection_request_accepted);

    std::error_code ec;
    server.set_incoming_password(std::vector<std::uint8_t>(256), ec);
    CHECK(ec == Errc::invalid_argument);
}

TEST_CASE("Peer: an advertisement arrives as a Packet without a connection", "[rakpeer]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& receiver = net.add(loopback_config(threading));
    Peer& advertiser = net.add(loopback_config(threading));

    const std::vector<std::uint8_t> data{1, 2, 3};
    advertiser.advertise(receiver.local_endpoint(), data);
    const std::optional<Packet> packet = net.wait_for_packet(receiver);
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

TEST_CASE("Peer: a timestamped message arrives converted into the receiver's clock", "[rakpeer]")
{
    // One side's clock runs an hour ahead, so an unconverted time would be an hour off.
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    const bool skew_sender = GENERATE(true, false);
    INFO((skew_sender ? "sender's clock is ahead" : "receiver's clock is ahead"));
    const ClockSource ahead = [] { return Clock::now() + 1h; };

    PeerConfig server_config = loopback_config(threading, 4);
    PeerConfig client_config = loopback_config(threading);
    (skew_sender ? client_config : server_config).clock = ahead;
    Network net;
    auto [server, client] = net.connected_pair(server_config, client_config);

    const std::int64_t skew = time_difference(client.time(), server.time());
    CHECK(std::abs(skew - (skew_sender ? 3'600'000 : -3'600'000)) <= 50);

    const std::uint64_t sent_at = client.time();
    BitStreamWriter message;
    message.write(id::timestamp);
    message.write(sent_at);
    message.write(id::user_packet_enum);
    message.write(std::uint32_t{0xC0FFEE});
    client.send(server.guid(), message);

    const std::optional<Packet> packet = net.wait_for_packet(server);
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
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    PeerConfig client_config = loopback_config(threading);
    client_config.clock = [] { return Clock::now() + 1h; }; // any conversion would change the bytes
    Network net;
    auto [server, client] = net.connected_pair(loopback_config(threading, 4), client_config);

    const std::vector<std::uint8_t> truncated{id::timestamp, 1, 2, 3, 4, 5, 6, 7};
    client.send(server.guid(), truncated);

    const std::optional<Packet> packet = net.wait_for_packet(server);
    REQUIRE(packet);
    CHECK(packet->id == id::timestamp);
    CHECK(packet->data == truncated);
    CHECK_FALSE(packet->timestamp);
    BitStreamReader reader = packet->reader();
    CHECK(reader.read<std::uint8_t>() == 1);
}

TEST_CASE("Peer: a client of a server that banned its address gets a connection-banned Packet", "[rakpeer][ban]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    Peer& server = net.add(loopback_config(threading, 4));
    server.ban("127.0.0.*");
    CHECK(server.is_banned("127.0.0.1"));
    Peer& client = net.add(loopback_config(threading));

    client.connect(server.local_endpoint());
    const std::optional<Packet> refused = net.wait_for_packet(client);
    REQUIRE(refused);
    CHECK(refused->id == id::connection_banned);
    CHECK(refused->address == server.local_endpoint());

    server.unban("127.0.0.*");
    CHECK_FALSE(server.is_banned("127.0.0.1"));
    client.connect(server.local_endpoint());
    const std::optional<Packet> accepted = net.wait_for_packet(client);
    REQUIRE(accepted);
    CHECK(accepted->id == id::connection_request_accepted);
}

TEST_CASE("Peer: the ban list reports misuse through the error code", "[rakpeer][ban]")
{
    Peer peer;
    std::error_code ec;
    peer.ban("1.2.3.4", 0ms, ec);
    CHECK(ec == Errc::not_open);
    CHECK_FALSE(peer.is_banned("1.2.3.4"));

    peer.start(loopback_config(Threading::app_driven));
    peer.ban("", 0ms, ec);
    CHECK(ec == Errc::invalid_argument);
    peer.ban("1.2.3.4", 1h, ec);
    CHECK_FALSE(ec);
    CHECK(peer.is_banned("1.2.3.4"));
    peer.clear_bans();
    CHECK_FALSE(peer.is_banned("1.2.3.4"));
}

TEST_CASE("Peer: a client reconnecting too quickly gets an IP-recently-connected Packet", "[rakpeer][reconnect]")
{
    const Threading threading = GENERATE(Threading::background, Threading::app_driven);
    Network net;
    PeerConfig server_config = loopback_config(threading, 4);
    server_config.engine.limit_ip_connection_frequency = true;
    Peer& server = net.add(server_config);

    // The clients use another loopback IP: the server's own address is exempt from the limit.
    PeerConfig client_config = loopback_config(threading);
    client_config.bind = Endpoint(IpAddress::v4(127, 0, 0, 2), 0);
    Peer& first = net.add(client_config);
    Peer& second = net.add(client_config);

    // A loopback handshake takes a few milliseconds, well inside the 100 ms window.
    first.connect(server.local_endpoint());
    const std::optional<Packet> accepted = net.wait_for_packet(first);
    REQUIRE(accepted);
    REQUIRE(accepted->id == id::connection_request_accepted);
    second.connect(server.local_endpoint());
    const std::optional<Packet> refused = net.wait_for_packet(second);
    REQUIRE(refused);
    CHECK(refused->id == id::ip_recently_connected);
}
