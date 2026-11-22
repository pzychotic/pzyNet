// Behavioural tests of the RakNet engine: two (or more) engines talking through an in-memory
// network with a fake clock. No sockets, fully deterministic.

#include "raknet/test_network.hpp"

#include "pzynet/core/byte_io.hpp"
#include "pzynet/core/error.hpp"
#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/wire.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <utility>

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

/// A user message: RakNet's first user id, a 32-bit index, then filler up to `size` bytes.
Bytes user_message(std::uint32_t index, std::size_t size = 5)
{
    Bytes bytes;
    ByteWriter writer(bytes);
    writer.u8(id::user_packet_enum);
    writer.u32_be(index);
    while (bytes.size() < size)
        bytes.push_back(static_cast<std::uint8_t>(bytes.size() * 31 + index));
    return bytes;
}

std::uint32_t index_of(const Bytes& message)
{
    ByteReader reader(message);
    reader.skip(1);
    return reader.u32_be();
}

void send(Node& node, ConnectionId connection, const Bytes& data,
          Reliability reliability = Reliability::reliable_ordered, std::uint8_t channel = 0,
          std::uint32_t receipt_token = 0)
{
    SendOptions options;
    options.reliability = reliability;
    options.channel = channel;
    options.receipt_token = receipt_token;
    std::error_code ec;
    node.engine.send(connection, data, options, ec);
    REQUIRE_FALSE(ec);
}

/// Connects `client` to `server` and returns {client's id, server's id} for the new connection.
std::pair<ConnectionId, ConnectionId> connect(Network& network, Node& client, Node& server, ByteView password = {})
{
    const std::size_t client_before = client.count<Connected>();
    const std::size_t server_before = server.count<Connected>();
    std::error_code ec;
    const ConnectionId id = client.engine.connect(server.address, password, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(network.run_until([&] {
        return client.count<Connected>() == client_before + 1 && server.count<Connected>() == server_before + 1;
    }));
    const Connected at_client = client.all<Connected>().back();
    const Connected at_server = server.all<Connected>().back();
    REQUIRE(at_client.connection == id);
    REQUIRE_FALSE(at_client.incoming);
    REQUIRE(at_server.incoming);
    return {id, at_server.connection};
}

struct Session
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();
    ConnectionId at_client = 0;
    ConnectionId at_server = 0;

    Session()
    {
        const auto ids = connect(network, client, server);
        at_client = ids.first;
        at_server = ids.second;
    }
};

} // namespace

// --- connecting ----------------------------------------------------------------------------------

TEST_CASE("two engines complete the RakNet handshake", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();

    std::vector<Network::Datagram> wire;
    network.tap = [&](const Network::Datagram& datagram) { wire.push_back(datagram); };

    const auto ids = connect(network, client, server);

    // The first four datagrams are the offline exchange, in order.
    REQUIRE(wire.size() >= 4);
    CHECK(wire[0].data[0] == id::open_connection_request_1);
    CHECK(wire[0].data.size() == 1492 - 28); // an MTU probe: padded to the full size...
    CHECK(wire[0].dont_fragment);            // ...and not allowed to fragment
    CHECK(wire[1].data[0] == id::open_connection_reply_1);
    CHECK(wire[1].dont_fragment);
    CHECK(wire[2].data[0] == id::open_connection_request_2);
    CHECK(wire[2].data.size() == 34);
    CHECK_FALSE(wire[2].dont_fragment);
    CHECK(wire[3].data[0] == id::open_connection_reply_2);
    CHECK(wire[3].data.size() == 35);
    // Everything after that is a connected datagram (valid bit set).
    for (std::size_t i = 4; i < wire.size(); ++i)
        CHECK((wire[i].data[0] & 0x80) != 0);

    const auto client_info = client.engine.connection_info(ids.first);
    const auto server_info = server.engine.connection_info(ids.second);
    REQUIRE(client_info);
    REQUIRE(server_info);
    CHECK(client_info->remote == server.address);
    CHECK(client_info->remote_guid == server.engine.guid());
    CHECK(server_info->remote_guid == client.engine.guid());
    CHECK(client_info->mtu == 1492);
    CHECK(server_info->mtu == 1492);
    CHECK_FALSE(client_info->incoming);
    CHECK(server_info->incoming);
    CHECK(client.engine.connection_count() == 1);
    CHECK(server.engine.connection_count() == 1);

    // Both ends share the simulated clock, so the measured offset is about zero.
    network.run(100ms);
    REQUIRE(client.engine.connection_info(ids.first)->clock_offset);
    CHECK(std::abs(client.engine.connection_info(ids.first)->clock_offset->count()) <= 10);
    REQUIRE(client.engine.connection_info(ids.first)->rtt);
    CHECK(*client.engine.connection_info(ids.first)->rtt >= 10ms); // twice the 5 ms link latency
    CHECK(*client.engine.connection_info(ids.first)->rtt < 40ms);
}

TEST_CASE("the first user message is numbered like a stock RakNet client's", "[raknet][engine]")
{
    Session s;
    std::vector<Bytes> wire;
    s.network.tap = [&](const Network::Datagram& datagram) {
        if (datagram.from == s.client.address)
            wire.push_back(datagram.data);
    };
    send(s.client, s.at_client, {0x86, 0x01, 0x02, 0x03});
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 1; }));

    REQUIRE_FALSE(wire.empty());
    ByteReader reader(wire[0]);
    wire::DatagramHeader datagram;
    wire::MessageHeader message;
    REQUIRE(wire::read_datagram_header(reader, datagram));
    REQUIRE(wire::read_message_header(reader, message));
    CHECK(datagram.kind == wire::DatagramKind::data);
    CHECK(message.reliability == wire::Reliability::reliable_ordered);
    CHECK(message.bit_length == 32);
    // ID_CONNECTION_REQUEST took reliable number 0, ID_NEW_INCOMING_CONNECTION number 1 and
    // ordering index 0 on channel 0; the first user message follows them.
    CHECK(message.reliable_number == 2);
    CHECK(message.ordering_index == 1);
    CHECK(message.ordering_channel == 0);
    const ByteView payload = reader.rest();
    CHECK(Bytes(payload.begin(), payload.end()) == Bytes{0x86, 0x01, 0x02, 0x03});
}

