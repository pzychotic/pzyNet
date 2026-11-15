// Byte-exact checks of the RakNet wire codecs. The expected bytes come from the protocol description
// in the RakNet fork (docs/research/raknet-udp-protocol.md); section numbers below refer to it.

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/wire.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace pzynet;
using namespace pzynet::raknet;
using wire::Bytes;

namespace
{

const Bytes magic(wire::offline_magic.begin(), wire::offline_magic.end());

Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes all;
    for (const Bytes& part : parts)
        all.insert(all.end(), part.begin(), part.end());
    return all;
}

Bytes be64(std::uint64_t value)
{
    Bytes bytes;
    ByteWriter(bytes).u64_be(value);
    return bytes;
}

const Endpoint local_address(IpAddress::v4(127, 0, 0, 1), 60000);
const Bytes local_address_bytes{0x04, 0x80, 0xFF, 0xFF, 0xFE, 0xEA, 0x60};

} // namespace

TEST_CASE("SystemAddress: version, inverted IPv4 address, big-endian port (3.4)", "[raknet][wire]")
{
    Bytes bytes;
    ByteWriter writer(bytes);
    wire::write_address(writer, local_address);
    CHECK(bytes == local_address_bytes);

    ByteReader reader(bytes);
    Endpoint decoded;
    REQUIRE(wire::read_address(reader, decoded));
    CHECK(decoded == local_address);

    // Only the IPv4 form exists in this wire format.
    const Bytes v6{0x06, 0, 0, 0, 0, 0, 0};
    ByteReader v6_reader(v6);
    CHECK_FALSE(wire::read_address(v6_reader, decoded));

    // An IPv6 endpoint has no representation; it degrades to 0.0.0.0 with its port.
    Bytes degraded;
    ByteWriter degraded_writer(degraded);
    wire::write_address(degraded_writer, Endpoint(IpAddress::loopback_v6(), 80));
    CHECK(degraded == Bytes{0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x50});
}

TEST_CASE("range list encoding (4.4)", "[raknet][wire]")
{
    SECTION("a single value costs four bytes")
    {
        Bytes bytes;
        ByteWriter writer(bytes);
        const std::vector<wire::Range> ranges{{7, 7}};
        CHECK(wire::write_ranges(writer, ranges, 100) == 1);
        CHECK(bytes == Bytes{0x00, 0x01, 0x01, 0x07, 0x00, 0x00});
    }
    SECTION("a run costs seven bytes; 24-bit numbers are little-endian")
    {
        Bytes bytes;
        ByteWriter writer(bytes);
        const std::vector<wire::Range> ranges{{1, 3}, {9, 9}, {0x010203, 0x010305}};
        CHECK(wire::write_ranges(writer, ranges, 100) == 3);
        CHECK(bytes == Bytes{0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01, 0x09,
                             0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x05, 0x03, 0x01});

        ByteReader reader(bytes);
        std::vector<wire::Range> decoded;
        REQUIRE(wire::read_ranges(reader, decoded));
        CHECK(decoded == ranges);
        CHECK(reader.remaining() == 0);
    }
    SECTION("ranges that do not fit are left for the next datagram")
    {
        Bytes bytes;
        ByteWriter writer(bytes);
        const std::vector<wire::Range> ranges{{1, 3}, {9, 9}, {20, 30}};
        CHECK(wire::write_ranges(writer, ranges, 2 + 7 + 4) == 2);
        CHECK(bytes.size() == 13);
        CHECK(bytes[1] == 2);
    }
    SECTION("malformed lists are rejected")
    {
        std::vector<wire::Range> decoded;
        const Bytes reversed{0x00, 0x01, 0x00, 0x09, 0x00, 0x00, 0x03, 0x00, 0x00}; // 9..3
        ByteReader reversed_reader(reversed);
        CHECK_FALSE(wire::read_ranges(reversed_reader, decoded));

        const Bytes truncated{0x00, 0x02, 0x01, 0x07, 0x00, 0x00}; // announces two, carries one
        ByteReader truncated_reader(truncated);
        CHECK_FALSE(wire::read_ranges(truncated_reader, decoded));

        const Bytes huge_count{0xFF, 0xFF}; // must not allocate for 65535 entries up front
        ByteReader huge_reader(huge_count);
        CHECK_FALSE(wire::read_ranges(huge_reader, decoded));
        CHECK(decoded.capacity() < 1000);
    }
}

