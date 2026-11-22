// Live checks of the Peer and the BitStream against a real RakPeer, built from the checkout named by
// PZYNET_RAKNET_INTEROP_DIR. Both sides run their own network thread over loopback UDP.

#include "pzynet/raknet/bit_stream.hpp"
#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/rakpeer/peer.hpp"

#include "BitStream.h"
#include "GetTime.h"
#include "MessageIdentifiers.h"
#include "PacketPriority.h"
#include "RakNetTypes.h"
#include "RakPeerInterface.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace std::chrono_literals;
using namespace RakNet; // message ids, priorities and reliabilities of the reference implementation
namespace rn = pzynet::raknet;
using Bytes = std::vector<std::uint8_t>;

namespace
{

enum class Direction
{
    peer_connects,
    rakpeer_connects
};

const char* name(Direction direction)
{
    return direction == Direction::peer_connects ? "Peer connects to RakPeer" : "RakPeer connects to Peer";
}

struct RakPacket
{
    unsigned char id = 0;
    Bytes data;
    RakNet::SystemAddress address;
    RakNet::RakNetGUID guid;
};

/// A pzyNet Peer and a RakPeer on loopback, each collecting what it receives.
struct Fixture
{
    rn::Peer peer;
    RakNet::RakPeerInterface* rak = RakNet::RakPeerInterface::GetInstance();

    std::vector<rn::Packet> peer_packets;
    std::vector<RakPacket> rak_packets;

    RakNet::SystemAddress peer_address; // the Peer as the RakPeer sees it

    explicit Fixture(ClockSource peer_clock = ClockSource())
    {
        rn::PeerConfig config;
        config.bind = Endpoint(IpAddress::loopback_v4(), 0);
        config.engine.max_incoming_connections = 4;
        config.clock = std::move(peer_clock);
        peer.start(config);

        RakNet::SocketDescriptor descriptor(0, "127.0.0.1");
        REQUIRE(rak->Startup(8, &descriptor, 1) == RakNet::RAKNET_STARTED);
        rak->SetMaximumIncomingConnections(4);
    }
    ~Fixture()
    {
        rak->Shutdown(0);
        RakNet::RakPeerInterface::DestroyInstance(rak);
    }

    Endpoint rak_endpoint() const { return Endpoint(IpAddress::loopback_v4(), rak->GetMyBoundAddress(0).GetPort()); }
    rn::Guid rak_guid() const { return rn::Guid{rak->GetMyGUID().g}; }

    void collect()
    {
        while (std::optional<rn::Packet> packet = peer.receive())
            peer_packets.push_back(std::move(*packet));
        while (RakNet::Packet* packet = rak->Receive())
        {
            RakPacket copy;
            copy.id = packet->data[0];
            copy.data.assign(packet->data, packet->data + packet->length);
            copy.address = packet->systemAddress;
            copy.guid = packet->guid;
            rak_packets.push_back(std::move(copy));
            rak->DeallocatePacket(packet);
        }
    }

    template <class Predicate>
    bool wait_until(Predicate done, std::chrono::milliseconds limit = 10s)
    {
        const auto give_up = std::chrono::steady_clock::now() + limit;
        for (;;)
        {
            collect();
            if (done())
                return true;
            if (std::chrono::steady_clock::now() >= give_up)
                return false;
            std::this_thread::sleep_for(1ms);
        }
    }

    std::size_t peer_count(std::uint8_t id) const
    {
        return static_cast<std::size_t>(std::count_if(peer_packets.begin(), peer_packets.end(),
                                                      [id](const rn::Packet& p) { return p.id == id; }));
    }
    const rn::Packet& peer_last(std::uint8_t id) const
    {
        const auto it = std::find_if(peer_packets.rbegin(), peer_packets.rend(),
                                     [id](const rn::Packet& p) { return p.id == id; });
        REQUIRE(it != peer_packets.rend());
        return *it;
    }
    std::size_t rak_count(unsigned char id) const
    {
        return static_cast<std::size_t>(std::count_if(rak_packets.begin(), rak_packets.end(),
                                                      [id](const RakPacket& p) { return p.id == id; }));
    }
    const RakPacket& rak_last(unsigned char id) const
    {
        const auto it = std::find_if(rak_packets.rbegin(), rak_packets.rend(),
                                     [id](const RakPacket& p) { return p.id == id; });
        REQUIRE(it != rak_packets.rend());
        return *it;
    }