TEST_CASE("a connection attempt to a silent address fails after the MTU ladder", "[raknet][engine]")
{
    Network network;
    Node& client = network.add();
    std::map<std::size_t, int> probes; // datagram size -> count
    network.tap = [&](const Network::Datagram& datagram) {
        CHECK(datagram.data[0] == id::open_connection_request_1);
        CHECK(datagram.dont_fragment);
        ++probes[datagram.data.size()];
    };

    std::error_code ec;
    const ConnectionId id = client.engine.connect(Endpoint(IpAddress::v4(10, 9, 9, 9), 1234), {}, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }));

    const ConnectFailed failure = client.all<ConnectFailed>()[0];
    CHECK(failure.connection == id);
    CHECK(failure.reason == ConnectFailure::no_response);
    // Twelve attempts, four at each MTU size, 500 ms apart.
    CHECK(probes == std::map<std::size_t, int>{{1492 - 28, 4}, {1200 - 28, 4}, {576 - 28, 4}});
    CHECK(client.engine.connection_count() == 0);
    CHECK_FALSE(client.engine.next_deadline());
}

TEST_CASE("the handshake settles on the largest MTU the path carries", "[raknet][engine]")
{
    Network network;
    network.path_mtu = 1300; // 1492-byte probes vanish, 1200-byte ones get through
    Node& server = network.add(server_config());
    Node& client = network.add();
    const auto ids = connect(network, client, server);

    CHECK(client.engine.connection_info(ids.first)->mtu == 1200);
    CHECK(server.engine.connection_info(ids.second)->mtu == 1200);

    // From now on nothing larger than the negotiated size is sent, even for big messages.
    std::size_t largest = 0;
    network.tap = [&](const Network::Datagram& datagram) { largest = std::max(largest, datagram.data.size()); };
    const Bytes big = user_message(1, 50'000);
    send(client, ids.first, big);
    REQUIRE(network.run_until([&] { return server.count<Message>() == 1; }));
    CHECK(server.messages()[0] == big);
    CHECK(largest <= 1200 - 28);
    CHECK(largest > 1100); // and the size is actually used
}

TEST_CASE("the password is checked by the server", "[raknet][engine]")
{
    Network network;
    Config config = server_config();
    config.incoming_password = {'s', 'e', 'c', 'r', 'e', 't'};
    Node& server = network.add(config);
    Node& wrong = network.add();
    Node& right = network.add();

    std::error_code ec;
    const Bytes bad{'g', 'u', 'e', 's', 's'};
    wrong.engine.connect(server.address, bad, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(network.run_until([&] { return wrong.count<ConnectFailed>() == 1; }));
    CHECK(wrong.all<ConnectFailed>()[0].reason == ConnectFailure::invalid_credentials);
    CHECK(server.count<Connected>() == 0);

    connect(network, right, server, config.incoming_password);
    CHECK(server.count<Connected>() == 1);

    // The rejected attempt leaves nothing behind on either side.
    network.run(11s);
    CHECK(wrong.engine.connection_count() == 0);
    CHECK(server.engine.connection_count() == 1);
}

TEST_CASE("connections beyond the incoming limit are refused", "[raknet][engine]")
{
    Network network;
    Config config;
    config.max_incoming_connections = 1;
    Node& server = network.add(config);
    Node& first = network.add();
    Node& second = network.add();
    Node& closed = network.add(); // max_incoming_connections = 0: accepts nobody

    connect(network, first, server);

    std::error_code ec;
    second.engine.connect(server.address, {}, ec);
    first.engine.connect(closed.address, {}, ec);
    REQUIRE(network.run_until([&] { return second.count<ConnectFailed>() == 1 && first.count<ConnectFailed>() == 1; }));
    CHECK(second.all<ConnectFailed>()[0].reason == ConnectFailure::no_free_slots);
    CHECK(first.all<ConnectFailed>()[0].reason == ConnectFailure::no_free_slots);
    CHECK(server.engine.connection_count() == 1);

    // Raising the limit at run time lets the next attempt in.
    server.engine.set_max_incoming_connections(2);
    connect(network, second, server);
}

TEST_CASE("connect reports misuse through the error code", "[raknet][engine]")
{
    Session s;
    std::error_code ec;
    s.client.engine.connect(s.server.address, {}, ec);
    CHECK(ec == make_error_code(Errc::already_connected));
    s.client.engine.connect(Endpoint(IpAddress::v4(10, 0, 0, 9), 0), {}, ec);
    CHECK(ec == make_error_code(Errc::invalid_argument));

    Config tiny;
    tiny.max_connections = 1;
    Node& limited = s.network.add(tiny);
    CHECK(limited.engine.connect(s.server.address, {}, ec) != invalid_connection);
    CHECK_FALSE(ec);
    CHECK(limited.engine.connect(s.client.address, {}, ec) == invalid_connection);
    CHECK(ec == make_error_code(Errc::connection_limit_reached));
}

TEST_CASE("handshake replies from an incompatible server fail the attempt", "[raknet][engine]")
{
    const Endpoint server(IpAddress::v4(10, 9, 9, 9), 1234);

    SECTION("different protocol version")
    {
        Network network;
        Node& client = network.add();
        std::error_code ec;
        client.engine.connect(server, {}, ec);
        network.run(20ms);
        wire::IncompatibleProtocolVersion reply;
        reply.protocol = 5;
        network.inject(server, client.address, wire::encode(reply));
        REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }, 1s));
        CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::incompatible_protocol);
    }
    SECTION("server demands transport security")
    {
        Network network;
        Node& client = network.add();
        std::error_code ec;
        client.engine.connect(server, {}, ec);
        network.run(20ms);
        wire::OpenConnectionReply1 reply;
        reply.mtu = 1492;
        reply.padded = false;
        Bytes bytes = wire::encode(reply);
        bytes[25] = 1; // hasCookie
        network.inject(server, client.address, bytes);
        REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }, 1s));
        CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::unsupported);
    }
    SECTION("banned")
    {
        Network network;
        Node& client = network.add();
        std::error_code ec;
        client.engine.connect(server, {}, ec);
        network.run(20ms);
        network.inject(server, client.address, wire::encode(wire::Rejection{id::connection_banned, 1}));
        REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }, 1s));
        CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::banned);
    }
    SECTION("each refusal keeps its own reason")
    {
        const auto [refusal, reason] = GENERATE(
            std::make_pair(id::no_free_incoming_connections, ConnectFailure::no_free_slots),
            std::make_pair(id::ip_recently_connected, ConnectFailure::recently_connected),
            std::make_pair(id::already_connected, ConnectFailure::already_connected),
            std::make_pair(id::connection_attempt_failed, ConnectFailure::connection_refused));
        Network network;
        Node& client = network.add();
        std::error_code ec;
        client.engine.connect(server, {}, ec);
        network.run(20ms);
        network.inject(server, client.address, wire::encode(wire::Rejection{refusal, 1}));
        REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }, 1s));
        CHECK(client.all<ConnectFailed>()[0].reason == reason);
    }
}

