#include "pzynet/raknet/wire.hpp"

#include "pzynet/core/sequence.hpp"
#include "pzynet/raknet/message_ids.hpp"

#include <algorithm>
#include <cstring>

namespace pzynet
{
namespace raknet
{
namespace wire
{
namespace
{

void write_magic(ByteWriter& out)
{
    out.bytes(ByteView(offline_magic.data(), offline_magic.size()));
}

/// Starts reading an offline message: checks the id and skips it.
bool expect_id(ByteReader& in, std::uint8_t expected)
{
    return in.u8() == expected && in.ok();
}

void skip_magic(ByteReader& in)
{
    in.skip(offline_magic.size());
}

Bytes to_bytes(ByteView view)
{
    return Bytes(view.begin(), view.end());
}

} // namespace

// --- SystemAddress -------------------------------------------------------------------------------

void write_address(ByteWriter& out, const Endpoint& endpoint)
{
    const std::array<std::uint8_t, 4> bytes =
        endpoint.address().is_v4() ? endpoint.address().v4_bytes() : std::array<std::uint8_t, 4>{};
    out.u8(4);
    for (std::uint8_t b : bytes)
        out.u8(static_cast<std::uint8_t>(~b));
    out.u16_be(endpoint.port());
}

bool read_address(ByteReader& in, Endpoint& endpoint)
{
    if (in.u8() != 4) // 6 would be a raw sockaddr_in6: not part of the IPv4 wire format
        return false;
    std::array<std::uint8_t, 4> bytes{};
    for (std::uint8_t& b : bytes)
        b = static_cast<std::uint8_t>(~in.u8());
    const std::uint16_t port = in.u16_be();
    if (!in.ok())
        return false;
    endpoint = Endpoint(IpAddress::v4(bytes), port);
    return true;
}

// --- ACK / NAK range list ------------------------------------------------------------------------

std::size_t write_ranges(ByteWriter& out, Span<const Range> ranges, std::size_t max_bytes)
{
    std::size_t count = 0;
    std::size_t bytes = 2;
    while (count < ranges.size() && count < 0xFFFF)
    {
        const std::size_t entry = ranges[count].first == ranges[count].last ? 4 : 7;
        if (bytes + entry > max_bytes)
            break;
        bytes += entry;
        ++count;
    }

    out.u16_be(static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const bool single = ranges[i].first == ranges[i].last;
        out.u8(single ? 1 : 0);
        out.u24_le(ranges[i].first);
        if (!single)
            out.u24_le(ranges[i].last);
    }
    return count;
}

bool read_ranges(ByteReader& in, std::vector<Range>& ranges)
{
    ranges.clear();
    const std::size_t count = in.u16_be();
    if (!in.ok())
        return false;
    ranges.reserve(std::min(count, in.remaining() / 4)); // never trust the count for allocation
    for (std::size_t i = 0; i < count; ++i)
    {
        const bool single = in.u8() != 0;
        Range range;
        range.first = in.u24_le();
        range.last = single ? range.first : in.u24_le();
        if (!in.ok() || seq24::distance(range.first, range.last) >= seq24::half) // last before first
            return false;
        ranges.push_back(range);
    }
    return true;
}

// --- Datagram header -----------------------------------------------------------------------------

void write_datagram_header(ByteWriter& out, const DatagramHeader& header)
{
    switch (header.kind)
    {
    case DatagramKind::ack:
        out.u8(static_cast<std::uint8_t>(0x80 | 0x40 | (header.has_b_and_as ? 0x20 : 0)));
        if (header.has_b_and_as)
            out.f32_be(header.arrival_rate);
        break;
    case DatagramKind::nak:
        out.u8(0x80 | 0x20);
        break;
    case DatagramKind::data:
        out.u8(static_cast<std::uint8_t>(0x80 | (header.packet_pair ? 0x10 : 0) |
                                         (header.continuous_send ? 0x08 : 0) |
                                         (header.needs_b_and_as ? 0x04 : 0)));
        out.u24_le(header.number);
        break;
    }
}

bool read_datagram_header(ByteReader& in, DatagramHeader& header)
{
    const std::uint8_t flags = in.u8();
    if (!in.ok() || (flags & 0x80) == 0)
        return false;

    header = DatagramHeader();
    if (flags & 0x40)
    {
        header.kind = DatagramKind::ack;
        header.has_b_and_as = (flags & 0x20) != 0;
        if (header.has_b_and_as)
            header.arrival_rate = in.f32_be();
    }
    else if (flags & 0x20)
        header.kind = DatagramKind::nak;
    else
    {
        header.kind = DatagramKind::data;
        header.packet_pair = (flags & 0x10) != 0;
        header.continuous_send = (flags & 0x08) != 0;
        header.needs_b_and_as = (flags & 0x04) != 0;
        header.number = in.u24_le();
    }
    return in.ok();
}

// --- Message header ------------------------------------------------------------------------------

void write_message_header(ByteWriter& out, const MessageHeader& header)
{
    out.u8(static_cast<std::uint8_t>((static_cast<unsigned>(header.reliability) << 5) | (header.split ? 0x10 : 0)));
    out.u16_be(header.bit_length);
    if (is_reliable(header.reliability))
        out.u24_le(header.reliable_number);
    if (is_sequenced(header.reliability))
        out.u24_le(header.sequencing_index);
    if (has_ordering(header.reliability))
    {
        out.u24_le(header.ordering_index);
        out.u8(header.ordering_channel);
    }
    if (header.split)
    {
        out.u32_be(header.split_count);
        out.u16_be(header.split_id);
        out.u32_be(header.split_index);
    }
}

bool read_message_header(ByteReader& in, MessageHeader& header)
{
    const std::uint8_t flags = in.u8();
    const unsigned reliability = flags >> 5;
    if (!in.ok() || reliability > static_cast<unsigned>(Reliability::reliable_sequenced))
        return false;

    header = MessageHeader();
    header.reliability = static_cast<Reliability>(reliability);
    header.split = (flags & 0x10) != 0;
    header.bit_length = in.u16_be();
    if (is_reliable(header.reliability))
        header.reliable_number = in.u24_le();
    if (is_sequenced(header.reliability))
        header.sequencing_index = in.u24_le();
    if (has_ordering(header.reliability))
    {
        header.ordering_index = in.u24_le();
        header.ordering_channel = in.u8();
    }
    if (header.split)
    {
        header.split_count = in.u32_be();
        header.split_id = in.u16_be();
        header.split_index = in.u32_be();
    }

    if (!in.ok() || header.bit_length == 0)
        return false;
    if (header.ordering_channel >= ordering_channel_count)
        return false;
    if (header.split && header.split_index >= header.split_count)
        return false;
    return true;
}

// --- Offline messages ----------------------------------------------------------------------------

std::optional<std::uint8_t> offline_message_id(ByteView datagram) noexcept
{
    if (datagram.empty())
        return std::nullopt;

    // The magic sits at a different offset depending on the message.
    std::size_t offset;
    switch (datagram[0])
    {
    case id::unconnected_ping:
    case id::unconnected_ping_open_connections:
    case id::out_of_band_internal:
        offset = 9;
        break;
    case id::unconnected_pong:
        offset = 17;
        break;
    case id::incompatible_protocol_version:
        offset = 2;
        break;
    case id::open_connection_request_1:
    case id::open_connection_reply_1:
    case id::open_connection_request_2:
    case id::open_connection_reply_2:
    case id::connection_attempt_failed:
    case id::already_connected:
    case id::no_free_incoming_connections:
    case id::connection_banned:
    case id::ip_recently_connected:
        offset = 1;
        break;
    default:
        return std::nullopt;
    }
    if (datagram.size() < offset + offline_magic.size() ||
        std::memcmp(datagram.data() + offset, offline_magic.data(), offline_magic.size()) != 0)
        return std::nullopt;
    return datagram[0];
}

Bytes encode(const UnconnectedPing& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(message.open_connections_only ? id::unconnected_ping_open_connections : id::unconnected_ping);
    out.u64_be(message.time);
    write_magic(out);
    out.u64_be(message.guid);
    return bytes;
}

bool decode(ByteView datagram, UnconnectedPing& message)
{
    ByteReader in(datagram);
    const std::uint8_t kind = in.u8();
    if (kind != id::unconnected_ping && kind != id::unconnected_ping_open_connections)
        return false;
    message.open_connections_only = kind == id::unconnected_ping_open_connections;
    message.time = in.u64_be();
    skip_magic(in);
    if (!in.ok())
        return false;
    message.guid = in.remaining() >= 8 ? in.u64_be() : 0; // RakNet tolerates a missing GUID
    return true;
}

Bytes encode(const UnconnectedPong& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::unconnected_pong);
    out.u64_be(message.time);
    out.u64_be(message.guid);
    write_magic(out);
    out.bytes(message.data);
    return bytes;
}

bool decode(ByteView datagram, UnconnectedPong& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::unconnected_pong))
        return false;
    message.time = in.u64_be();
    message.guid = in.u64_be();
    skip_magic(in);
    if (!in.ok())
        return false;
    message.data = to_bytes(in.rest());
    return true;
}

