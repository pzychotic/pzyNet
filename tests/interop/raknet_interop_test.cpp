// Live wire-compatibility tests: a pzyNet RakNetEngine (driven by UdpHost over real loopback UDP)
// talks to a real RakNet peer built from the checkout named by PZYNET_RAKNET_INTEROP_DIR.
// Most scenarios run in both directions: pzyNet connecting to RakNet, and RakNet connecting to pzyNet.

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/raknet_engine.hpp"
#include "pzynet/raknet/wire.hpp"
#include "pzynet/transport/udp_host.hpp"

#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace pzynet::transport;
using namespace std::chrono_literals;
using namespace RakNet; // message ids, priorities and reliabilities of the reference implementation
using Bytes = std::vector<std::uint8_t>;
namespace rn = pzynet::raknet;

namespace
{

enum class Direction
{
    pzynet_connects,
    raknet_connects
};

struct RakPacket
{
    unsigned char id = 0;
    Bytes data;
    RakNet::SystemAddress address;
    RakNet::RakNetGUID guid;
};

Bytes user_message(std::uint32_t index, std::size_t size = 5)
{
    Bytes bytes;
    ByteWriter writer(bytes);
    writer.u8(rn::id::user_packet_enum);
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

PacketReliability to_raknet(Reliability reliability)
{
    switch (reliability)
    {
    case Reliability::unreliable: return UNRELIABLE;
    case Reliability::unreliable_sequenced: return UNRELIABLE_SEQUENCED;
    case Reliability::reliable: return RELIABLE;
    case Reliability::reliable_ordered: return RELIABLE_ORDERED;
    case Reliability::reliable_sequenced: return RELIABLE_SEQUENCED;
    }
    return RELIABLE;
}

/// One pzyNet endpoint and one RakNet peer on loopback.
struct Fixture
{
    rn::RakNetEngine engine;
    UdpHost host{engine};
    RakNet::RakPeerInterface* rak = RakNet::RakPeerInterface::GetInstance();

    std::vector<Event> events;      // from pzyNet
    std::vector<RakPacket> packets; // from RakNet

    ConnectionId connection = invalid_connection; // pzyNet's id for the connection to the RakNet peer
    RakNet::SystemAddress pzynet_address;         // the pzyNet endpoint as RakNet sees it

    explicit Fixture(rn::Config config = pzynet_config()) : engine(std::move(config))
    {
        host.bind(Endpoint(IpAddress::loopback_v4(), 0));
    }
    ~Fixture()
    {
        rak->Shutdown(0);
        RakNet::RakPeerInterface::DestroyInstance(rak);
    }

    static rn::Config pzynet_config()
    {
        rn::Config config;
        config.max_incoming_connections = 4;
        return config;
    }

    void start_raknet(unsigned short max_incoming = 4)
    {
        RakNet::SocketDescriptor descriptor(0, "127.0.0.1");
        REQUIRE(rak->Startup(8, &descriptor, 1) == RakNet::RAKNET_STARTED);
        rak->SetMaximumIncomingConnections(max_incoming);
    }

    Endpoint raknet_endpoint() { return Endpoint(IpAddress::loopback_v4(), rak->GetMyBoundAddress(0).GetPort()); }

    /// Moves traffic on both sides for one short slice and collects what each side reports.
    void pump_once()
    {
        host.update(2ms);
        Event event;
        while (host.poll_event(event))
            events.push_back(std::move(event));
        while (RakNet::Packet* packet = rak->Receive())
        {
            RakPacket copy;
            copy.id = packet->data[0];
            copy.data.assign(packet->data, packet->data + packet->length);
            copy.address = packet->systemAddress;
            copy.guid = packet->guid;
            packets.push_back(std::move(copy));
            rak->DeallocatePacket(packet);
        }
    }

    template <class Predicate>
    bool pump_until(Predicate done, std::chrono::milliseconds limit = 15s)
    {
        const auto give_up = std::chrono::steady_clock::now() + limit;
        for (;;)
        {
            pump_once();
            if (done())
                return true;
            if (std::chrono::steady_clock::now() >= give_up)
                return false;
        }
    }

    void pump_for(std::chrono::milliseconds duration)
    {
        pump_until([] { return false; }, duration);
    }

    // --- what pzyNet reported ---
    template <class T>
    std::vector<T> all() const
    {
        std::vector<T> found;
        for (const Event& event : events)
            if (const T* typed = std::get_if<T>(&event))
                found.push_back(*typed);
        return found;
    }
    template <class T>
    std::size_t count() const
    {
        return all<T>().size();
    }

    // --- what RakNet reported ---
    std::size_t rak_count(unsigned char id) const
    {
        return static_cast<std::size_t>(
            std::count_if(packets.begin(), packets.end(), [id](const RakPacket& p) { return p.id == id; }));
    }
    const RakPacket& rak_last(unsigned char id) const
    {
        const auto it = std::find_if(packets.rbegin(), packets.rend(), [id](const RakPacket& p) { return p.id == id; });
        REQUIRE(it != packets.rend());
        return *it;
    }
    std::vector<Bytes> rak_user_messages() const
    {
        std::vector<Bytes> found;
        for (const RakPacket& packet : packets)
            if (packet.id >= ID_USER_PACKET_ENUM)
                found.push_back(packet.data);
        return found;
    }

    /// Establishes the connection in the given direction, with optional password on both sides.
    void establish(Direction direction)
    {
        start_raknet();
        if (direction == Direction::pzynet_connects)
        {
            connection = host.connect(raknet_endpoint());
            REQUIRE(pump_until([&] { return count<Connected>() == 1 && rak_count(ID_NEW_INCOMING_CONNECTION) == 1; }));
            pzynet_address = rak_last(ID_NEW_INCOMING_CONNECTION).address;
            CHECK_FALSE(all<Connected>()[0].incoming);
        }
        else
        {
            REQUIRE(rak->Connect("127.0.0.1", host.local_endpoint().port(), nullptr, 0) ==
                    RakNet::CONNECTION_ATTEMPT_STARTED);
            REQUIRE(pump_until(
                [&] { return count<Connected>() == 1 && rak_count(ID_CONNECTION_REQUEST_ACCEPTED) == 1; }));
            pzynet_address = rak_last(ID_CONNECTION_REQUEST_ACCEPTED).address;
            connection = all<Connected>()[0].connection;
            CHECK(all<Connected>()[0].incoming);
        }
        REQUIRE(connection != invalid_connection);
    }

    void send_from_pzynet(const Bytes& data, Reliability reliability = Reliability::reliable_ordered,
                          std::uint8_t channel = 0, std::uint32_t receipt_token = 0)
    {
        SendOptions options;
        options.reliability = reliability;
        options.channel = channel;
        options.receipt_token = receipt_token;
        host.send(connection, data, options);
    }

    void send_from_raknet(const Bytes& data, Reliability reliability = Reliability::reliable_ordered,
                          std::uint8_t channel = 0)
    {
        const std::uint32_t result =
            rak->Send(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), HIGH_PRIORITY,
                      to_raknet(reliability), static_cast<char>(channel), pzynet_address, false);
        REQUIRE(result != 0);
    }
};

const char* name(Direction direction)
{
    return direction == Direction::pzynet_connects ? "pzyNet connects to RakNet" : "RakNet connects to pzyNet";
}

} // namespace