TEST_CASE("a client speaking another protocol version is told so", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    const Endpoint stranger(IpAddress::v4(10, 9, 9, 9), 1234);
    std::vector<Bytes> replies;
    network.tap = [&](const Network::Datagram& datagram) {
        if (datagram.to == stranger)
            replies.push_back(datagram.data);
    };

    wire::OpenConnectionRequest1 request;
    request.protocol = 5;
    request.mtu = 576;
    network.inject(stranger, server.address, wire::encode(request));
    network.run(50ms);

    REQUIRE(replies.size() == 1);
    wire::IncompatibleProtocolVersion reply;
    REQUIRE(wire::decode(replies[0], reply));
    CHECK(reply.protocol == wire::protocol_version);
    CHECK(reply.guid == server.engine.guid());
    CHECK(server.engine.connection_count() == 0);
}

// --- ban list and reconnect limit ----------------------------------------------------------------

TEST_CASE("a banned address is refused with the connection-banned id", "[raknet][engine][ban]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();
    std::vector<Bytes> replies;
    network.tap = [&](const Network::Datagram& datagram) {
        if (datagram.to == client.address)
            replies.push_back(datagram.data);
    };

    REQUIRE(server.engine.ban("10.0.0.2", 0ms, network.now));
    CHECK(server.engine.is_banned("10.0.0.2", network.now));

    std::error_code ec;
    client.engine.connect(server.address, {}, ec);
    REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }));
    CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::banned);
    CHECK(server.engine.connection_count() == 0);

    REQUIRE_FALSE(replies.empty());
    wire::Rejection rejection;
    REQUIRE(wire::decode(replies[0], rejection));
    CHECK(rejection.id == id::connection_banned);
    CHECK(rejection.guid == server.engine.guid());
}

TEST_CASE("a ban for a duration expires", "[raknet][engine][ban]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();

    REQUIRE(server.engine.ban("10.0.0.2", 2s, network.now));
    std::error_code ec;
    client.engine.connect(server.address, {}, ec);
    REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }));
    CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::banned);

    CHECK(server.engine.is_banned("10.0.0.2", network.now + 1s));
    network.run(2s);
    CHECK_FALSE(server.engine.is_banned("10.0.0.2", network.now));
    connect(network, client, server);
}

TEST_CASE("banning again replaces the duration", "[raknet][engine][ban]")
{
    Network network;
    Node& server = network.add(server_config());
    REQUIRE(server.engine.ban("10.0.0.2", 1s, network.now));
    REQUIRE(server.engine.ban("10.0.0.2", 0ms, network.now));
    CHECK(server.engine.is_banned("10.0.0.2", network.now + 24h));
    REQUIRE(server.engine.ban("10.0.0.2", 1s, network.now));
    CHECK_FALSE(server.engine.is_banned("10.0.0.2", network.now + 2s));
}

TEST_CASE("unbanning and clearing the ban list let a client in again", "[raknet][engine][ban]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();
    REQUIRE(server.engine.ban("10.0.0.2", 0ms, network.now));
    REQUIRE(server.engine.ban("10.0.0.3", 0ms, network.now));

    SECTION("unban")
    {
        server.engine.unban("10.0.0.2");
        CHECK(server.engine.is_banned("10.0.0.3", network.now));
    }
    SECTION("clear")
    {
        server.engine.clear_bans();
        CHECK_FALSE(server.engine.is_banned("10.0.0.3", network.now));
    }
    CHECK_FALSE(server.engine.is_banned("10.0.0.2", network.now));
    connect(network, client, server);
}

TEST_CASE("ban entries match like RakNet's", "[raknet][engine][ban]")
{
    Network network;
    Node& server = network.add(server_config());
    RakNetEngine& engine = server.engine;
    const TimePoint now = network.now;

    REQUIRE(engine.ban("10.0.0.2", 0ms, now));
    CHECK(engine.is_banned("10.0.0.2", now));
    CHECK_FALSE(engine.is_banned("10.0.0.20", now)); // no prefix match without a wildcard
    CHECK_FALSE(engine.is_banned("10.0.0.", now));

    REQUIRE(engine.ban("192.168.*", 0ms, now));
    CHECK(engine.is_banned("192.168.1.7", now));
    CHECK(engine.is_banned("192.168.250.250", now));
    CHECK_FALSE(engine.is_banned("192.169.1.7", now));

    engine.unban("192.168"); // only the exact entry is removed
    CHECK(engine.is_banned("192.168.1.7", now));

    CHECK_FALSE(engine.ban("", 0ms, now));
    CHECK_FALSE(engine.ban("1234:5678:9abc::1", 0ms, now)); // longer than a dotted IPv4 address

    Node& client = network.add(); // 10.0.0.2 via the wildcard
    REQUIRE(engine.ban("10.0.*", 0ms, now));
    engine.unban("10.0.0.2");
    std::error_code ec;
    client.engine.connect(server.address, {}, ec);
    REQUIRE(network.run_until([&] { return client.count<ConnectFailed>() == 1; }));
    CHECK(client.all<ConnectFailed>()[0].reason == ConnectFailure::banned);
}