    void establish(Direction direction)
    {
        if (direction == Direction::peer_connects)
        {
            peer.connect(rak_endpoint());
            REQUIRE(wait_until([&] {
                return peer_count(rn::id::connection_request_accepted) == 1 &&
                       rak_count(ID_NEW_INCOMING_CONNECTION) == 1;
            }));
            peer_address = rak_last(ID_NEW_INCOMING_CONNECTION).address;
        }
        else
        {
            REQUIRE(rak->Connect("127.0.0.1", peer.local_endpoint().port(), nullptr, 0) ==
                    RakNet::CONNECTION_ATTEMPT_STARTED);
            REQUIRE(wait_until([&] {
                return peer_count(rn::id::new_incoming_connection) == 1 &&
                       rak_count(ID_CONNECTION_REQUEST_ACCEPTED) == 1;
            }));
            peer_address = rak_last(ID_CONNECTION_REQUEST_ACCEPTED).address;
        }
    }
};

} // namespace

TEST_CASE("Peer: connection outcomes carry the RakPeer's GUID and address", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const rn::Packet& packet = f.peer_last(direction == Direction::peer_connects ? rn::id::connection_request_accepted
                                                                                 : rn::id::new_incoming_connection);
    CHECK(packet.guid == f.rak_guid());
    CHECK(packet.address == f.rak_endpoint());
    CHECK(packet.data == Bytes{packet.id});

    const RakPacket& rak_packet = f.rak_last(direction == Direction::peer_connects ? ID_NEW_INCOMING_CONNECTION
                                                                                   : ID_CONNECTION_REQUEST_ACCEPTED);
    CHECK(rak_packet.guid.g == f.peer.guid().value);
    CHECK(f.peer_address.GetPort() == f.peer.local_endpoint().port());
}

TEST_CASE("BitStream: a mixed message is understood by RakNet's BitStream in both directions",
          "[interop][rakpeer][bitstream]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    // pzyNet -> RakNet
    rn::BitStreamWriter out;
    out.write(static_cast<std::uint8_t>(ID_USER_PACKET_ENUM));
    out.write(true);
    out.write(std::uint16_t{0xBEEF});
    out.write(false);
    out.write(true);
    out.write(std::int32_t{-123456789});
    out.write(std::uint64_t{0x0123456789ABCDEFull});
    out.write(std::int8_t{-7});
    out.write(true);
    f.peer.send(f.rak_guid(), out);
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_USER_PACKET_ENUM) == 1; }));

    Bytes received = f.rak_last(ID_USER_PACKET_ENUM).data;
    RakNet::BitStream in(received.data(), static_cast<unsigned int>(received.size()), false);
    in.IgnoreBytes(1);
    bool b1 = false, b2 = true, b3 = false, b4 = false;
    std::uint16_t u16 = 0;
    std::int32_t i32 = 0;
    std::uint64_t u64 = 0;
    std::int8_t i8 = 0;
    CHECK(in.Read(b1));
    CHECK(in.Read(u16));
    CHECK(in.Read(b2));
    CHECK(in.Read(b3));
    CHECK(in.Read(i32));
    CHECK(in.Read(u64));
    CHECK(in.Read(i8));
    CHECK(in.Read(b4));
    CHECK(b1);
    CHECK(u16 == 0xBEEF);
    CHECK_FALSE(b2);
    CHECK(b3);
    CHECK(i32 == -123456789);
    CHECK(u64 == 0x0123456789ABCDEFull);
    CHECK(i8 == -7);
    CHECK(b4);

    // RakNet -> pzyNet
    RakNet::BitStream reply;
    reply.Write(static_cast<unsigned char>(ID_USER_PACKET_ENUM + 1));
    reply.Write(false);
    reply.Write(std::uint32_t{0xCAFEF00D});
    reply.Write(true);
    reply.Write(std::int16_t{-2});
    reply.Write(true);
    reply.Write(std::uint8_t{0x5A});
    REQUIRE(f.rak->Send(&reply, HIGH_PRIORITY, RELIABLE_ORDERED, 0, f.peer_address, false) != 0);
    REQUIRE(f.wait_until([&] { return f.peer_count(ID_USER_PACKET_ENUM + 1) == 1; }));

    const rn::Packet& packet = f.peer_last(ID_USER_PACKET_ENUM + 1);
    CHECK(packet.guid == f.rak_guid());
    rn::BitStreamReader reader = packet.reader();
    CHECK(reader.read<bool>() == false);
    CHECK(reader.read<std::uint32_t>() == 0xCAFEF00D);
    CHECK(reader.read<bool>() == true);
    CHECK(reader.read<std::int16_t>() == -2);
    CHECK(reader.read<bool>() == true);
    CHECK(reader.read<std::uint8_t>() == 0x5A);
    CHECK(reader.ok());
}