Bytes encode(const OpenConnectionRequest1& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::open_connection_request_1);
    write_magic(out);
    out.u8(message.protocol);
    if (message.mtu > udp_header_size)
        out.pad_to(message.mtu - udp_header_size);
    return bytes;
}

bool decode(ByteView datagram, OpenConnectionRequest1& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::open_connection_request_1))
        return false;
    skip_magic(in);
    message.protocol = in.u8();
    message.mtu = static_cast<std::uint16_t>(std::min<std::size_t>(datagram.size() + udp_header_size, 0xFFFF));
    return in.ok();
}

Bytes encode(const OpenConnectionReply1& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::open_connection_reply_1);
    write_magic(out);
    out.u64_be(message.guid);
    out.u8(0); // no cookie: security is not supported
    out.u16_be(message.mtu);
    if (message.padded && message.mtu > udp_header_size)
        out.pad_to(message.mtu - udp_header_size);
    return bytes;
}

bool decode(ByteView datagram, OpenConnectionReply1& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::open_connection_reply_1))
        return false;
    skip_magic(in);
    message.guid = in.u64_be();
    message.has_security = in.u8() != 0;
    if (!in.ok())
        return false;
    if (message.has_security) // a cookie and public key follow; the caller has to give up anyway
    {
        message.mtu = 0;
        return true;
    }
    message.mtu = in.u16_be();
    return in.ok();
}