TEST_CASE("a connection from a newly banned address times out", "[raknet][engine][ban]")
{
    Session s;
    REQUIRE(s.server.engine.ban("10.0.0.2", 0ms, s.network.now));
    send(s.client, s.at_client, user_message(1));
    REQUIRE(s.network.run_until([&] { return s.server.count<Disconnected>() == 1; }, 30s));
    CHECK(s.server.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
    CHECK(s.server.count<Message>() == 0);
    REQUIRE(s.network.run_until([&] { return s.client.count<Disconnected>() == 1; }, 30s));
    CHECK(s.client.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
}

TEST_CASE("the reconnect limit refuses an IP that connects again within 100 ms", "[raknet][engine][reconnect]")
{
    Network network;
    Config config = server_config();
    config.limit_ip_connection_frequency = true;
    Node& server = network.add(config);
    Node& first = network.add();
    Node& second = network.add();
    second.address = Endpoint(first.address.address(), 60001); // same IP, another port
    std::vector<Bytes> replies;
    network.tap = [&](const Network::Datagram& datagram) {
        if (datagram.to == second.address && datagram.data[0] == id::ip_recently_connected)
            replies.push_back(datagram.data);
    };

    std::error_code ec;
    first.engine.connect(server.address, {}, ec);
    network.run(1ms);
    second.engine.connect(server.address, {}, ec);
    REQUIRE(network.run_until([&] { return second.count<ConnectFailed>() == 1; }, 1s));
    CHECK(second.all<ConnectFailed>()[0].reason == ConnectFailure::recently_connected);
    REQUIRE(replies.size() == 1);
    wire::Rejection rejection;
    REQUIRE(wire::decode(replies[0], rejection));
    CHECK(rejection.guid == server.engine.guid());
    REQUIRE(network.run_until([&] { return first.count<Connected>() == 1; }));

    // Once the window has passed, the same IP is welcome again.
    network.run(100ms);
    connect(network, second, server);
    CHECK(server.engine.connection_count() == 2);
}

TEST_CASE("without the reconnect limit an IP may connect again at once", "[raknet][engine][reconnect]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& first = network.add();
    Node& second = network.add();
    second.address = Endpoint(first.address.address(), 60001);

    std::error_code ec;
    first.engine.connect(server.address, {}, ec);
    network.run(1ms);
    second.engine.connect(server.address, {}, ec);
    REQUIRE(network.run_until([&] { return server.count<Connected>() == 2; }, 1s));
    CHECK(second.count<ConnectFailed>() == 0);
}

TEST_CASE("the reconnect limit does not apply to the engine's own IP", "[raknet][engine][reconnect]")
{
    // RakNet exempts its own addresses, among them the external address the first completed
    // handshake reported. Here the server sees itself as 10.0.0.1, the IP these clients share.
    Network network;
    Config config = server_config();
    config.limit_ip_connection_frequency = true;
    Node& server = network.add(config);
    Node& first = network.add();
    Node& second = network.add();
    first.address = Endpoint(server.address.address(), 60001);
    second.address = Endpoint(server.address.address(), 60002);

    const TimePoint start = network.now;
    connect(network, first, server);
    connect(network, second, server);
    CHECK(network.now - start < 100ms); // the second attempt came within the window
}

// --- delivering ----------------------------------------------------------------------------------

TEST_CASE("every reliability delivers on a clean link", "[raknet][engine]")
{
    Session s;
    const Reliability all[] = {Reliability::unreliable, Reliability::unreliable_sequenced, Reliability::reliable,
                               Reliability::reliable_ordered, Reliability::reliable_sequenced};
    std::uint32_t index = 0;
    for (Reliability reliability : all)
    {
        send(s.client, s.at_client, user_message(index++), reliability, 3);
        send(s.server, s.at_server, user_message(index++), reliability, 3);
    }
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 5 && s.client.count<Message>() == 5; }));

    const auto at_server = s.server.all<Message>();
    for (std::size_t i = 0; i < 5; ++i)
    {
        CHECK(at_server[i].connection == s.at_server);
        CHECK(at_server[i].reliability == all[i]);
        CHECK(index_of(at_server[i].data) == i * 2);
    }
    // Ordered and sequenced messages report the channel they travelled on.
    CHECK(at_server[1].channel == 3);
    CHECK(at_server[3].channel == 3);
    CHECK(at_server[4].channel == 3);
}

TEST_CASE("reliable ordered messages survive loss, duplication and reordering", "[raknet][engine]")
{
    Session s;
    s.network.loss = 0.2;
    s.network.duplication = 0.05;
    s.network.jitter = 20ms;

    const std::uint32_t count = 300;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        send(s.client, s.at_client, user_message(i, 40), Reliability::reliable_ordered, 0);
        send(s.client, s.at_client, user_message(i, 40), Reliability::reliable_ordered, 1);
        send(s.server, s.at_server, user_message(i, 40), Reliability::reliable_ordered, 0);
    }
    REQUIRE(s.network.run_until(
        [&] { return s.server.count<Message>() == count * 2 && s.client.count<Message>() == count; }, 120s));

    // Exactly once and in order, independently per channel and per direction.
    std::map<std::uint8_t, std::uint32_t> next;
    for (const Message& message : s.server.all<Message>())
        CHECK(index_of(message.data) == next[message.channel]++);
    CHECK(next[0] == count);
    CHECK(next[1] == count);
    std::uint32_t expected = 0;
    for (const Bytes& message : s.client.messages())
        CHECK(index_of(message) == expected++);

    s.network.run(2s); // nothing arrives late or twice
    CHECK(s.server.count<Message>() == count * 2);
    CHECK(s.client.count<Message>() == count);
}