TEST_CASE("handshake with a real RakNet peer", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    // Both sides learned each other's identity and agreed on the MTU.
    const auto info = f.engine.connection_info(f.connection);
    REQUIRE(info);
    CHECK(info->remote == f.raknet_endpoint());
    CHECK(info->remote_guid == f.rak->GetMyGUID().g);
    CHECK(info->mtu == 1492);
    CHECK(f.rak->GetMTUSize(f.pzynet_address) == 1492);
    CHECK(f.rak->GetGuidFromSystemAddress(f.pzynet_address).g == f.engine.guid());
    CHECK(f.pzynet_address.GetPort() == f.host.local_endpoint().port());
    CHECK(f.rak->GetConnectionState(f.pzynet_address) == RakNet::IS_CONNECTED);
}

TEST_CASE("every reliability is understood in both directions", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const Reliability all[] = {Reliability::unreliable, Reliability::unreliable_sequenced, Reliability::reliable,
                               Reliability::reliable_ordered, Reliability::reliable_sequenced};
    for (std::uint32_t i = 0; i < 5; ++i)
    {
        // One at a time: on loopback nothing is lost, and this keeps the expected order unambiguous.
        f.send_from_pzynet(user_message(i, 20), all[i], static_cast<std::uint8_t>(i));
        f.send_from_raknet(user_message(100 + i, 20), all[i], static_cast<std::uint8_t>(i));
        REQUIRE(f.pump_until([&] { return f.rak_user_messages().size() == i + 1 && f.count<Message>() == i + 1; }));

        CHECK(f.rak_user_messages()[i] == user_message(i, 20));
        const Message received = f.all<Message>()[i];
        CHECK(received.data == user_message(100 + i, 20));
        CHECK(received.reliability == all[i]);
        CHECK(received.connection == f.connection);
    }
}

