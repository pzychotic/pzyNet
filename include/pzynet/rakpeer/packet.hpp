#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/raknet/bit_stream.hpp"
#include "pzynet/raknet/guid.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace pzynet
{
namespace raknet
{

/// One item from a Peer's receive queue: a received message or a connection event, named by its
/// message id. For a connection event, `data` holds just the id.
struct Packet
{
    /// The message id: data[0]. For a timestamped message this is id::timestamp, and the real message
    /// id follows the 8-byte time, at data[9].
    std::uint8_t id = 0;
    /// The remote system this Packet is from or about.
    Guid guid;
    Endpoint address;
    /// The whole message, starting at the message id. A timestamped message's time has already been
    /// rewritten into local time, as RakPeer does.
    std::vector<std::uint8_t> data;
    /// The byte order of the Peer that received this Packet; reader() uses it.
    ByteOrder byte_order = ByteOrder::big_endian;
    /// For a timestamped message, its send time converted into this Peer's clock (comparable with
    /// Peer::time()); nullopt for any other Packet, including one whose timestamp header is cut short.
    std::optional<std::uint64_t> timestamp;

    /// A BitStream reader over the message contents, positioned after the header: after the message
    /// id, or for a timestamped message after id::timestamp and the time, i.e. at the real message id.
    BitStreamReader reader() const noexcept
    {
        const std::size_t header = timestamp ? timestamp_header_size : 1;
        return BitStreamReader(ByteView(data.data(), data.size()).subspan(std::min(header, data.size())),
                               byte_order);
    }

    /// id::timestamp followed by the 8-byte time.
    static constexpr std::size_t timestamp_header_size = 9;
};

/// Numbers a send (Peer::send() returns it); receipt Packets carry it. Never 0 for an accepted send.
using ReceiptNumber = std::uint32_t;

/// The receipt number of an id::snd_receipt_acked or id::snd_receipt_loss Packet: the 4 bytes after the
/// id, in host byte order as RakNet stores them. 0 if the Packet is too short.
inline ReceiptNumber receipt_number(const Packet& packet) noexcept
{
    ReceiptNumber number = 0;
    if (packet.data.size() >= 1 + sizeof number)
        std::memcpy(&number, packet.data.data() + 1, sizeof number);
    return number;
}

} // namespace raknet
} // namespace pzynet