TEST_CASE("reliable unordered messages arrive exactly once", "[raknet][engine]")
{
    Session s;
    s.network.loss = 0.15;
    s.network.duplication = 0.3;
    s.network.jitter = 30ms;

    const std::uint32_t count = 200;
    for (std::uint32_t i = 0; i < count; ++i)
        send(s.client, s.at_client, user_message(i), Reliability::reliable);
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == count; }, 120s));
    s.network.run(2s);

    std::vector<std::uint32_t> seen;
    for (const Bytes& message : s.server.messages())
        seen.push_back(index_of(message));
    std::sort(seen.begin(), seen.end());
    std::vector<std::uint32_t> all(count);
    std::iota(all.begin(), all.end(), 0u);
    CHECK(seen == all);
}

TEST_CASE("sequenced messages never go backwards", "[raknet][engine]")
{
    for (Reliability reliability : {Reliability::unreliable_sequenced, Reliability::reliable_sequenced})
    {
        Session s;
        s.network.jitter = 30ms; // heavy reordering
        s.network.loss = 0.1;

        const std::uint32_t count = 200;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            send(s.client, s.at_client, user_message(i), reliability, 5);
            s.network.step(); // spread the sends so the jitter can reorder them
        }
        // A final reliable ordered message on the same channel marks the end of the stream.
        send(s.client, s.at_client, user_message(count), Reliability::reliable_ordered, 5);
        REQUIRE(s.network.run_until(
            [&] { return !s.server.messages().empty() && index_of(s.server.messages().back()) == count; }, 60s));

        const std::vector<Bytes> messages = s.server.messages();
        CAPTURE(static_cast<int>(reliability), messages.size());
        CHECK(messages.size() > 10);       // plenty got through...
        CHECK(messages.size() < count + 1); // ...but stale ones were dropped
        for (std::size_t i = 1; i < messages.size(); ++i)
            CHECK(index_of(messages[i]) > index_of(messages[i - 1]));
    }
}

TEST_CASE("ordering channels do not block each other", "[raknet][engine]")
{
    Session s;
    // Lose exactly the datagram carrying the first message on channel 0.
    bool dropped = false;
    s.network.tap = [&](const Network::Datagram& datagram) {
        if (!dropped && datagram.from == s.client.address && datagram.data.size() > 10)
        {
            dropped = true;
            s.network.loss = 1.0;
        }
        else
            s.network.loss = 0.0;
    };
    send(s.client, s.at_client, user_message(0), Reliability::reliable_ordered, 0);
    s.network.step();
    send(s.client, s.at_client, user_message(1), Reliability::reliable_ordered, 0);
    send(s.client, s.at_client, user_message(100), Reliability::reliable_ordered, 1);

    // Channel 1 is delivered while channel 0 still waits for the retransmission.
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() >= 1; }, 500ms));
    CHECK(index_of(s.server.messages()[0]) == 100);
    CHECK(s.server.count<Message>() == 1);

    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 3; }));
    CHECK(index_of(s.server.messages()[1]) == 0);
    CHECK(index_of(s.server.messages()[2]) == 1);
}

TEST_CASE("large messages are split and reassembled", "[raknet][engine]")
{
    Session s;
    s.network.loss = 0.1;
    s.network.jitter = 5ms;
    std::size_t largest = 0;
    s.network.tap = [&](const Network::Datagram& datagram) { largest = std::max(largest, datagram.data.size()); };

    const Bytes to_server = user_message(1, 200'000);
    const Bytes to_client = user_message(2, 123'457);
    send(s.client, s.at_client, to_server, Reliability::reliable_ordered);
    send(s.server, s.at_server, to_client, Reliability::unreliable); // upgraded to reliable when split
    // Ordering still holds around a split message.
    send(s.client, s.at_client, user_message(3), Reliability::reliable_ordered);

    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 2 && s.client.count<Message>() == 1; },
                                120s));
    CHECK(s.server.messages()[0] == to_server);
    CHECK(index_of(s.server.messages()[1]) == 3);
    CHECK(s.client.messages()[0] == to_client);
    CHECK(s.client.all<Message>()[0].reliability == Reliability::reliable);
    CHECK(largest <= 1492 - 28); // never more than one MTU per datagram
}

TEST_CASE("a megabyte goes through quickly once the window has opened", "[raknet][engine]")
{
    Session s;
    const Bytes big = user_message(7, 1024 * 1024);
    send(s.client, s.at_client, big);
    const TimePoint start = s.network.now;
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 1; }, 60s));
    CHECK(s.server.messages()[0] == big);
    // ~730 datagrams over a 10 ms round trip: slow start has to ramp up, not crawl one per round trip.
    CHECK(s.network.now - start < 2s);
}

