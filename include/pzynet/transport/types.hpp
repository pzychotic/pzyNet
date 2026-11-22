#pragma once

#include "pzynet/core/address.hpp"

#include <cstdint>
#include <variant>
#include <vector>

namespace pzynet
{
namespace transport
{

/// Identifies one connection of one engine. Never reused by that engine; 0 is never a valid id.
using ConnectionId = std::uint64_t;
constexpr ConnectionId invalid_connection = 0;

enum class Reliability : std::uint8_t
{
    unreliable,           ///< may be lost, duplicated or arrive out of order
    unreliable_sequenced, ///< may be lost; messages older than the newest delivered one are dropped
    reliable,             ///< delivered exactly once, in any order
    reliable_ordered,     ///< delivered exactly once, in sending order (per channel)
    reliable_sequenced,   ///< retransmitted, but dropped if a newer one was already delivered
};

enum class Priority : std::uint8_t
{
    immediate,
    high,
    medium,
    low,
};

struct SendOptions
{
    Reliability reliability = Reliability::reliable_ordered;
    /// Ordering/sequencing stream; messages on different channels do not wait for each other.
    std::uint8_t channel = 0;
    Priority priority = Priority::medium;
    /// Non-zero: a SendReceipt event carrying this token reports the fate of the message.
    std::uint32_t receipt_token = 0;
};

enum class DisconnectMode
{
    graceful, ///< notify the peer and flush pending reliable data first
    abrupt,   ///< drop the connection now; the peer finds out by timeout
};

enum class ConnectFailure
{
    no_response,
    incompatible_protocol,
    connection_refused, ///< peer has no free slot or does not accept connections
    already_connected,
    banned,
    invalid_credentials,
    unsupported, ///< peer demands a feature this implementation lacks (e.g. transport security)
    ip_recently_connected, ///< peer refuses another connection from this IP so soon after the last one
};

enum class DisconnectReason
{
    closed_locally,
    closed_by_peer,
    timed_out,
    protocol_violation,
};

struct Connected
{
    ConnectionId connection = invalid_connection;
    Endpoint remote;
    bool incoming = false; ///< true if the peer initiated the connection
};

struct ConnectFailed
{
    ConnectionId connection = invalid_connection;
    Endpoint remote;
    ConnectFailure reason = ConnectFailure::no_response;
};

struct Disconnected
{
    ConnectionId connection = invalid_connection;
    Endpoint remote;
    DisconnectReason reason = DisconnectReason::closed_locally;
};

struct Message
{
    ConnectionId connection = invalid_connection;
    Reliability reliability = Reliability::unreliable;
    std::uint8_t channel = 0; ///< meaningful for ordered and sequenced messages
    std::vector<std::uint8_t> data;
};

/// Outcome of a send that carried a receipt_token: delivered (acknowledged by the peer) or lost.
struct SendReceipt
{
    ConnectionId connection = invalid_connection;
    std::uint32_t token = 0;
    bool delivered = false;
};

/// A connectionless datagram addressed to the application (e.g. a discovery reply). `data` is the
/// datagram as the protocol defines it; the protocol layer offers a parser.
struct Unconnected
{
    Endpoint from;
    std::vector<std::uint8_t> data;
};

using Event = std::variant<Connected, ConnectFailed, Disconnected, Message, SendReceipt, Unconnected>;

/// One datagram an engine wants put on the wire.
struct Transmit
{
    Endpoint to;
    std::vector<std::uint8_t> data;
    bool dont_fragment = false; ///< send with the IP "don't fragment" flag (path-MTU probe)
};

} // namespace transport
} // namespace pzynet
