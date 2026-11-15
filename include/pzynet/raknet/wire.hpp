#pragma once

// Byte-exact encoders and decoders for the RakNet 4.x wire format (protocol version 6, no security,
// IPv4 address encoding). Pure functions over byte buffers; the engine is built on top of them.
//
// Conventions on the wire: 16/32/64-bit integers are big-endian, 24-bit sequence numbers are
// little-endian, and every structure is a whole number of bytes.

#include "pzynet/core/address.hpp"
#include "pzynet/core/byte_io.hpp"
#include "pzynet/core/span.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace pzynet
{
namespace raknet
{
namespace wire
{

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t protocol_version = 6;
/// IP header (20) + UDP header (8): what RakNet subtracts from an MTU to get the UDP payload size.
constexpr std::size_t udp_header_size = 28;
constexpr std::uint16_t maximum_mtu = 1492;
constexpr std::uint16_t minimum_mtu = 576;
constexpr std::size_t internal_address_count = 10;
constexpr std::size_t ordering_channel_count = 32;

/// Marks a datagram as a connectionless ("offline") message.
constexpr std::array<std::uint8_t, 16> offline_magic = {0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE,
                                                        0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};

// --- SystemAddress -------------------------------------------------------------------------------

constexpr std::size_t address_size = 7;

/// 7 bytes: version (4), the address bitwise inverted, the port. A non-IPv4 endpoint is written as
/// 0.0.0.0 with its port, since the IPv4 encoding has no room for it.
void write_address(ByteWriter& out, const Endpoint& endpoint);
bool read_address(ByteReader& in, Endpoint& endpoint);

// --- ACK / NAK range list ------------------------------------------------------------------------

/// An inclusive run of 24-bit datagram numbers.
struct Range
{
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};
inline bool operator==(const Range& a, const Range& b) noexcept { return a.first == b.first && a.last == b.last; }

/// Writes as many ranges as fit into `max_bytes`; returns how many were written.
std::size_t write_ranges(ByteWriter& out, Span<const Range> ranges, std::size_t max_bytes);
bool read_ranges(ByteReader& in, std::vector<Range>& ranges);

// --- Datagram header -----------------------------------------------------------------------------

enum class DatagramKind
{
    data,
    ack,
    nak
};

struct DatagramHeader
{
    DatagramKind kind = DatagramKind::data;
    // data datagrams
    bool packet_pair = false;
    bool continuous_send = false;
    bool needs_b_and_as = false;
    std::uint32_t number = 0; ///< 24-bit datagram sequence number
    // ack datagrams
    bool has_b_and_as = false;
    float arrival_rate = 0.0f; ///< "AS"; only present when has_b_and_as
};

constexpr std::size_t data_datagram_header_size = 4;

void write_datagram_header(ByteWriter& out, const DatagramHeader& header);
/// False if the datagram is truncated or its "valid" bit is clear (i.e. it is not a connected datagram).
bool read_datagram_header(ByteReader& in, DatagramHeader& header);

// --- Message ("internal packet") header ----------------------------------------------------------

/// The five reliabilities that exist on the wire. (RakNet's *_WITH_ACK_RECEIPT variants are local only.)
enum class Reliability : std::uint8_t
{
    unreliable = 0,
    unreliable_sequenced = 1,
    reliable = 2,
    reliable_ordered = 3,
    reliable_sequenced = 4,
};
constexpr bool is_reliable(Reliability r) noexcept
{
    return r == Reliability::reliable || r == Reliability::reliable_ordered || r == Reliability::reliable_sequenced;
}
constexpr bool is_sequenced(Reliability r) noexcept
{
    return r == Reliability::unreliable_sequenced || r == Reliability::reliable_sequenced;
}
/// True if the header carries an ordering index and channel.
constexpr bool has_ordering(Reliability r) noexcept
{
    return is_sequenced(r) || r == Reliability::reliable_ordered;
}

struct MessageHeader
{
    Reliability reliability = Reliability::unreliable;
    bool split = false;
    std::uint16_t bit_length = 0;          ///< payload length in bits
    std::uint32_t reliable_number = 0;     ///< present iff is_reliable
    std::uint32_t sequencing_index = 0;    ///< present iff is_sequenced
    std::uint32_t ordering_index = 0;      ///< present iff has_ordering
    std::uint8_t ordering_channel = 0;     ///< present iff has_ordering
    std::uint32_t split_count = 0;         ///< present iff split
    std::uint16_t split_id = 0;            ///< present iff split
    std::uint32_t split_index = 0;         ///< present iff split

    std::size_t payload_size() const noexcept { return (static_cast<std::size_t>(bit_length) + 7) / 8; }
};

constexpr std::size_t message_header_size(Reliability r, bool split) noexcept
{
    return 3 + (is_reliable(r) ? 3u : 0u) + (is_sequenced(r) ? 3u : 0u) + (has_ordering(r) ? 4u : 0u) +
           (split ? 10u : 0u);
}
constexpr std::size_t maximum_message_header_size = 23; // reliable_sequenced + split

void write_message_header(ByteWriter& out, const MessageHeader& header);
/// Reads and validates a header the way RakNet does: rejects empty payloads, unknown reliabilities,
/// channels >= 32 and split indices outside the split count.
bool read_message_header(ByteReader& in, MessageHeader& header);

// --- Offline (connectionless) messages -----------------------------------------------------------

/// If `datagram` is an offline message (known id with the magic at that id's offset), returns its id.
std::optional<std::uint8_t> offline_message_id(ByteView datagram) noexcept;

struct UnconnectedPing
{
    std::uint64_t time = 0;
    std::uint64_t guid = 0;
    bool open_connections_only = false; ///< ID_UNCONNECTED_PING_OPEN_CONNECTIONS
};
Bytes encode(const UnconnectedPing& message);
bool decode(ByteView datagram, UnconnectedPing& message);

struct UnconnectedPong
{
    std::uint64_t time = 0; ///< the ping's time, echoed
    std::uint64_t guid = 0;
    Bytes data; ///< the responder's offline ping response
};
Bytes encode(const UnconnectedPong& message);
bool decode(ByteView datagram, UnconnectedPong& message);

struct OpenConnectionRequest1
{
    std::uint8_t protocol = protocol_version;
    /// The MTU being probed: the datagram is zero-padded so that IP + UDP + payload is this long.
    std::uint16_t mtu = maximum_mtu;
};
Bytes encode(const OpenConnectionRequest1& message);
bool decode(ByteView datagram, OpenConnectionRequest1& message);

struct OpenConnectionReply1
{
    std::uint64_t guid = 0;
    bool has_security = false; ///< always false when written; a true value read means "unsupported"
    std::uint16_t mtu = 0;
    bool padded = true; ///< zero-pad to the MTU so the reply probes the reverse path (encode only)
};
Bytes encode(const OpenConnectionReply1& message);
bool decode(ByteView datagram, OpenConnectionReply1& message);

struct OpenConnectionRequest2
{
    Endpoint server_address; ///< the address the client used to reach the server
    std::uint16_t mtu = 0;
    std::uint64_t guid = 0;
};
Bytes encode(const OpenConnectionRequest2& message);
bool decode(ByteView datagram, OpenConnectionRequest2& message);

struct OpenConnectionReply2
{
    std::uint64_t guid = 0;
    Endpoint client_address; ///< the client's address as the server sees it
    std::uint16_t mtu = 0;
    bool requires_security = false;
};
Bytes encode(const OpenConnectionReply2& message);
bool decode(ByteView datagram, OpenConnectionReply2& message);

/// The handshake refusals that share one layout: ID_ALREADY_CONNECTED, ID_NO_FREE_INCOMING_CONNECTIONS,
/// ID_CONNECTION_BANNED, ID_IP_RECENTLY_CONNECTED, ID_CONNECTION_ATTEMPT_FAILED.
struct Rejection
{
    std::uint8_t id = 0;
    std::uint64_t guid = 0;
};
Bytes encode(const Rejection& message);
bool decode(ByteView datagram, Rejection& message);

struct IncompatibleProtocolVersion
{
    std::uint8_t protocol = protocol_version; ///< the sender's own version
    std::uint64_t guid = 0;
};
Bytes encode(const IncompatibleProtocolVersion& message);
bool decode(ByteView datagram, IncompatibleProtocolVersion& message);

/// ID_OUT_OF_BAND_INTERNAL; used for ID_ADVERTISE_SYSTEM, which is then the first byte of `data`.
struct OutOfBand
{
    std::uint64_t guid = 0;
    Bytes data;
};
Bytes encode(const OutOfBand& message);
bool decode(ByteView datagram, OutOfBand& message);

// --- Connected handshake and keep-alive messages (payloads of reliable messages) -----------------

struct ConnectionRequest
{
    std::uint64_t guid = 0;
    std::uint64_t time = 0;
    Bytes password;
};
Bytes encode(const ConnectionRequest& message);
bool decode(ByteView payload, ConnectionRequest& message);

struct ConnectionRequestAccepted
{
    Endpoint client_address;
    std::uint16_t system_index = 0;
    std::array<Endpoint, internal_address_count> internal_addresses{};
    std::uint64_t request_time = 0; ///< echoed from ConnectionRequest
    std::uint64_t accept_time = 0;
};
Bytes encode(const ConnectionRequestAccepted& message);
bool decode(ByteView payload, ConnectionRequestAccepted& message);

struct NewIncomingConnection
{
    Endpoint server_address;
    std::array<Endpoint, internal_address_count> internal_addresses{};
    std::uint64_t accept_time = 0; ///< echoed from ConnectionRequestAccepted
    std::uint64_t time = 0;
};
Bytes encode(const NewIncomingConnection& message);
bool decode(ByteView payload, NewIncomingConnection& message);

struct ConnectedPing
{
    std::uint64_t time = 0;
};
Bytes encode(const ConnectedPing& message);
bool decode(ByteView payload, ConnectedPing& message);

struct ConnectedPong
{
    std::uint64_t ping_time = 0; ///< echoed
    std::uint64_t pong_time = 0;
};
Bytes encode(const ConnectedPong& message);
bool decode(ByteView payload, ConnectedPong& message);

/// ID_INVALID_PASSWORD followed by the server's GUID.
Bytes encode_invalid_password(std::uint64_t guid);

} // namespace wire
} // namespace raknet
} // namespace pzynet
