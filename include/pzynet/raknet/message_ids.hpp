#pragma once

#include <cstdint>

namespace pzynet
{
namespace raknet
{

/// RakNet message identifiers: the first byte of every offline datagram and of every connected message.
/// Only the ones the core protocol uses are listed; 31..133 belong to RakNet plugins.
namespace id
{
constexpr std::uint8_t connected_ping = 0;
constexpr std::uint8_t unconnected_ping = 1;
constexpr std::uint8_t unconnected_ping_open_connections = 2;
constexpr std::uint8_t connected_pong = 3;
constexpr std::uint8_t detect_lost_connections = 4;
constexpr std::uint8_t open_connection_request_1 = 5;
constexpr std::uint8_t open_connection_reply_1 = 6;
constexpr std::uint8_t open_connection_request_2 = 7;
constexpr std::uint8_t open_connection_reply_2 = 8;
constexpr std::uint8_t connection_request = 9;
constexpr std::uint8_t remote_system_requires_public_key = 10;
constexpr std::uint8_t our_system_requires_security = 11;
constexpr std::uint8_t public_key_mismatch = 12;
constexpr std::uint8_t out_of_band_internal = 13;
constexpr std::uint8_t snd_receipt_acked = 14;
constexpr std::uint8_t snd_receipt_loss = 15;
constexpr std::uint8_t connection_request_accepted = 16;
constexpr std::uint8_t connection_attempt_failed = 17;
constexpr std::uint8_t already_connected = 18;
constexpr std::uint8_t new_incoming_connection = 19;
constexpr std::uint8_t no_free_incoming_connections = 20;
constexpr std::uint8_t disconnection_notification = 21;
constexpr std::uint8_t connection_lost = 22;
constexpr std::uint8_t connection_banned = 23;
constexpr std::uint8_t invalid_password = 24;
constexpr std::uint8_t incompatible_protocol_version = 25;
constexpr std::uint8_t ip_recently_connected = 26;
constexpr std::uint8_t timestamp = 27;
constexpr std::uint8_t unconnected_pong = 28;
constexpr std::uint8_t advertise_system = 29;
constexpr std::uint8_t download_progress = 30;

/// First identifier free for application messages when talking to RakNet peers.
constexpr std::uint8_t user_packet_enum = 134;
} // namespace id

} // namespace raknet
} // namespace pzynet