TEST_CASE("BitStream: the writer's bytes equal RakNet's for the same booleans and integers",
          "[interop][bitstream]")
{
    rn::BitStreamWriter ours;
    RakNet::BitStream theirs;
    for (std::uint32_t i = 0; i < 200; ++i)
    {
        const std::uint32_t value = i * 2654435761u;
        switch (value % 5)
        {
        case 0: ours.write((value & 8) != 0); theirs.Write((value & 8) != 0); break;
        case 1: ours.write(static_cast<std::uint8_t>(value >> 8)); theirs.Write(static_cast<std::uint8_t>(value >> 8)); break;
        case 2: ours.write(static_cast<std::int16_t>(value >> 4)); theirs.Write(static_cast<std::int16_t>(value >> 4)); break;
        case 3: ours.write(value); theirs.Write(value); break;
        case 4: ours.write(std::uint64_t{value} << 20); theirs.Write(std::uint64_t{value} << 20); break;
        }
    }
    CHECK(ours.bit_count() == theirs.GetNumberOfBitsUsed());
    CHECK(Bytes(ours.data().begin(), ours.data().end()) ==
          Bytes(theirs.GetData(), theirs.GetData() + theirs.GetNumberOfBytesUsed()));
}

TEST_CASE("Peer: the external address agrees with the RakPeer's view of the connection", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const auto endpoint_of = [](const RakNet::SystemAddress& address) {
        return Endpoint::parse(address.ToString(true, ':'));
    };
    // The Peer's external address is where the RakPeer sees it...
    const std::optional<Endpoint> external = f.peer.external_address(f.rak_guid());
    REQUIRE(external);
    CHECK(external == endpoint_of(f.peer_address));
    // ...and the RakPeer's external address, which the Peer reported, is where the Peer sees it.
    CHECK(endpoint_of(f.rak->GetExternalID(f.peer_address)) == f.rak_endpoint());
}

TEST_CASE("Peer: a shortened time-out notices a vanished RakPeer early", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);
    f.peer.set_timeout(f.rak_guid(), 1s);

    const auto vanished = std::chrono::steady_clock::now();
    f.rak->Shutdown(0); // no notification
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::connection_lost) == 1; }, 5s));
    CHECK(std::chrono::steady_clock::now() - vanished < 4s);
    CHECK(f.peer_last(rn::id::connection_lost).guid == f.rak_guid());
}