TEST_CASE("ordered streams keep their order across many messages", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const std::uint32_t count = 2000;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto channel = static_cast<std::uint8_t>(i % 3);
        f.send_from_pzynet(user_message(i, 60), Reliability::reliable_ordered, channel);
        f.send_from_raknet(user_message(i, 60), Reliability::reliable_ordered, channel);
        if (i % 100 == 0)
            f.pump_once();
    }
    REQUIRE(f.pump_until([&] { return f.rak_user_messages().size() == count && f.count<Message>() == count; }, 30s));

    // Per channel, indices must be strictly increasing; overall, nothing is missing or duplicated.
    std::map<std::uint32_t, std::uint32_t> last_at_raknet, last_at_pzynet;
    for (const Bytes& message : f.rak_user_messages())
    {
        const std::uint32_t index = index_of(message);
        const std::uint32_t channel = index % 3;
        if (last_at_raknet.count(channel))
            CHECK(index == last_at_raknet[channel] + 3);
        last_at_raknet[channel] = index;
    }
    for (const Message& message : f.all<Message>())
    {
        const std::uint32_t index = index_of(message.data);
        CHECK(message.channel == index % 3);
        if (last_at_pzynet.count(message.channel))
            CHECK(index == last_at_pzynet[message.channel] + 3);
        last_at_pzynet[message.channel] = index;
    }
}

