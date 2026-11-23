// Per-connection statistics of the RakNet engine, checked against what the in-memory network saw on
// the wire and what it was told to lose.

#include "raknet/test_network.hpp"

#include "pzynet/core/error.hpp"
#include "pzynet/raknet/message_ids.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

using namespace pzynet;
using namespace pzynet::raknet;
using namespace pzynet::transport;
using namespace test;

namespace
{

Config server_config()
{
    Config config;
    config.max_incoming_connections = 8;
    return config;
}

/// A user message of `size` bytes: RakNet's first user id, a marker byte, then 'x' filler.
Bytes marked_message(std::uint8_t marker, std::size_t size = 100)
{
    Bytes bytes(size, 'x');
    bytes[0] = id::user_packet_enum;
    bytes[1] = marker;
    return bytes;
}

bool carries(const Bytes& datagram, std::uint8_t marker)
{
    const Bytes needle{id::user_packet_enum, marker, 'x', 'x', 'x', 'x'};
    return std::search(datagram.begin(), datagram.end(), needle.begin(), needle.end()) != datagram.end();
}

/// Datagrams of an established connection carry the "valid" flag; offline messages never do.
bool connected_datagram(const Network::Datagram& datagram)
{
    return !datagram.data.empty() && (datagram.data[0] & 0x80) != 0;
}

void send(Node& node, ConnectionId connection, const Bytes& data)
{
    std::error_code ec;
    node.engine.send(connection, data, SendOptions(), ec);
    REQUIRE_FALSE(ec);
}

/// Bytes of the connected datagrams the network saw, per direction.
struct WireCount
{
    std::uint64_t client_to_server = 0;
    std::uint64_t server_to_client = 0;
};

struct Fixture
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();
    ConnectionId at_client = 0;
    ConnectionId at_server = 0;
    WireCount sent;     // everything put on the wire
    WireCount received; // what was not dropped

    Fixture()
    {
        network.tap = [this](const Network::Datagram& datagram) {
            if (connected_datagram(datagram))
                (datagram.from == client.address ? sent.client_to_server : sent.server_to_client) +=
                    datagram.data.size();
        };
        std::error_code ec;
        at_client = client.engine.connect(server.address, ByteView(), ec);
        REQUIRE_FALSE(ec);
        REQUIRE(network.run_until([&] { return client.count<Connected>() == 1 && server.count<Connected>() == 1; }));
        at_server = server.all<Connected>().back().connection;
        network.run(std::chrono::milliseconds(200)); // let the handshake's acknowledgements settle
    }

    /// Installs a drop rule and keeps `received` in step with what gets through.
    void lose(std::function<bool(const Network::Datagram&)> rule)
    {
        network.drop = [this, rule](const Network::Datagram& datagram) {
            const bool lost = rule(datagram);
            if (!lost && connected_datagram(datagram))
                (datagram.from == client.address ? received.client_to_server : received.server_to_client) +=
                    datagram.data.size();
            return lost;
        };
        received = sent;
    }

    ConnectionStats client_stats() const
    {
        const std::optional<ConnectionStats> stats = client.engine.connection_stats(at_client);
        REQUIRE(stats);
        return *stats;
    }
    ConnectionStats server_stats() const
    {
        const std::optional<ConnectionStats> stats = server.engine.connection_stats(at_server);
        REQUIRE(stats);
        return *stats;
    }
};

} // namespace

TEST_CASE("connection statistics count the traffic of a lossless connection", "[raknet][engine][stats]")
{
    Fixture f;
    for (std::uint8_t i = 1; i <= 10; ++i)
    {
        send(f.client, f.at_client, marked_message(i));
        f.network.run(std::chrono::milliseconds(20));
    }
    f.network.run(std::chrono::milliseconds(500));
    REQUIRE(f.server.messages().size() == 10);

    const ConnectionStats client = f.client_stats();
    const ConnectionStats server = f.server_stats();
    CHECK(client.messages_sent == 10);
    CHECK(client.messages_received == 0);
    CHECK(server.messages_received == 10);
    CHECK(server.messages_sent == 0);

    CHECK(client.bytes_sent == f.sent.client_to_server);
    CHECK(server.bytes_received == f.sent.client_to_server);
    CHECK(server.bytes_sent == f.sent.server_to_client);
    CHECK(client.bytes_received == f.sent.server_to_client);

    CHECK(client.datagrams_resent == 0);
    CHECK(client.datagrams_lost == 0);
    CHECK(server.datagrams_resent == 0);
    CHECK(server.datagrams_lost == 0);
    CHECK(client.queued_messages == 0);
    CHECK(client.queued_bytes == 0);
}

TEST_CASE("connection statistics count injected loss as lost and resent datagrams", "[raknet][engine][stats]")
{
    Fixture f;
    // The first transmissions of messages 2, 5 and 8 are lost and reported by a NAK once the next
    // message arrives; message 10 is the last one, so only the retransmission time-out recovers it.
    const std::set<std::uint8_t> victims{2, 5, 8, 10};
    std::set<std::uint8_t> dropped;
    f.lose([&](const Network::Datagram& datagram) {
        if (datagram.from != f.client.address)
            return false;
        for (std::uint8_t marker : victims)
            if (carries(datagram.data, marker) && dropped.insert(marker).second)
                return true;
        return false;
    });

    for (std::uint8_t i = 1; i <= 10; ++i)
    {
        send(f.client, f.at_client, marked_message(i));
        f.network.run(std::chrono::milliseconds(20));
    }
    REQUIRE(f.network.run_until([&] { return f.server.messages().size() == 10; }, std::chrono::seconds(5)));
    f.network.run(std::chrono::milliseconds(500));
    REQUIRE(dropped == victims);

    const ConnectionStats client = f.client_stats();
    const ConnectionStats server = f.server_stats();
    CHECK(client.datagrams_lost == 4);
    CHECK(client.datagrams_resent == 4);
    CHECK(client.messages_sent == 10);
    CHECK(server.messages_received == 10);
    CHECK(client.bytes_sent == f.sent.client_to_server);
    CHECK(server.bytes_received == f.received.client_to_server);
    CHECK(server.bytes_received < client.bytes_sent);
    CHECK(server.datagrams_lost == 0);
    CHECK(server.datagrams_resent == 0);
}

TEST_CASE("connection statistics report the send queue", "[raknet][engine][stats]")
{
    Fixture f;
    send(f.client, f.at_client, marked_message(1, 100));
    send(f.client, f.at_client, marked_message(2, 250));
    send(f.client, f.at_client, marked_message(3, 5000)); // split into several pieces

    ConnectionStats stats = f.client_stats();
    CHECK(stats.queued_messages == 3);
    CHECK(stats.queued_bytes == 5350);
    CHECK(stats.messages_sent == 3);

    REQUIRE(f.network.run_until([&] { return f.server.messages().size() == 3; }));
    stats = f.client_stats();
    CHECK(stats.queued_messages == 0);
    CHECK(stats.queued_bytes == 0);
}

TEST_CASE("connection statistics are not available for an unknown connection", "[raknet][engine][stats]")
{
    Fixture f;
    CHECK_FALSE(f.client.engine.connection_stats(f.at_client + 100));
    f.client.engine.disconnect(f.at_client, DisconnectMode::graceful);
    CHECK_FALSE(f.client.engine.connection_stats(f.at_client));
}