TEST_CASE("messages larger than the configured limit are refused", "[raknet][engine]")
{
    Network network;
    Config config = server_config();
    config.max_message_size = 10'000;
    Node& server = network.add(config);
    Node& client = network.add(config);
    const auto ids = connect(network, client, server);

    SendOptions options;
    std::error_code ec;
    client.engine.send(ids.first, user_message(1, 10'001), options, ec);
    CHECK(ec == make_error_code(Errc::message_too_large));
    client.engine.send(ids.first, user_message(1, 10'000), options, ec);
    CHECK_FALSE(ec);
    REQUIRE(network.run_until([&] { return server.count<Message>() == 1; }));
}

TEST_CASE("send receipts report delivery and loss", "[raknet][engine]")
{
    Session s;
    send(s.client, s.at_client, user_message(1), Reliability::reliable, 0, 11);
    send(s.client, s.at_client, user_message(2), Reliability::unreliable, 0, 12);
    send(s.client, s.at_client, user_message(3, 20'000), Reliability::reliable_ordered, 0, 13); // split
    REQUIRE(s.network.run_until([&] { return s.client.count<SendReceipt>() == 3; }));
    for (const SendReceipt& receipt : s.client.all<SendReceipt>())
    {
        CHECK(receipt.connection == s.at_client);
        CHECK(receipt.delivered);
    }

    // An unreliable message that vanishes is reported lost.
    s.network.loss = 1.0;
    send(s.client, s.at_client, user_message(4), Reliability::unreliable, 0, 14);
    REQUIRE(s.network.run_until([&] { return s.client.count<SendReceipt>() == 4; }, 3s));
    const SendReceipt lost = s.client.all<SendReceipt>().back();
    CHECK(lost.token == 14);
    CHECK_FALSE(lost.delivered);

    // RakNet has no receipts for sequenced messages.
    SendOptions options;
    options.reliability = Reliability::reliable_sequenced;
    options.receipt_token = 15;
    std::error_code ec;
    s.client.engine.send(s.at_client, user_message(5), options, ec);
    CHECK(ec == make_error_code(Errc::invalid_argument));
}

TEST_CASE("send rejects invalid requests", "[raknet][engine]")
{
    Session s;
    SendOptions options;
    std::error_code ec;

    s.client.engine.send(999, user_message(1), options, ec);
    CHECK(ec == make_error_code(Errc::unknown_connection));
    s.client.engine.send(s.at_client, Bytes{}, options, ec);
    CHECK(ec == make_error_code(Errc::invalid_argument));
    // A payload starting with an id the protocol consumes itself would never reach the application.
    for (std::uint8_t reserved : {id::connected_ping, id::connected_pong, id::connection_request,
                                  id::connection_request_accepted, id::new_incoming_connection,
                                  id::disconnection_notification, id::invalid_password})
    {
        s.client.engine.send(s.at_client, Bytes{reserved, 1, 2}, options, ec);
        CHECK(ec == make_error_code(Errc::reserved_message_id));
    }
    options.channel = 32;
    s.client.engine.send(s.at_client, user_message(1), options, ec);
    CHECK(ec == make_error_code(Errc::invalid_argument));

    // Not established yet.
    Node& late = s.network.add();
    const ConnectionId pending = late.engine.connect(s.server.address, {}, ec);
    REQUIRE_FALSE(ec);
    late.engine.send(pending, user_message(1), SendOptions(), ec);
    CHECK(ec == make_error_code(Errc::not_connected));
}

// --- disconnecting -------------------------------------------------------------------------------

TEST_CASE("a graceful disconnect flushes queued data and notifies the peer", "[raknet][engine]")
{
    Session s;
    s.network.loss = 0.1;
    for (std::uint32_t i = 0; i < 50; ++i)
        send(s.client, s.at_client, user_message(i));
    s.client.engine.disconnect(s.at_client, DisconnectMode::graceful);

    // The local side is told at once and the id is gone.
    s.network.step();
    REQUIRE(s.client.count<Disconnected>() == 1);
    CHECK(s.client.all<Disconnected>()[0].reason == DisconnectReason::closed_locally);
    CHECK(s.client.all<Disconnected>()[0].connection == s.at_client);
    CHECK(s.client.engine.connection_count() == 0);
    std::error_code ec;
    s.client.engine.send(s.at_client, user_message(99), SendOptions(), ec);
    CHECK(ec == make_error_code(Errc::unknown_connection));

    REQUIRE(s.network.run_until([&] { return s.server.count<Disconnected>() == 1; }));
    CHECK(s.server.all<Disconnected>()[0].reason == DisconnectReason::closed_by_peer);
    CHECK(s.server.all<Disconnected>()[0].connection == s.at_server);
    // Everything sent before the disconnect arrived first, in order.
    REQUIRE(s.server.count<Message>() == 50);
    CHECK(std::holds_alternative<Disconnected>(s.server.events.back()));
    CHECK(s.server.engine.connection_count() == 0);

    // Both sides forget the connection entirely and go quiet.
    s.network.run(12s);
    const std::size_t sent = s.network.datagrams_sent;
    s.network.run(12s);
    CHECK(s.network.datagrams_sent == sent);
    CHECK_FALSE(s.client.engine.next_deadline());
    CHECK_FALSE(s.server.engine.next_deadline());
}

TEST_CASE("a peer can reconnect right after disconnecting", "[raknet][engine]")
{
    Session s;
    s.client.engine.disconnect(s.at_client, DisconnectMode::graceful);
    const auto ids = connect(s.network, s.client, s.server);
    CHECK(ids.first != s.at_client); // ids are never reused
    CHECK(ids.second != s.at_server);

    send(s.client, ids.first, user_message(1));
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 1; }));
    CHECK(s.server.all<Message>()[0].connection == ids.second);
}

TEST_CASE("a silent peer is detected by time-out", "[raknet][engine]")
{
    Session s;
    s.network.run(1s);
    s.network.loss = 1.0; // the cable is cut

    const TimePoint cut = s.network.now;
    REQUIRE(s.network.run_until(
        [&] { return s.client.count<Disconnected>() == 1 && s.server.count<Disconnected>() == 1; }, 40s));
    CHECK(s.client.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
    CHECK(s.server.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
    // Idle connections notice through the keep-alive: at most timeout/2 until the next reliable
    // ping plus the timeout itself.
    CHECK(s.network.now - cut <= 16s);
    CHECK(s.client.engine.connection_count() == 0);
    CHECK(s.server.engine.connection_count() == 0);
}

TEST_CASE("a connection with a shorter time-out drops while others survive the same stall", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& impatient = network.add();
    Node& patient = network.add();
    const auto short_lived = connect(network, impatient, server);
    const auto long_lived = connect(network, patient, server);

    std::error_code ec;
    server.engine.set_timeout(short_lived.second, 2s, ec);
    REQUIRE_FALSE(ec);
    CHECK(server.engine.connection_info(short_lived.second)->timeout == 2s);
    CHECK(server.engine.connection_info(long_lived.second)->timeout == 10s);

    network.run(1s);
    network.loss = 1.0; // every connection stalls
    const TimePoint cut = network.now;
    REQUIRE(network.run_until([&] { return server.count<Disconnected>() == 1; }, 20s));
    CHECK(network.now - cut <= 4s); // within keep-alive interval (timeout/2) plus the timeout
    CHECK(server.all<Disconnected>()[0].connection == short_lived.second);
    CHECK(server.all<Disconnected>()[0].reason == DisconnectReason::timed_out);

    // Long after the impatient connection gave up, the patient one rides out the stall.
    network.run(4s);
    network.loss = 0.0;
    CHECK(server.count<Disconnected>() == 1);
    send(patient, long_lived.first, user_message(1));
    REQUIRE(network.run_until([&] { return server.count<Message>() == 1; }));
    CHECK(server.all<Message>()[0].connection == long_lived.second);
}

TEST_CASE("the time-out can be changed for every connection at once", "[raknet][engine]")
{
    Session s;
    s.server.engine.set_timeout(s.at_server, 30s);
    s.server.engine.set_timeout(3s); // like RakNet's SetTimeoutTime with UNASSIGNED_SYSTEM_ADDRESS
    CHECK(s.server.engine.connection_info(s.at_server)->timeout == 3s);

    Node& late = s.network.add();
    const auto ids = connect(s.network, late, s.server);
    CHECK(s.server.engine.connection_info(ids.second)->timeout == 3s);
}

TEST_CASE("set_timeout rejects unknown connections and non-positive durations", "[raknet][engine]")
{
    Session s;
    std::error_code ec;
    s.server.engine.set_timeout(ConnectionId(9999), 1s, ec);
    CHECK(ec == Errc::unknown_connection);
    s.server.engine.set_timeout(s.at_server, 0ms, ec);
    CHECK(ec == Errc::invalid_argument);
    CHECK(s.server.engine.connection_info(s.at_server)->timeout == 10s);
}

TEST_CASE("each side learns its external address from the handshake", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();

    std::error_code ec;
    const ConnectionId pending = client.engine.connect(server.address, {}, ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(client.engine.connection_info(pending)->external_address);

    REQUIRE(network.run_until([&] { return client.count<Connected>() == 1 && server.count<Connected>() == 1; }));
    const auto client_info = client.engine.connection_info(pending);
    const auto server_info = server.engine.connection_info(server.all<Connected>()[0].connection);
    REQUIRE(client_info->external_address);
    REQUIRE(server_info->external_address);
    CHECK(*client_info->external_address == client.address);
    CHECK(*server_info->external_address == server.address);
}

TEST_CASE("an abrupt disconnect leaves the peer to time out", "[raknet][engine]")
{
    Session s;
    s.client.engine.disconnect(s.at_client, DisconnectMode::abrupt);
    s.network.step();
    CHECK(s.client.count<Disconnected>() == 1);
    CHECK(s.client.engine.connection_count() == 0);

    REQUIRE(s.network.run_until([&] { return s.server.count<Disconnected>() == 1; }, 40s));
    CHECK(s.server.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
}

TEST_CASE("an idle connection stays alive on keep-alives", "[raknet][engine]")
{
    Session s;
    s.network.run(60s, 5ms);
    CHECK(s.client.count<Disconnected>() == 0);
    CHECK(s.server.count<Disconnected>() == 0);
    CHECK(s.client.count<Message>() == 0); // keep-alive traffic is invisible to the application

    send(s.client, s.at_client, user_message(1));
    REQUIRE(s.network.run_until([&] { return s.server.count<Message>() == 1; }));
}

TEST_CASE("next_deadline reflects pending work", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    Node& client = network.add();
    CHECK_FALSE(client.engine.next_deadline()); // nothing to do

    const auto ids = connect(network, client, server);
    network.run(500ms);
    // Idle: the next thing on the agenda is the keep-alive, seconds away.
    REQUIRE(client.engine.next_deadline());
    CHECK(*client.engine.next_deadline() > network.now + 3s);

    send(client, ids.first, user_message(1));
    REQUIRE(client.engine.next_deadline());
    CHECK(*client.engine.next_deadline() <= network.now); // queued data: as soon as possible
}

// --- connectionless ------------------------------------------------------------------------------

TEST_CASE("unconnected pings are answered with the offline response", "[raknet][engine]")
{
    Network network;
    Config config = server_config();
    config.offline_ping_response = {'m', 'o', 't', 'd'};
    Node& server = network.add(config);
    Node& closed = network.add(); // accepts no connections
    Node& client = network.add();

    client.engine.ping(server.address);
    client.engine.ping(closed.address, true); // "only if accepting": stays unanswered
    network.run(100ms);

    REQUIRE(client.count<Unconnected>() == 1);
    const Unconnected reply = client.all<Unconnected>()[0];
    CHECK(reply.from == server.address);
    wire::UnconnectedPong pong;
    REQUIRE(wire::decode(reply.data, pong));
    CHECK(pong.guid == server.engine.guid());
    CHECK(pong.data == config.offline_ping_response);
    // The ping time is echoed, so the round trip can be measured.
    CHECK(RakNetEngine::wire_time(network.now) - pong.time <= 150);

    // A plain ping is answered even by a peer that accepts no connections.
    client.engine.ping(closed.address);
    server.engine.set_offline_ping_response(Bytes{'n', 'e', 'w'});
    client.engine.ping(server.address);
    network.run(100ms);
    REQUIRE(client.count<Unconnected>() == 3);
}

TEST_CASE("advertise reaches a peer without a connection", "[raknet][engine]")
{
    Network network;
    Node& a = network.add();
    Node& b = network.add();

    std::error_code ec;
    a.engine.advertise(b.address, Bytes{'h', 'e', 'y'}, ec);
    REQUIRE_FALSE(ec);
    network.run(50ms);

    REQUIRE(b.count<Unconnected>() == 1);
    wire::OutOfBand message;
    REQUIRE(wire::decode(b.all<Unconnected>()[0].data, message));
    CHECK(message.guid == a.engine.guid());
    CHECK(message.data == Bytes{id::advertise_system, 'h', 'e', 'y'});

    a.engine.advertise(b.address, Bytes(400, 0), ec);
    CHECK(ec == make_error_code(Errc::message_too_large));
}

// --- hostile input -------------------------------------------------------------------------------

TEST_CASE("garbage datagrams are ignored", "[raknet][engine]")
{
    Session s;
    std::mt19937 random(42);
    const Endpoint stranger(IpAddress::v4(10, 9, 9, 9), 1234);

    std::vector<Bytes> captured;
    s.network.tap = [&](const Network::Datagram& datagram) { captured.push_back(datagram.data); };
    send(s.client, s.at_client, user_message(0, 3000));
    s.network.run(100ms);
    s.network.tap = nullptr;
    REQUIRE(captured.size() > 3);

    for (int round = 0; round < 2000; ++round)
    {
        Bytes junk(random() % 80);
        for (std::uint8_t& byte : junk)
            byte = static_cast<std::uint8_t>(random());
        if (!junk.empty() && round % 2 == 0)
            junk[0] |= 0x80; // look like a connected datagram half of the time

        // Truncated and bit-flipped copies of real traffic reach deeper into the parsers.
        Bytes mangled = captured[random() % captured.size()];
        mangled.resize(random() % (mangled.size() + 1));
        if (!mangled.empty())
            mangled[random() % mangled.size()] ^= static_cast<std::uint8_t>(1u << (random() % 8));

        s.network.inject(stranger, s.server.address, junk);
        s.network.inject(stranger, s.server.address, mangled);
        s.network.inject(s.client.address, s.server.address, junk);
        if (round % 50 == 0)
            s.network.step();
    }
    s.network.run(200ms);

    // The connection may have been upset by forged datagrams from the client's own address, but
    // nothing crashed, and an untouched pair still works.
    Session fresh;
    send(fresh.client, fresh.at_client, user_message(1));
    REQUIRE(fresh.network.run_until([&] { return fresh.server.count<Message>() == 1; }));
    CHECK(s.server.engine.connection_count() <= 1);
}

TEST_CASE("an absurd split count drops the connection instead of allocating", "[raknet][engine]")
{
    Session s;

    Bytes datagram;
    ByteWriter writer(datagram);
    wire::DatagramHeader datagram_header;
    datagram_header.number = 1000;
    wire::write_datagram_header(writer, datagram_header);
    wire::MessageHeader message;
    message.reliability = wire::Reliability::unreliable;
    message.split = true;
    message.split_count = 0xFFFFFFFFu;
    message.split_index = 0;
    message.bit_length = 8;
    wire::write_message_header(writer, message);
    writer.u8(0x86);

    s.network.inject(s.client.address, s.server.address, datagram);
    REQUIRE(s.network.run_until([&] { return s.server.count<Disconnected>() == 1; }, 1s));
    CHECK(s.server.all<Disconnected>()[0].reason == DisconnectReason::protocol_violation);
    CHECK(s.server.engine.connection_count() == 0);
}

TEST_CASE("a sender that never completes its split messages cannot exhaust memory", "[raknet][engine]")
{
    Network network;
    Config config = server_config();
    config.max_message_size = 64 * 1024;
    Node& server = network.add(config);
    Node& client = network.add();
    connect(network, client, server);

    // Open ever more split messages, one small chunk each, and never finish any of them.
    for (std::uint32_t i = 0; i < 2000 && server.count<Disconnected>() == 0; ++i)
    {
        Bytes datagram;
        ByteWriter writer(datagram);
        wire::DatagramHeader datagram_header;
        datagram_header.number = 100 + i;
        wire::write_datagram_header(writer, datagram_header);
        wire::MessageHeader message;
        message.reliability = wire::Reliability::unreliable;
        message.split = true;
        message.split_count = 1000;
        message.split_id = static_cast<std::uint16_t>(i);
        message.split_index = 0;
        message.bit_length = 8;
        wire::write_message_header(writer, message);
        writer.u8(0x86);
        network.inject(client.address, server.address, datagram);
        network.step();
    }
    network.run(50ms);
    REQUIRE(server.count<Disconnected>() == 1);
    CHECK(server.all<Disconnected>()[0].reason == DisconnectReason::protocol_violation);
}

TEST_CASE("an unverified peer must present a connection request first", "[raknet][engine]")
{
    Network network;
    Node& server = network.add(server_config());
    const Endpoint stranger(IpAddress::v4(10, 9, 9, 9), 1234);
    std::vector<Bytes> replies;
    network.tap = [&](const Network::Datagram& datagram) {
        if (datagram.to == stranger)
            replies.push_back(datagram.data);
    };

    // Complete the offline part by hand...
    wire::OpenConnectionRequest2 request;
    request.server_address = server.address;
    request.mtu = 1492;
    request.guid = 77;
    network.inject(stranger, server.address, wire::encode(request));
    network.run(20ms);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0][0] == id::open_connection_reply_2);

    // ...then send a user message instead of ID_CONNECTION_REQUEST.
    Bytes datagram;
    ByteWriter writer(datagram);
    wire::DatagramHeader datagram_header;
    wire::write_datagram_header(writer, datagram_header);
    wire::MessageHeader message;
    message.reliability = wire::Reliability::reliable;
    message.bit_length = 16;
    wire::write_message_header(writer, message);
    writer.bytes(Bytes{0x86, 0x01});
    network.inject(stranger, server.address, datagram);
    network.run(100ms);

    CHECK(server.count<Connected>() == 0);
    CHECK(server.count<Message>() == 0);
    // The half-open slot is released at once, not held until some time-out.
    CHECK_FALSE(server.engine.next_deadline());
}