TEST_CASE("Peer: timestamped messages exchanged with a RakPeer arrive in local time on both sides",
          "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    // The Peer's clock runs an hour ahead of the RakPeer's, so an unconverted time would be an hour off.
    Fixture f([] { return Clock::now() + 1h; });
    f.establish(direction);
    const auto difference = [](std::uint64_t a, std::uint64_t b) { return static_cast<std::int64_t>(a - b); };
    REQUIRE(difference(f.peer.time(), RakNet::GetTime()) > 3'500'000);

    // pzyNet -> RakNet: RakPeer's Receive shifts the time into its own clock.
    rn::BitStreamWriter out;
    out.write(rn::id::timestamp);
    out.write(f.peer.time());
    out.write(static_cast<std::uint8_t>(ID_USER_PACKET_ENUM));
    out.write(std::uint16_t{0xBEEF});
    f.peer.send(f.rak_guid(), out);
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_TIMESTAMP) == 1; }));
    const RakNet::Time rak_received_at = RakNet::GetTime();
    Bytes received = f.rak_last(ID_TIMESTAMP).data;
    RakNet::BitStream in(received.data(), static_cast<unsigned int>(received.size()), false);
    in.IgnoreBytes(1);
    RakNet::Time rak_time = 0;
    unsigned char real_id = 0;
    std::uint16_t value = 0;
    CHECK(in.Read(rak_time));
    CHECK(in.Read(real_id));
    CHECK(in.Read(value));
    CHECK(difference(rak_received_at, rak_time) >= -20);
    CHECK(difference(rak_received_at, rak_time) <= 100);
    CHECK(real_id == ID_USER_PACKET_ENUM);
    CHECK(value == 0xBEEF);

    // RakNet -> pzyNet
    RakNet::BitStream reply;
    reply.Write(static_cast<unsigned char>(ID_TIMESTAMP));
    reply.Write(RakNet::GetTime());
    reply.Write(static_cast<unsigned char>(ID_USER_PACKET_ENUM + 2));
    reply.Write(std::uint32_t{0xCAFEF00D});
    REQUIRE(f.rak->Send(&reply, HIGH_PRIORITY, RELIABLE_ORDERED, 0, f.peer_address, false) != 0);
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::timestamp) == 1; }));
    const std::uint64_t peer_received_at = f.peer.time();
    const rn::Packet& packet = f.peer_last(rn::id::timestamp);
    REQUIRE(packet.timestamp);
    CHECK(difference(peer_received_at, *packet.timestamp) >= -20);
    CHECK(difference(peer_received_at, *packet.timestamp) <= 100);
    rn::BitStreamReader reader = packet.reader();
    CHECK(reader.read<std::uint8_t>() == ID_USER_PACKET_ENUM + 2);
    CHECK(reader.read<std::uint32_t>() == 0xCAFEF00D);
    CHECK(reader.ok());
}

TEST_CASE("Peer: a RakPeer's close arrives as a disconnection notification", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    f.rak->CloseConnection(f.peer_address, true);
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::disconnection_notification) == 1; }));
    CHECK(f.peer_last(rn::id::disconnection_notification).guid == f.rak_guid());
    CHECK(f.peer_last(rn::id::disconnection_notification).address == f.rak_endpoint());
}

TEST_CASE("Peer: closing locally notifies the RakPeer and produces no Packet", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);
    const std::size_t before = f.peer_packets.size();

    f.peer.disconnect(f.rak_endpoint());
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_DISCONNECTION_NOTIFICATION) == 1; }));
    f.wait_until([] { return false; }, 300ms);
    CHECK(f.peer_packets.size() == before);
}

TEST_CASE("Peer: after shutdown the RakPeer gets no notification and the port is free", "[interop][rakpeer]")
{
    Fixture f;
    f.establish(Direction::peer_connects);
    const std::uint16_t port = f.peer.local_endpoint().port();

    f.peer.shutdown();
    f.wait_until([] { return false; }, 300ms);
    CHECK(f.rak_count(ID_DISCONNECTION_NOTIFICATION) == 0);
    CHECK(f.rak->GetConnectionState(f.peer_address) == RakNet::IS_CONNECTED);

    rn::PeerConfig config;
    config.bind = Endpoint(IpAddress::loopback_v4(), port);
    rn::Peer again;
    std::error_code ec;
    again.start(config, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("Peer: the connection list names the RakPeer by GUID and address", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    const std::vector<rn::RemoteSystem> listed = f.peer.connections();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].guid == f.rak_guid());
    CHECK(listed[0].address == f.rak_endpoint());
    CHECK(listed[0].incoming == (direction == Direction::rakpeer_connects));
    CHECK(listed[0].mtu == f.rak->GetMTUSize(f.peer_address));
    CHECK(f.peer.guid_of(f.rak_endpoint()) == f.rak_guid());
    CHECK(f.peer.address_of(f.rak_guid()) == f.rak_endpoint());
    CHECK(f.rak->GetGuidFromSystemAddress(f.peer_address).g == f.peer.guid().value);

    f.rak->CloseConnection(f.peer_address, true);
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::disconnection_notification) == 1; }));
    CHECK(f.peer.connections().empty());
    CHECK_FALSE(f.peer.guid_of(f.rak_endpoint()));
}

