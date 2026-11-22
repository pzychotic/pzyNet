// The Peer through its public API: Peers talking over loopback UDP, in both threading modes.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
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