TEST_CASE("datagram header (4.1 - 4.3)", "[raknet][wire]")
{
    const auto encode_header = [](const wire::DatagramHeader& header) {
        Bytes bytes;
        ByteWriter writer(bytes);
        wire::write_datagram_header(writer, header);
        return bytes;
    };
    const auto decode_header = [](const Bytes& bytes, wire::DatagramHeader& header) {
        ByteReader reader(bytes);
        return wire::read_datagram_header(reader, header);
    };

    wire::DatagramHeader data;
    data.kind = wire::DatagramKind::data;
    data.needs_b_and_as = true;
    data.number = 7;
    CHECK(encode_header(data) == Bytes{0x84, 0x07, 0x00, 0x00});
    data.continuous_send = true;
    data.number = 0x123456;
    CHECK(encode_header(data) == Bytes{0x8C, 0x56, 0x34, 0x12});

    wire::DatagramHeader ack;
    ack.kind = wire::DatagramKind::ack;
    CHECK(encode_header(ack) == Bytes{0xC0});
    wire::DatagramHeader nak;
    nak.kind = wire::DatagramKind::nak;
    CHECK(encode_header(nak) == Bytes{0xA0});

    wire::DatagramHeader decoded;
    REQUIRE(decode_header({0x9C, 0x56, 0x34, 0x12}, decoded));
    CHECK(decoded.kind == wire::DatagramKind::data);
    CHECK(decoded.packet_pair);
    CHECK(decoded.continuous_send);
    CHECK(decoded.needs_b_and_as);
    CHECK(decoded.number == 0x123456);

    // Pad bits carry no meaning and must not be rejected (16.7).
    REQUIRE(decode_header({0xDF}, decoded));
    CHECK(decoded.kind == wire::DatagramKind::ack);
    CHECK_FALSE(decoded.has_b_and_as);

    // An ACK from a peer that reports its arrival rate carries a float before the ranges.
    REQUIRE(decode_header({0xE0, 0x3F, 0xC0, 0x00, 0x00}, decoded));
    CHECK(decoded.has_b_and_as);
    CHECK(decoded.arrival_rate == 1.5f);

    REQUIRE(decode_header({0xA0}, decoded));
    CHECK(decoded.kind == wire::DatagramKind::nak);

    CHECK_FALSE(decode_header({0x05, 0x00, 0x00, 0x00}, decoded)); // valid bit clear: an offline message
    CHECK_FALSE(decode_header({0x84, 0x07}, decoded));             // truncated
    CHECK_FALSE(decode_header({}, decoded));
}

TEST_CASE("message header sizes match RakNet's table (5.3)", "[raknet][wire]")
{
    using wire::Reliability;
    CHECK(wire::message_header_size(Reliability::unreliable, false) == 3);
    CHECK(wire::message_header_size(Reliability::unreliable_sequenced, false) == 10);
    CHECK(wire::message_header_size(Reliability::reliable, false) == 6);
    CHECK(wire::message_header_size(Reliability::reliable_ordered, false) == 10);
    CHECK(wire::message_header_size(Reliability::reliable_sequenced, false) == 13);
    CHECK(wire::message_header_size(Reliability::unreliable, true) == 13);
    CHECK(wire::message_header_size(Reliability::reliable_sequenced, true) == wire::maximum_message_header_size);

    for (int r = 0; r <= 4; ++r)
        for (bool split : {false, true})
        {
            wire::MessageHeader header;
            header.reliability = static_cast<Reliability>(r);
            header.split = split;
            header.bit_length = 40;
            header.reliable_number = 0x0A0B0C;
            header.sequencing_index = 0x010203;
            header.ordering_index = 0x040506;
            header.ordering_channel = 31;
            header.split_count = 9;
            header.split_id = 0xBEEF;
            header.split_index = 8;

            Bytes bytes;
            ByteWriter writer(bytes);
            wire::write_message_header(writer, header);
            CAPTURE(r, split);
            CHECK(bytes.size() == wire::message_header_size(header.reliability, split));

            ByteReader reader(bytes);
            wire::MessageHeader decoded;
            REQUIRE(wire::read_message_header(reader, decoded));
            CHECK(decoded.reliability == header.reliability);
            CHECK(decoded.split == split);
            CHECK(decoded.bit_length == 40);
            CHECK(decoded.payload_size() == 5);
            if (wire::is_reliable(header.reliability))
                CHECK(decoded.reliable_number == 0x0A0B0C);
            if (wire::is_sequenced(header.reliability))
                CHECK(decoded.sequencing_index == 0x010203);
            if (wire::has_ordering(header.reliability))
            {
                CHECK(decoded.ordering_index == 0x040506);
                CHECK(decoded.ordering_channel == 31);
            }
            if (split)
            {
                CHECK(decoded.split_count == 9);
                CHECK(decoded.split_id == 0xBEEF);
                CHECK(decoded.split_index == 8);
            }
        }
}

