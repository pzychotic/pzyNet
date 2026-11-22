// The Peer's sending through its public API: receipts, broadcast and the reserved-id rule, between
// Peers over loopback UDP.

#include "pzynet/rakpeer/peer.hpp"

#include "pzynet/raknet/message_ids.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace pzynet::raknet;
using namespace std::chrono_literals;
using transport::Reliability;
using Bytes = std::vector<std::uint8_t>;

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

/// Connects `client` to `server`, consuming the connection Packets on both sides.
void connect(Peer& client, Peer& server)
{
    client.connect(server.local_endpoint());
    const std::optional<Packet> accepted = wait_for_packet(client);
    REQUIRE(accepted);
    REQUIRE(accepted->id == id::connection_request_accepted);
    const std::optional<Packet> incoming = wait_for_packet(server);
    REQUIRE(incoming);
    REQUIRE(incoming->id == id::new_incoming_connection);
}

/// The receipt number of a receipt Packet, read the way RakNet applications do: the 4 bytes after the
/// id, in host byte order.
std::uint32_t number_of(const Packet& packet)
{
    REQUIRE(packet.data.size() == 5);
    std::uint32_t number = 0;
    std::memcpy(&number, packet.data.data() + 1, sizeof number);
    return number;
}

SendOptions with(Reliability reliability, bool ack_receipt)
{
    SendOptions options;
    options.reliability = reliability;
    options.ack_receipt = ack_receipt;
    return options;
}

const Bytes message{id::user_packet_enum, 1, 2, 3};

} // namespace

TEST_CASE("Peer: receipt numbers increase per send and acknowledged sends report their number", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());
    connect(client, server);

    const std::uint32_t first = client.send(server.guid(), message, with(Reliability::reliable_ordered, true));
    const std::uint32_t second = client.send(server.guid(), message, with(Reliability::reliable, false));
    const std::uint32_t third = client.send(server.guid(), message, with(Reliability::unreliable, true));
    CHECK(first == 1);
    CHECK(second == 2);
    CHECK(third == 3);

    std::vector<std::uint32_t> acked;
    for (int i = 0; i < 2; ++i)
    {
        const std::optional<Packet> receipt = wait_for_packet(client);
        REQUIRE(receipt);
        CHECK(receipt->id == id::snd_receipt_acked);
        CHECK(receipt->guid == server.guid());
        CHECK(receipt->address == server.local_endpoint());
        acked.push_back(number_of(*receipt));
    }
    CHECK(((acked == std::vector<std::uint32_t>{1, 3}) || (acked == std::vector<std::uint32_t>{3, 1})));
    CHECK_FALSE(wait_for_packet(client, 300ms));
}

TEST_CASE("Peer: no receipt Packets appear unless acknowledgement was requested", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());
    connect(client, server);

    for (const Reliability reliability : {Reliability::unreliable, Reliability::unreliable_sequenced,
                                          Reliability::reliable, Reliability::reliable_ordered,
                                          Reliability::reliable_sequenced})
        CHECK(client.send(server.guid(), message, with(reliability, false)) != 0);

    for (int i = 0; i < 5; ++i)
        CHECK(wait_for_packet(server));
    CHECK_FALSE(wait_for_packet(client, 300ms));
}

TEST_CASE("Peer: an unreliable send that is never acknowledged reports a receipt loss", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());
    connect(client, server);
    const Guid server_guid = server.guid();
    const Endpoint server_address = server.local_endpoint();
    server.shutdown(); // silently: the client still counts it as connected, but nothing is acknowledged

    const std::uint32_t number = client.send(server_guid, message, with(Reliability::unreliable, true));

    const std::optional<Packet> receipt = wait_for_packet(client);
    REQUIRE(receipt);
    CHECK(receipt->id == id::snd_receipt_loss);
    CHECK(number_of(*receipt) == number);
    CHECK(receipt->guid == server_guid);
    CHECK(receipt->address == server_address);
}

TEST_CASE("Peer: acknowledgement cannot be requested for sequenced reliabilities", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());
    connect(client, server);

    for (const Reliability reliability : {Reliability::unreliable_sequenced, Reliability::reliable_sequenced})
    {
        std::error_code ec;
        CHECK(client.send(server.guid(), message, with(reliability, true), ec) == 0);
        CHECK(ec == Errc::invalid_argument);
    }
}

TEST_CASE("Peer: a message id reserved by the protocol is rejected", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer client;
    client.start(loopback_config());
    connect(client, server);

    for (const std::uint8_t reserved : {id::connected_ping, id::connected_pong, id::detect_lost_connections,
                                        id::connection_request, id::connection_request_accepted,
                                        id::new_incoming_connection, id::disconnection_notification,
                                        id::invalid_password})
    {
        std::error_code ec;
        CHECK(client.send(server.guid(), Bytes{reserved, 1}, SendOptions(), ec) == 0);
        CHECK(ec == Errc::reserved_message_id);
        ec.clear();
        CHECK(client.broadcast(Bytes{reserved, 1}, SendOptions(), std::nullopt, ec) == 0);
        CHECK(ec == Errc::reserved_message_id);
    }
    CHECK_FALSE(wait_for_packet(server, 300ms));
    CHECK(client.send(server.guid(), message) == 1); // rejected sends take no receipt number
}

TEST_CASE("Peer: a broadcast reaches every connected system except the excluded one", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer clients[3];
    for (Peer& client : clients)
    {
        client.start(loopback_config());
        connect(client, server);
    }

    const std::uint32_t everyone = server.broadcast(Bytes{id::user_packet_enum, 1});
    const std::uint32_t all_but_one = server.broadcast(Bytes{id::user_packet_enum, 2}, SendOptions(), clients[1].guid());
    CHECK(all_but_one == everyone + 1);

    for (Peer& client : clients)
    {
        const std::optional<Packet> first = wait_for_packet(client);
        REQUIRE(first);
        CHECK(first->data == Bytes{id::user_packet_enum, 1});
        CHECK(first->guid == server.guid());
    }
    for (const int i : {0, 2})
    {
        const std::optional<Packet> second = wait_for_packet(clients[i]);
        REQUIRE(second);
        CHECK(second->data == Bytes{id::user_packet_enum, 2});
    }
    CHECK_FALSE(wait_for_packet(clients[1], 300ms));
}

TEST_CASE("Peer: a broadcast with acknowledgement reports its one receipt number for each system", "[rakpeer]")
{
    Peer server;
    server.start(loopback_config(4));
    Peer clients[2];
    for (Peer& client : clients)
    {
        client.start(loopback_config());
        connect(client, server);
    }

    const std::uint32_t number = server.broadcast(message, with(Reliability::reliable, true));

    std::vector<Guid> acknowledged_by;
    for (int i = 0; i < 2; ++i)
    {
        const std::optional<Packet> receipt = wait_for_packet(server);
        REQUIRE(receipt);
        CHECK(receipt->id == id::snd_receipt_acked);
        CHECK(number_of(*receipt) == number);
        acknowledged_by.push_back(receipt->guid);
    }
    CHECK(((acknowledged_by == std::vector<Guid>{clients[0].guid(), clients[1].guid()}) ||
           (acknowledged_by == std::vector<Guid>{clients[1].guid(), clients[0].guid()})));
}