TEST_CASE("large messages are split by one side and reassembled by the other", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const Bytes to_raknet = user_message(1, 300'000);
    const Bytes to_pzynet = user_message(2, 250'001);
    f.send_from_pzynet(to_raknet);
    f.send_from_raknet(to_pzynet);
    // A small ordered message behind each big one must still arrive after it.
    f.send_from_pzynet(user_message(3));
    f.send_from_raknet(user_message(4));

    REQUIRE(f.pump_until([&] { return f.rak_user_messages().size() == 2 && f.count<Message>() == 2; }, 60s));
    CHECK(f.rak_user_messages()[0] == to_raknet);
    CHECK(f.rak_user_messages()[1] == user_message(3));
    CHECK(f.all<Message>()[0].data == to_pzynet);
    CHECK(f.all<Message>()[1].data == user_message(4));
}

TEST_CASE("bulk transfer in both directions at once", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const std::uint32_t count = 40;
    const std::size_t size = 50'000; // 2 MB each way, every message split into ~35 datagrams
    for (std::uint32_t i = 0; i < count; ++i)
    {
        f.send_from_pzynet(user_message(i, size), Reliability::reliable_ordered, 1);
        f.send_from_raknet(user_message(i, size), Reliability::reliable_ordered, 1);
    }
    REQUIRE(f.pump_until([&] { return f.rak_user_messages().size() == count && f.count<Message>() == count; }, 120s));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        CHECK(f.rak_user_messages()[i] == user_message(i, size));
        CHECK(f.all<Message>()[i].data == user_message(i, size));
    }
}

TEST_CASE("send receipts work against a RakNet peer", "[interop]")
{
    Fixture f;
    f.establish(Direction::pzynet_connects);

    f.send_from_pzynet(user_message(1), Reliability::reliable, 0, 21);
    f.send_from_pzynet(user_message(2), Reliability::unreliable, 0, 22);
    f.send_from_pzynet(user_message(3, 10'000), Reliability::reliable_ordered, 0, 23);
    REQUIRE(f.pump_until([&] { return f.count<SendReceipt>() == 3; }));
    for (const SendReceipt& receipt : f.all<SendReceipt>())
        CHECK(receipt.delivered);
    CHECK(f.rak_user_messages().size() == 3);
}

TEST_CASE("passwords are exchanged compatibly", "[interop]")
{
    const Bytes password{'o', 'p', 'e', 'n', ' ', 's', 'e', 's', 'a', 'm', 'e'};
    const char* password_text = reinterpret_cast<const char*>(password.data());
    const int password_length = static_cast<int>(password.size());

    SECTION("RakNet server accepts the right password from pzyNet")
    {
        Fixture f;
        f.start_raknet();
        f.rak->SetIncomingPassword(password_text, password_length);
        f.host.connect(f.raknet_endpoint(), password);
        REQUIRE(f.pump_until([&] { return f.count<Connected>() == 1 && f.rak_count(ID_NEW_INCOMING_CONNECTION) == 1; }));
    }
    SECTION("RakNet server rejects a wrong password from pzyNet")
    {
        Fixture f;
        f.start_raknet();
        f.rak->SetIncomingPassword(password_text, password_length);
        f.host.connect(f.raknet_endpoint(), Bytes{'n', 'o'});
        REQUIRE(f.pump_until([&] { return f.count<ConnectFailed>() == 1; }));
        CHECK(f.all<ConnectFailed>()[0].reason == ConnectFailure::invalid_credentials);
        CHECK(f.count<Connected>() == 0);
        CHECK(f.rak_count(ID_NEW_INCOMING_CONNECTION) == 0);
    }
    SECTION("pzyNet server accepts the right password from RakNet")
    {
        rn::Config config = Fixture::pzynet_config();
        config.incoming_password = password;
        Fixture f(config);
        f.start_raknet();
        REQUIRE(f.rak->Connect("127.0.0.1", f.host.local_endpoint().port(), password_text, password_length) ==
                RakNet::CONNECTION_ATTEMPT_STARTED);
        REQUIRE(f.pump_until([&] { return f.count<Connected>() == 1 && f.rak_count(ID_CONNECTION_REQUEST_ACCEPTED) == 1; }));
    }
    SECTION("pzyNet server rejects a wrong password from RakNet")
    {
        rn::Config config = Fixture::pzynet_config();
        config.incoming_password = password;
        Fixture f(config);
        f.start_raknet();
        REQUIRE(f.rak->Connect("127.0.0.1", f.host.local_endpoint().port(), "no", 2) ==
                RakNet::CONNECTION_ATTEMPT_STARTED);
        REQUIRE(f.pump_until([&] { return f.rak_count(ID_INVALID_PASSWORD) == 1; }));
        CHECK(f.count<Connected>() == 0);
        CHECK(f.rak_count(ID_CONNECTION_REQUEST_ACCEPTED) == 0);
    }
}

TEST_CASE("a full server refuses in a way the other side understands", "[interop]")
{
    SECTION("RakNet server without free slots")
    {
        Fixture f;
        f.start_raknet(0);
        f.host.connect(f.raknet_endpoint());
        REQUIRE(f.pump_until([&] { return f.count<ConnectFailed>() == 1; }));
        CHECK(f.all<ConnectFailed>()[0].reason == ConnectFailure::no_free_slots);
    }
    SECTION("pzyNet server without free slots")
    {
        rn::Config config;
        config.max_incoming_connections = 0;
        Fixture f(config);
        f.start_raknet();
        REQUIRE(f.rak->Connect("127.0.0.1", f.host.local_endpoint().port(), nullptr, 0) ==
                RakNet::CONNECTION_ATTEMPT_STARTED);
        REQUIRE(f.pump_until([&] { return f.rak_count(ID_NO_FREE_INCOMING_CONNECTIONS) == 1; }));
        CHECK(f.count<Connected>() == 0);
    }
}

TEST_CASE("graceful disconnects are noticed by the other side", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));

    SECTION("pzyNet disconnects")
    {
        Fixture f;
        f.establish(direction);
        for (std::uint32_t i = 0; i < 20; ++i)
            f.send_from_pzynet(user_message(i));
        f.host.disconnect(f.connection);
        REQUIRE(f.pump_until([&] { return f.rak_count(ID_DISCONNECTION_NOTIFICATION) == 1; }));
        // Data queued before the disconnect was delivered first.
        CHECK(f.rak_user_messages().size() == 20);
        CHECK(f.packets.back().id == ID_DISCONNECTION_NOTIFICATION);
        REQUIRE(f.count<Disconnected>() == 1);
        CHECK(f.all<Disconnected>()[0].reason == DisconnectReason::closed_locally);
        REQUIRE(f.pump_until([&] { return f.rak->GetConnectionState(f.pzynet_address) != RakNet::IS_CONNECTED; }));
    }
    SECTION("RakNet disconnects")
    {
        Fixture f;
        f.establish(direction);
        for (std::uint32_t i = 0; i < 20; ++i)
            f.send_from_raknet(user_message(i));
        f.rak->CloseConnection(f.pzynet_address, true);
        REQUIRE(f.pump_until([&] { return f.count<Disconnected>() == 1; }));
        CHECK(f.all<Disconnected>()[0].reason == DisconnectReason::closed_by_peer);
        CHECK(f.count<Message>() == 20);
        CHECK(std::holds_alternative<Disconnected>(f.events.back()));
        CHECK(f.engine.connection_count() == 0);
    }
}

TEST_CASE("a vanished peer is detected by time-out", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));

    SECTION("RakNet vanishes")
    {
        rn::Config config = Fixture::pzynet_config();
        config.timeout = 2000ms;
        Fixture f(config);
        f.establish(direction);
        f.rak->Shutdown(0); // no notification: the peer is simply gone
        REQUIRE(f.pump_until([&] { return f.count<Disconnected>() == 1; }, 10s));
        CHECK(f.all<Disconnected>()[0].reason == DisconnectReason::timed_out);
    }
    SECTION("pzyNet vanishes")
    {
        Fixture f;
        f.establish(direction);
        f.rak->SetTimeoutTime(2000, f.pzynet_address);
        // Stop pumping the pzyNet host; only RakNet keeps running.
        const auto give_up = std::chrono::steady_clock::now() + 15s;
        bool lost = false;
        while (!lost && std::chrono::steady_clock::now() < give_up)
        {
            while (RakNet::Packet* packet = f.rak->Receive())
            {
                lost = lost || packet->data[0] == ID_CONNECTION_LOST;
                f.rak->DeallocatePacket(packet);
            }
            std::this_thread::sleep_for(10ms);
        }
        CHECK(lost);
    }
}