TEST_CASE("message header field layout", "[raknet][wire]")
{
    wire::MessageHeader header;
    header.reliability = wire::Reliability::reliable_sequenced;
    header.split = true;
    header.bit_length = 0x0120;
    header.reliable_number = 0x000102;
    header.sequencing_index = 0x000304;
    header.ordering_index = 0x000506;
    header.ordering_channel = 7;
    header.split_count = 0x01020304;
    header.split_id = 0x0506;
    header.split_index = 0x00000007;

    Bytes bytes;
    ByteWriter writer(bytes);
    wire::write_message_header(writer, header);
    CHECK(bytes == Bytes{0x90,                   // reliability 4 in the top three bits, split flag
                         0x01, 0x20,             // bit length, big-endian
                         0x02, 0x01, 0x00,       // reliable message number, little-endian
                         0x04, 0x03, 0x00,       // sequencing index, little-endian
                         0x06, 0x05, 0x00, 0x07, // ordering index (little-endian), channel
                         0x01, 0x02, 0x03, 0x04, // split count, big-endian
                         0x05, 0x06,             // split id, big-endian
                         0x00, 0x00, 0x00, 0x07});
}

TEST_CASE("message header validation mirrors RakNet (5.4)", "[raknet][wire]")
{
    const auto parses = [](const Bytes& bytes) {
        ByteReader reader(bytes);
        wire::MessageHeader header;
        return wire::read_message_header(reader, header);
    };
    CHECK(parses({0x00, 0x00, 0x08}));
    CHECK_FALSE(parses({0x00, 0x00, 0x00}));                               // empty payload
    CHECK_FALSE(parses({0xA0, 0x00, 0x08}));                               // reliability 5: never on the wire
    CHECK_FALSE(parses({0xE0, 0x00, 0x08}));                               // reliability 7
    CHECK(parses({0x60, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 31}));               // channel 31
    CHECK_FALSE(parses({0x60, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 32}));         // channel 32
    CHECK(parses({0x10, 0x00, 0x08, 0, 0, 0, 2, 0, 1, 0, 0, 0, 1}));       // split index 1 of 2
    CHECK_FALSE(parses({0x10, 0x00, 0x08, 0, 0, 0, 2, 0, 1, 0, 0, 0, 2})); // split index 2 of 2
    CHECK_FALSE(parses({0x10, 0x00, 0x08, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0})); // split count 0
    CHECK_FALSE(parses({0x40, 0x00, 0x08, 0, 0}));                         // truncated
}

TEST_CASE("the documented worked example is reproduced byte for byte (15)", "[raknet][wire]")
{
    // RELIABLE_ORDERED user message 86 01 02 03 on channel 0, datagram 7, first reliable message.
    Bytes datagram;
    ByteWriter writer(datagram);
    wire::DatagramHeader datagram_header;
    datagram_header.needs_b_and_as = true;
    datagram_header.number = 7;
    wire::write_datagram_header(writer, datagram_header);

    wire::MessageHeader message;
    message.reliability = wire::Reliability::reliable_ordered;
    message.bit_length = 32;
    wire::write_message_header(writer, message);
    writer.bytes(Bytes{0x86, 0x01, 0x02, 0x03});

    CHECK(datagram == Bytes{0x84, 0x07, 0x00, 0x00, 0x60, 0x00, 0x20, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x86, 0x01, 0x02, 0x03});

    // ...and the acknowledgement the peer answers with.
    Bytes ack;
    ByteWriter ack_writer(ack);
    wire::DatagramHeader ack_header;
    ack_header.kind = wire::DatagramKind::ack;
    wire::write_datagram_header(ack_writer, ack_header);
    const std::vector<wire::Range> ranges{{7, 7}};
    wire::write_ranges(ack_writer, ranges, 100);
    CHECK(ack == Bytes{0xC0, 0x00, 0x01, 0x01, 0x07, 0x00, 0x00});
}