Bytes encode(const OpenConnectionRequest2& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::open_connection_request_2);
    write_magic(out);
    write_address(out, message.server_address);
    out.u16_be(message.mtu);
    out.u64_be(message.guid);
    return bytes;
}

bool decode(ByteView datagram, OpenConnectionRequest2& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::open_connection_request_2))
        return false;
    skip_magic(in);
    if (!read_address(in, message.server_address))
        return false;
    message.mtu = in.u16_be();
    message.guid = in.u64_be();
    return in.ok();
}

Bytes encode(const OpenConnectionReply2& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::open_connection_reply_2);
    write_magic(out);
    out.u64_be(message.guid);
    write_address(out, message.client_address);
    out.u16_be(message.mtu);
    // RakNet writes this flag as a single bit, so it lands in the top bit of a final, padded byte.
    out.u8(message.requires_security ? 0x80 : 0x00);
    return bytes;
}

bool decode(ByteView datagram, OpenConnectionReply2& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::open_connection_reply_2))
        return false;
    skip_magic(in);
    message.guid = in.u64_be();
    if (!read_address(in, message.client_address))
        return false;
    message.mtu = in.u16_be();
    message.requires_security = (in.u8() & 0x80) != 0;
    return in.ok();
}

Bytes encode(const Rejection& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(message.id);
    write_magic(out);
    out.u64_be(message.guid);
    return bytes;
}

bool decode(ByteView datagram, Rejection& message)
{
    ByteReader in(datagram);
    message.id = in.u8();
    skip_magic(in);
    message.guid = in.u64_be();
    return in.ok();
}