TEST_CASE("Peer: a RakPeer's offline ping gets the configured response", "[interop][rakpeer]")
{
    Fixture f;
    const Bytes response{'p', 'z', 'y'};
    f.peer.set_offline_ping_response(response);

    REQUIRE(f.rak->Ping("127.0.0.1", f.peer.local_endpoint().port(), false));
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_UNCONNECTED_PONG) == 1; }));
    const RakPacket& pong = f.rak_last(ID_UNCONNECTED_PONG);
    CHECK(pong.guid.g == f.peer.guid().value);
    CHECK(pong.address.GetPort() == f.peer.local_endpoint().port());
    REQUIRE(pong.data.size() == 1 + 4 + response.size()); // [id][RakNet::TimeMS][response]
    CHECK(Bytes(pong.data.begin() + 5, pong.data.end()) == response);

    // Only systems that accept connections answer the "open connections" ping.
    f.peer.set_max_incoming_connections(0);
    REQUIRE(f.rak->Ping("127.0.0.1", f.peer.local_endpoint().port(), true));
    f.wait_until([] { return false; }, 300ms);
    CHECK(f.rak_count(ID_UNCONNECTED_PONG) == 1);
    f.peer.set_max_incoming_connections(1);
    REQUIRE(f.rak->Ping("127.0.0.1", f.peer.local_endpoint().port(), true));
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_UNCONNECTED_PONG) == 2; }));
}

TEST_CASE("Peer: pinging a RakPeer yields its pong as a Packet", "[interop][rakpeer]")
{
    Fixture f;
    const char response[] = "raknet";
    f.rak->SetOfflinePingResponse(response, 6);

    f.peer.ping(f.rak_endpoint());
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::unconnected_pong) == 1; }));
    const rn::Packet& pong = f.peer_last(rn::id::unconnected_pong);
    CHECK(pong.guid == f.rak_guid());
    CHECK(pong.address == f.rak_endpoint());
    REQUIRE(pong.data.size() == 1 + 4 + 6);
    CHECK(Bytes(pong.data.begin() + 5, pong.data.end()) == Bytes(response, response + 6));

    f.rak->SetMaximumIncomingConnections(0);
    f.peer.ping(f.rak_endpoint(), true);
    f.wait_until([] { return false; }, 300ms);
    CHECK(f.peer_count(rn::id::unconnected_pong) == 1);
}

TEST_CASE("Peer: advertisements reach a RakPeer and arrive from one", "[interop][rakpeer]")
{
    Fixture f;

    const Bytes ours{1, 2, 3, 4};
    f.peer.advertise(f.rak_endpoint(), ours);
    REQUIRE(f.wait_until([&] { return f.rak_count(ID_ADVERTISE_SYSTEM) == 1; }));
    const RakPacket& received = f.rak_last(ID_ADVERTISE_SYSTEM);
    CHECK(received.data == Bytes{ID_ADVERTISE_SYSTEM, 1, 2, 3, 4});
    CHECK(received.guid.g == f.peer.guid().value);
    CHECK(received.address.GetPort() == f.peer.local_endpoint().port());

    const char theirs[] = {9, 8, 7};
    REQUIRE(f.rak->AdvertiseSystem("127.0.0.1", f.peer.local_endpoint().port(), theirs, 3));
    REQUIRE(f.wait_until([&] { return f.peer_count(rn::id::advertise_system) == 1; }));
    const rn::Packet& packet = f.peer_last(rn::id::advertise_system);
    CHECK(packet.data == Bytes{rn::id::advertise_system, 9, 8, 7});
    CHECK(packet.guid == f.rak_guid());
    CHECK(packet.address == f.rak_endpoint());
    CHECK(f.peer.connections().empty());
}