TEST_CASE("unconnected ping and pong (2.3, 2.4)", "[raknet][wire]")
{
    wire::UnconnectedPing ping;
    ping.time = 0x0102030405060708ull;
    ping.guid = 0x1112131415161718ull;
    const Bytes ping_bytes = wire::encode(ping);
    CHECK(ping_bytes == concat({{0x01}, be64(ping.time), magic, be64(ping.guid)}));
    CHECK(ping_bytes.size() == 33);
    CHECK(wire::offline_message_id(ping_bytes) == id::unconnected_ping);

    wire::UnconnectedPing decoded_ping;
    REQUIRE(wire::decode(ping_bytes, decoded_ping));
    CHECK(decoded_ping.time == ping.time);
    CHECK(decoded_ping.guid == ping.guid);
    CHECK_FALSE(decoded_ping.open_connections_only);

    // The GUID is optional for the receiver.
    REQUIRE(wire::decode(ByteView(ping_bytes.data(), 25), decoded_ping));
    CHECK(decoded_ping.guid == 0);

    ping.open_connections_only = true;
    CHECK(wire::encode(ping)[0] == 0x02);

    // In the pong the magic comes after the GUID, unlike the ping.
    wire::UnconnectedPong pong;
    pong.time = ping.time;
    pong.guid = 0x2122232425262728ull;
    pong.data = {'h', 'i'};
    const Bytes pong_bytes = wire::encode(pong);
    CHECK(pong_bytes == concat({{0x1C}, be64(pong.time), be64(pong.guid), magic, {'h', 'i'}}));
    CHECK(wire::offline_message_id(pong_bytes) == id::unconnected_pong);

    wire::UnconnectedPong decoded_pong;
    REQUIRE(wire::decode(pong_bytes, decoded_pong));
    CHECK(decoded_pong.time == pong.time);
    CHECK(decoded_pong.guid == pong.guid);
    CHECK(decoded_pong.data == pong.data);
}

TEST_CASE("open connection request and reply 1 (2.5, 2.6)", "[raknet][wire]")
{
    wire::OpenConnectionRequest1 request;
    request.mtu = 1492;
    const Bytes request_bytes = wire::encode(request);
    REQUIRE(request_bytes.size() == 1492 - 28); // padded so that the IP datagram is exactly the MTU
    CHECK(Bytes(request_bytes.begin(), request_bytes.begin() + 18) == concat({{0x05}, magic, {0x06}}));
    CHECK(std::all_of(request_bytes.begin() + 18, request_bytes.end(), [](std::uint8_t b) { return b == 0; }));
    CHECK(wire::offline_message_id(request_bytes) == id::open_connection_request_1);

    wire::OpenConnectionRequest1 decoded_request;
    REQUIRE(wire::decode(request_bytes, decoded_request));
    CHECK(decoded_request.protocol == 6);
    CHECK(decoded_request.mtu == 1492); // measured from the datagram's length

    wire::OpenConnectionReply1 reply;
    reply.guid = 0x0102030405060708ull;
    reply.mtu = 1200;
    reply.padded = false;
    CHECK(wire::encode(reply) == concat({{0x06}, magic, be64(reply.guid), {0x00, 0x04, 0xB0}}));
    reply.padded = true;
    const Bytes reply_bytes = wire::encode(reply);
    CHECK(reply_bytes.size() == 1200 - 28);

    wire::OpenConnectionReply1 decoded_reply;
    REQUIRE(wire::decode(reply_bytes, decoded_reply));
    CHECK(decoded_reply.guid == reply.guid);
    CHECK(decoded_reply.mtu == 1200);
    CHECK_FALSE(decoded_reply.has_security);

    // A server demanding security sets the cookie flag; that is reported, not misparsed.
    Bytes secured = wire::encode(reply);
    secured[25] = 1;
    REQUIRE(wire::decode(secured, decoded_reply));
    CHECK(decoded_reply.has_security);
}