Bytes encode(const IncompatibleProtocolVersion& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::incompatible_protocol_version);
    out.u8(message.protocol);
    write_magic(out);
    out.u64_be(message.guid);
    return bytes;
}

bool decode(ByteView datagram, IncompatibleProtocolVersion& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::incompatible_protocol_version))
        return false;
    message.protocol = in.u8();
    skip_magic(in);
    message.guid = in.u64_be();
    return in.ok();
}

Bytes encode(const OutOfBand& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::out_of_band_internal);
    out.u64_be(message.guid);
    write_magic(out);
    out.bytes(message.data);
    return bytes;
}

bool decode(ByteView datagram, OutOfBand& message)
{
    ByteReader in(datagram);
    if (!expect_id(in, id::out_of_band_internal))
        return false;
    message.guid = in.u64_be();
    skip_magic(in);
    if (!in.ok())
        return false;
    message.data = to_bytes(in.rest());
    return true;
}

// --- Connected messages --------------------------------------------------------------------------

Bytes encode(const ConnectionRequest& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::connection_request);
    out.u64_be(message.guid);
    out.u64_be(message.time);
    out.u8(0); // doSecurity
    out.bytes(message.password);
    return bytes;
}

bool decode(ByteView payload, ConnectionRequest& message)
{
    ByteReader in(payload);
    if (!expect_id(in, id::connection_request))
        return false;
    message.guid = in.u64_be();
    message.time = in.u64_be();
    const bool security = in.u8() != 0;
    if (!in.ok() || security)
        return false;
    message.password = to_bytes(in.rest());
    return true;
}

Bytes encode(const ConnectionRequestAccepted& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::connection_request_accepted);
    write_address(out, message.client_address);
    out.u16_be(message.system_index);
    for (const Endpoint& address : message.internal_addresses)
        write_address(out, address);
    out.u64_be(message.request_time);
    out.u64_be(message.accept_time);
    return bytes;
}

bool decode(ByteView payload, ConnectionRequestAccepted& message)
{
    ByteReader in(payload);
    if (!expect_id(in, id::connection_request_accepted) || !read_address(in, message.client_address))
        return false;
    message.system_index = in.u16_be();
    for (Endpoint& address : message.internal_addresses)
        if (!read_address(in, address))
            return false;
    message.request_time = in.u64_be();
    message.accept_time = in.u64_be();
    return in.ok();
}

Bytes encode(const NewIncomingConnection& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::new_incoming_connection);
    write_address(out, message.server_address);
    for (const Endpoint& address : message.internal_addresses)
        write_address(out, address);
    out.u64_be(message.accept_time);
    out.u64_be(message.time);
    return bytes;
}

bool decode(ByteView payload, NewIncomingConnection& message)
{
    ByteReader in(payload);
    if (!expect_id(in, id::new_incoming_connection) || !read_address(in, message.server_address))
        return false;
    for (Endpoint& address : message.internal_addresses)
        if (!read_address(in, address))
            return false;
    message.accept_time = in.u64_be();
    message.time = in.u64_be();
    return in.ok();
}

Bytes encode(const ConnectedPing& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::connected_ping);
    out.u64_be(message.time);
    return bytes;
}

bool decode(ByteView payload, ConnectedPing& message)
{
    ByteReader in(payload);
    if (!expect_id(in, id::connected_ping))
        return false;
    message.time = in.u64_be();
    return in.ok();
}

Bytes encode(const ConnectedPong& message)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::connected_pong);
    out.u64_be(message.ping_time);
    out.u64_be(message.pong_time);
    return bytes;
}

bool decode(ByteView payload, ConnectedPong& message)
{
    ByteReader in(payload);
    if (!expect_id(in, id::connected_pong))
        return false;
    message.ping_time = in.u64_be();
    message.pong_time = in.u64_be();
    return in.ok();
}

Bytes encode_invalid_password(std::uint64_t guid)
{
    Bytes bytes;
    ByteWriter out(bytes);
    out.u8(id::invalid_password);
    out.u64_be(guid);
    return bytes;
}

} // namespace wire
} // namespace raknet
} // namespace pzynet