TEST_CASE("an idle connection survives several time-out periods", "[interop]")
{
    const Direction direction = GENERATE(Direction::pzynet_connects, Direction::raknet_connects);
    INFO(name(direction));
    rn::Config config = Fixture::pzynet_config();
    config.timeout = 1500ms;
    Fixture f(config);
    f.establish(direction);
    f.rak->SetTimeoutTime(1500, f.pzynet_address);

    f.pump_for(6s); // four time-out periods without any application traffic
    CHECK(f.count<Disconnected>() == 0);
    CHECK(f.rak_count(ID_CONNECTION_LOST) == 0);
    CHECK(f.rak->GetConnectionState(f.pzynet_address) == RakNet::IS_CONNECTED);

    f.send_from_pzynet(user_message(1));
    f.send_from_raknet(user_message(2));
    REQUIRE(f.pump_until([&] { return f.rak_user_messages().size() == 1 && f.count<Message>() == 1; }));

    // The keep-alive pings also produced a round-trip estimate and a clock offset.
    const auto info = f.engine.connection_info(f.connection);
    REQUIRE(info);
    CHECK(info->rtt);
    CHECK(info->clock_offset);
}

TEST_CASE("unconnected ping and pong interoperate", "[interop]")
{
    SECTION("pzyNet pings a RakNet server")
    {
        Fixture f;
        f.start_raknet();
        f.rak->SetOfflinePingResponse("raknet says hi", 14);
        f.engine.ping(f.raknet_endpoint());
        REQUIRE(f.pump_until([&] { return f.count<Unconnected>() == 1; }));

        rn::wire::UnconnectedPong pong;
        REQUIRE(rn::wire::decode(f.all<Unconnected>()[0].data, pong));
        CHECK(pong.guid == f.rak->GetMyGUID().g);
        CHECK(std::string(pong.data.begin(), pong.data.end()) == "raknet says hi");
        CHECK(f.all<Unconnected>()[0].from == f.raknet_endpoint());
    }
    SECTION("RakNet pings a pzyNet server")
    {
        rn::Config config = Fixture::pzynet_config();
        config.offline_ping_response = {'p', 'z', 'y'};
        Fixture f(config);
        f.start_raknet();
        REQUIRE(f.rak->Ping("127.0.0.1", f.host.local_endpoint().port(), false));
        REQUIRE(f.pump_until([&] { return f.rak_count(ID_UNCONNECTED_PONG) == 1; }));

        // RakNet hands its application: id, 4-byte time, then the response payload.
        const RakPacket& pong = f.rak_last(ID_UNCONNECTED_PONG);
        REQUIRE(pong.data.size() == 1 + 4 + 3);
        CHECK(Bytes(pong.data.end() - 3, pong.data.end()) == Bytes{'p', 'z', 'y'});
        CHECK(pong.guid.g == f.engine.guid());
    }
}