TEST_CASE("open connection request and reply 2 (2.7, 2.8)", "[raknet][wire]")
{
    wire::OpenConnectionRequest2 request;
    request.server_address = local_address;
    request.mtu = 1492;
    request.guid = 0x0102030405060708ull;
    const Bytes request_bytes = wire::encode(request);
    CHECK(request_bytes == concat({{0x07}, magic, local_address_bytes, {0x05, 0xD4}, be64(request.guid)}));
    CHECK(request_bytes.size() == 34);

    wire::OpenConnectionRequest2 decoded_request;
    REQUIRE(wire::decode(request_bytes, decoded_request));
    CHECK(decoded_request.server_address == local_address);
    CHECK(decoded_request.mtu == 1492);
    CHECK(decoded_request.guid == request.guid);

    wire::OpenConnectionReply2 reply;
    reply.guid = 0x1112131415161718ull;
    reply.client_address = local_address;
    reply.mtu = 576;
    const Bytes reply_bytes = wire::encode(reply);
    // The trailing security flag is a single bit, padded to a byte: 35 bytes in total (16.6).
    CHECK(reply_bytes == concat({{0x08}, magic, be64(reply.guid), local_address_bytes, {0x02, 0x40}, {0x00}}));
    CHECK(reply_bytes.size() == 35);

    wire::OpenConnectionReply2 decoded_reply;
    REQUIRE(wire::decode(reply_bytes, decoded_reply));
    CHECK(decoded_reply.guid == reply.guid);
    CHECK(decoded_reply.client_address == local_address);
    CHECK(decoded_reply.mtu == 576);
    CHECK_FALSE(decoded_reply.requires_security);

    Bytes secured = reply_bytes;
    secured.back() = 0x80;
    REQUIRE(wire::decode(secured, decoded_reply));
    CHECK(decoded_reply.requires_security);
}

TEST_CASE("handshake refusals (2.6, 2.8)", "[raknet][wire]")
{
    for (std::uint8_t reason : {id::already_connected, id::no_free_incoming_connections, id::connection_banned,
                                id::ip_recently_connected})
    {
        wire::Rejection rejection;
        rejection.id = reason;
        rejection.guid = 0x0102030405060708ull;
        const Bytes bytes = wire::encode(rejection);
        CHECK(bytes == concat({{reason}, magic, be64(rejection.guid)}));
        CHECK(bytes.size() == 25);
        CHECK(wire::offline_message_id(bytes) == reason);

        wire::Rejection decoded;
        REQUIRE(wire::decode(bytes, decoded));
        CHECK(decoded.id == reason);
        CHECK(decoded.guid == rejection.guid);
    }

    wire::IncompatibleProtocolVersion incompatible;
    incompatible.guid = 0x0102030405060708ull;
    const Bytes bytes = wire::encode(incompatible);
    CHECK(bytes == concat({{0x19, 0x06}, magic, be64(incompatible.guid)}));
    CHECK(bytes.size() == 26);
    CHECK(wire::offline_message_id(bytes) == id::incompatible_protocol_version);
}

TEST_CASE("out-of-band message (2.12)", "[raknet][wire]")
{
    wire::OutOfBand message;
    message.guid = 0x0102030405060708ull;
    message.data = {id::advertise_system, 'x'};
    const Bytes bytes = wire::encode(message);
    CHECK(bytes == concat({{0x0D}, be64(message.guid), magic, {0x1D, 'x'}}));

    wire::OutOfBand decoded;
    REQUIRE(wire::decode(bytes, decoded));
    CHECK(decoded.guid == message.guid);
    CHECK(decoded.data == message.data);
}

TEST_CASE("offline messages are recognised by id and magic position (2.1)", "[raknet][wire]")
{
    // A connected datagram is never mistaken for an offline one...
    CHECK_FALSE(wire::offline_message_id(Bytes{0x84, 0x07, 0x00, 0x00, 0x60, 0x00, 0x20}));
    // ...nor is an offline id without the magic, or with the magic at another message's offset.
    Bytes no_magic(40, 0);
    no_magic[0] = id::open_connection_request_1;
    CHECK_FALSE(wire::offline_message_id(no_magic));

    Bytes misplaced = concat({{id::unconnected_pong}, magic, Bytes(20, 0)}); // pong expects it at 17
    CHECK_FALSE(wire::offline_message_id(misplaced));

    CHECK_FALSE(wire::offline_message_id(concat({{id::open_connection_request_1}, Bytes(magic.begin(), magic.end() - 1)})));
    CHECK_FALSE(wire::offline_message_id(Bytes{}));
    // Ids that only exist inside a connection are not offline messages even with a magic after them.
    CHECK_FALSE(wire::offline_message_id(concat({{id::connection_request}, magic})));
}

