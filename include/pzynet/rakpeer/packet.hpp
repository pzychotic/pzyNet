#pragma once

#include "pzynet/core/address.hpp"
#include "pzynet/raknet/bit_stream.hpp"
#include "pzynet/raknet/guid.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace pzynet
{
namespace raknet
{

/// One item from a Peer's receive queue: a received message or a connection event, named by its
/// message id. For a connection event, `data` holds just the id.
struct Packet
{
    /// The message id: data[0].
    std::uint8_t id = 0;
    /// The remote system this Packet is from or about.
    Guid guid;
    Endpoint address;
    /// The whole message, starting at the message id.
    std::vector<std::uint8_t> data;
    /// The byte order of the Peer that received this Packet; reader() uses it.
    ByteOrder byte_order = ByteOrder::big_endian;

    /// A BitStream reader over the message contents, positioned after the message id.
    BitStreamReader reader() const noexcept
    {
        return BitStreamReader(ByteView(data.data(), data.size()).subspan(1), byte_order);
    }
};

/// The receipt number of an id::snd_receipt_acked or id::snd_receipt_loss Packet: the 4 bytes after the
/// id, in host byte order as RakNet stores them. 0 if the Packet is too short.
inline std::uint32_t receipt_number(const Packet& packet) noexcept
{
    std::uint32_t number = 0;
    if (packet.data.size() >= 1 + sizeof number)
        std::memcpy(&number, packet.data.data() + 1, sizeof number);
    return number;
}

} // namespace raknet
} // namespace pzynet