TEST_CASE("advertise system interoperates", "[interop]")
{
    SECTION("pzyNet advertises to RakNet")
    {
        Fixture f;
        f.start_raknet();
        std::error_code ec;
        f.engine.advertise(f.raknet_endpoint(), Bytes{'h', 'e', 'l', 'l', 'o'}, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(f.pump_until([&] { return f.rak_count(ID_ADVERTISE_SYSTEM) == 1; }));
        CHECK(f.rak_last(ID_ADVERTISE_SYSTEM).data == Bytes{ID_ADVERTISE_SYSTEM, 'h', 'e', 'l', 'l', 'o'});
    }
    SECTION("RakNet advertises to pzyNet")
    {
        Fixture f;
        f.start_raknet();
        REQUIRE(f.rak->AdvertiseSystem("127.0.0.1", f.host.local_endpoint().port(), "hello", 5));
        REQUIRE(f.pump_until([&] { return f.count<Unconnected>() == 1; }));
        rn::wire::OutOfBand message;
        REQUIRE(rn::wire::decode(f.all<Unconnected>()[0].data, message));
        CHECK(message.guid == f.rak->GetMyGUID().g);
        CHECK(message.data == Bytes{ID_ADVERTISE_SYSTEM, 'h', 'e', 'l', 'l', 'o'});
    }
}

TEST_CASE("a pzyNet server handles several RakNet clients", "[interop]")
{
    rn::Config config;
    config.max_incoming_connections = 6;
    rn::RakNetEngine engine(config);
    UdpHost host(engine);
    host.bind(Endpoint(IpAddress::loopback_v4(), 0));
    host.start_thread(); // the server runs on its own thread, like a real one would

    const std::size_t client_count = 6;
    std::vector<RakNet::RakPeerInterface*> clients;
    for (std::size_t i = 0; i < client_count; ++i)
    {
        RakNet::RakPeerInterface* client = RakNet::RakPeerInterface::GetInstance();
        RakNet::SocketDescriptor descriptor(0, "127.0.0.1");
        REQUIRE(client->Startup(1, &descriptor, 1) == RakNet::RAKNET_STARTED);
        REQUIRE(client->Connect("127.0.0.1", host.local_endpoint().port(), nullptr, 0) ==
                RakNet::CONNECTION_ATTEMPT_STARTED);
        clients.push_back(client);
    }

    // The server echoes every message back to its sender; each client sends a numbered series.
    const std::uint32_t per_client = 200;
    std::vector<std::uint32_t> echoed(client_count, 0);
    std::vector<bool> started(client_count, false);
    std::size_t connected = 0;

    const auto give_up = std::chrono::steady_clock::now() + 60s;
    const auto finished = [&] {
        return std::all_of(echoed.begin(), echoed.end(), [&](std::uint32_t n) { return n == per_client; });
    };
    while (!finished() && std::chrono::steady_clock::now() < give_up)
    {
        Event event;
        while (host.wait_event(event, 1ms))
        {
            if (std::holds_alternative<Connected>(event))
                ++connected;
            else if (const Message* message = std::get_if<Message>(&event))
                host.send(message->connection, message->data);
        }
        for (std::size_t i = 0; i < client_count; ++i)
        {
            while (RakNet::Packet* packet = clients[i]->Receive())
            {
                if (packet->data[0] == ID_CONNECTION_REQUEST_ACCEPTED && !started[i])
                {
                    started[i] = true;
                    for (std::uint32_t n = 0; n < per_client; ++n)
                    {
                        const Bytes message = user_message(static_cast<std::uint32_t>(i) * 1000 + n, 100);
                        REQUIRE(clients[i]->Send(reinterpret_cast<const char*>(message.data()),
                                                 static_cast<int>(message.size()), MEDIUM_PRIORITY, RELIABLE_ORDERED,
                                                 0, packet->systemAddress, false) != 0);
                    }
                }
                else if (packet->data[0] >= ID_USER_PACKET_ENUM)
                {
                    const Bytes message(packet->data, packet->data + packet->length);
                    // Each client gets back exactly its own series, in order.
                    CHECK(message == user_message(static_cast<std::uint32_t>(i) * 1000 + echoed[i], 100));
                    ++echoed[i];
                }
                clients[i]->DeallocatePacket(packet);
            }
        }
    }
    CHECK(finished());
    CHECK(connected == client_count);
    CHECK(host.with_engine([](Engine& e) { return static_cast<rn::RakNetEngine&>(e).connection_count(); }) ==
          client_count);

    for (RakNet::RakPeerInterface* client : clients)
    {
        client->Shutdown(100);
        RakNet::RakPeerInterface::DestroyInstance(client);
    }
    host.stop();
}