TEST_CASE("connected handshake messages (2.9 - 2.11)", "[raknet][wire]")
{
    wire::ConnectionRequest request;
    request.guid = 0x0102030405060708ull;
    request.time = 0x1112131415161718ull;
    request.password = {'p', 'w'};
    const Bytes request_bytes = wire::encode(request);
    CHECK(request_bytes == concat({{0x09}, be64(request.guid), be64(request.time), {0x00}, {'p', 'w'}}));

    wire::ConnectionRequest decoded_request;
    REQUIRE(wire::decode(request_bytes, decoded_request));
    CHECK(decoded_request.guid == request.guid);
    CHECK(decoded_request.time == request.time);
    CHECK(decoded_request.password == request.password);

    Bytes secured = request_bytes;
    secured[17] = 1; // doSecurity: not supported
    CHECK_FALSE(wire::decode(secured, decoded_request));

    wire::ConnectionRequestAccepted accepted;
    accepted.client_address = local_address;
    accepted.system_index = 0x0102;
    accepted.internal_addresses[0] = local_address;
    accepted.request_time = request.time;
    accepted.accept_time = 0x2122232425262728ull;
    const Bytes accepted_bytes = wire::encode(accepted);
    REQUIRE(accepted_bytes.size() == 96);
    CHECK(Bytes(accepted_bytes.begin(), accepted_bytes.begin() + 17) ==
          concat({{0x10}, local_address_bytes, {0x01, 0x02}, local_address_bytes}));
    CHECK(Bytes(accepted_bytes.end() - 16, accepted_bytes.end()) ==
          concat({be64(accepted.request_time), be64(accepted.accept_time)}));

    wire::ConnectionRequestAccepted decoded_accepted;
    REQUIRE(wire::decode(accepted_bytes, decoded_accepted));
    CHECK(decoded_accepted.client_address == local_address);
    CHECK(decoded_accepted.system_index == 0x0102);
    CHECK(decoded_accepted.internal_addresses[0] == local_address);
    CHECK(decoded_accepted.internal_addresses[9] == Endpoint());
    CHECK(decoded_accepted.request_time == accepted.request_time);
    CHECK(decoded_accepted.accept_time == accepted.accept_time);
    CHECK_FALSE(wire::decode(ByteView(accepted_bytes.data(), 95), decoded_accepted));

    wire::NewIncomingConnection incoming;
    incoming.server_address = local_address;
    incoming.accept_time = accepted.accept_time;
    incoming.time = 0x3132333435363738ull;
    const Bytes incoming_bytes = wire::encode(incoming);
    REQUIRE(incoming_bytes.size() == 94);
    CHECK(incoming_bytes[0] == 0x13);

    wire::NewIncomingConnection decoded_incoming;
    REQUIRE(wire::decode(incoming_bytes, decoded_incoming));
    CHECK(decoded_incoming.server_address == local_address);
    CHECK(decoded_incoming.accept_time == incoming.accept_time);
    CHECK(decoded_incoming.time == incoming.time);
}

TEST_CASE("connected ping and pong (12.1)", "[raknet][wire]")
{
    wire::ConnectedPing ping;
    ping.time = 0x0102030405060708ull;
    CHECK(wire::encode(ping) == concat({{0x00}, be64(ping.time)}));

    wire::ConnectedPong pong;
    pong.ping_time = ping.time;
    pong.pong_time = 0x1112131415161718ull;
    const Bytes pong_bytes = wire::encode(pong);
    CHECK(pong_bytes == concat({{0x03}, be64(pong.ping_time), be64(pong.pong_time)}));
    CHECK(pong_bytes.size() == 17);

    wire::ConnectedPong decoded;
    REQUIRE(wire::decode(pong_bytes, decoded));
    CHECK(decoded.ping_time == pong.ping_time);
    CHECK(decoded.pong_time == pong.pong_time);
    CHECK_FALSE(wire::decode(ByteView(pong_bytes.data(), 16), decoded));

    CHECK(wire::encode_invalid_password(0x0102030405060708ull) == concat({{0x18}, be64(0x0102030405060708ull)}));
}