namespace
{

/// One send of the receipt scenarios, in both libraries' terms.
struct ReceiptSend
{
    transport::Reliability reliability;
    bool ack_receipt;
    PacketReliability rak_reliability;
};

const ReceiptSend receipt_sends[] = {
    {transport::Reliability::reliable_ordered, true, RELIABLE_ORDERED_WITH_ACK_RECEIPT},
    {transport::Reliability::unreliable, false, UNRELIABLE},
    {transport::Reliability::unreliable, true, UNRELIABLE_WITH_ACK_RECEIPT},
    {transport::Reliability::reliable_sequenced, false, RELIABLE_SEQUENCED},
    {transport::Reliability::reliable, true, RELIABLE_WITH_ACK_RECEIPT},
};

std::uint32_t send_from_peer(Fixture& f, const ReceiptSend& send)
{
    rn::SendOptions options;
    options.reliability = send.reliability;
    options.ack_receipt = send.ack_receipt;
    return f.peer.send(f.rak_guid(), Bytes{rn::id::user_packet_enum, 7}, options);
}

std::uint32_t send_from_rakpeer(Fixture& f, const ReceiptSend& send)
{
    const char data[] = {static_cast<char>(ID_USER_PACKET_ENUM), 7};
    return f.rak->Send(data, 2, HIGH_PRIORITY, send.rak_reliability, 0, f.peer_address, false);
}

/// The receipt Packets' bytes, sorted (acknowledgements need not arrive in send order).
std::vector<Bytes> peer_receipts(const Fixture& f)
{
    std::vector<Bytes> result;
    for (const rn::Packet& packet : f.peer_packets)
        if (packet.id == rn::id::snd_receipt_acked || packet.id == rn::id::snd_receipt_loss)
            result.push_back(packet.data);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<Bytes> rak_receipts(const Fixture& f)
{
    std::vector<Bytes> result;
    for (const RakPacket& packet : f.rak_packets)
        if (packet.id == ID_SND_RECEIPT_ACKED || packet.id == ID_SND_RECEIPT_LOSS)
            result.push_back(packet.data);
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

TEST_CASE("Peer: receipt numbers and acknowledgement Packets match the RakPeer's", "[interop][rakpeer]")
{
    const Direction direction = GENERATE(Direction::peer_connects, Direction::rakpeer_connects);
    INFO(name(direction));
    Fixture f;
    f.establish(direction);

    std::vector<std::uint32_t> peer_numbers;
    std::vector<std::uint32_t> rak_numbers;
    for (const ReceiptSend& send : receipt_sends)
    {
        peer_numbers.push_back(send_from_peer(f, send));
        rak_numbers.push_back(send_from_rakpeer(f, send));
    }
    CHECK(peer_numbers == rak_numbers);

    REQUIRE(f.wait_until([&] { return peer_receipts(f).size() == 3 && rak_receipts(f).size() == 3; }));
    f.wait_until([] { return false; }, 300ms); // nothing more may arrive
    CHECK(peer_receipts(f) == rak_receipts(f));
    CHECK(f.peer_count(rn::id::snd_receipt_acked) == 3);
    CHECK(f.peer_last(rn::id::snd_receipt_acked).guid == f.rak_guid());
    CHECK(f.peer_last(rn::id::snd_receipt_acked).address == f.rak_endpoint());
}

TEST_CASE("Peer: an unacknowledged unreliable send reports the same loss Packet as the RakPeer's",
          "[interop][rakpeer]")
{
    const ReceiptSend& unreliable = receipt_sends[2];

    Fixture rak_silent;
    rak_silent.establish(Direction::peer_connects);
    rak_silent.rak->Shutdown(0);
    send_from_peer(rak_silent, unreliable);
    REQUIRE(rak_silent.wait_until([&] { return !peer_receipts(rak_silent).empty(); }));

    Fixture peer_silent;
    peer_silent.establish(Direction::peer_connects);
    peer_silent.peer.shutdown();
    send_from_rakpeer(peer_silent, unreliable);
    REQUIRE(peer_silent.wait_until([&] { return !rak_receipts(peer_silent).empty(); }));

    CHECK(peer_receipts(rak_silent).front().front() == rn::id::snd_receipt_loss);
    CHECK(peer_receipts(rak_silent) == rak_receipts(peer_silent));
}
