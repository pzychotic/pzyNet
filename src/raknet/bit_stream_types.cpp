// BitStream encodings of RakNet's own types and strings, as upstream RakNet 4.081 writes them
// (BitStream.h, RakString.cpp, RakWString.cpp). Built only on the streams' public primitives.

#include "pzynet/raknet/bit_stream.hpp"

namespace pzynet
{
namespace raknet
{

// --- 24-bit integers -------------------------------------------------------------------------------

void BitStreamWriter::write_uint24(std::uint32_t value)
{
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                   static_cast<std::uint8_t>(value >> 16)};
    write_aligned_bytes(ByteView(bytes, 3));
}

std::uint32_t BitStreamReader::read_uint24()
{
    std::uint8_t bytes[3] = {};
    if (!read_aligned_bytes(MutableByteView(bytes, 3)))
        return 0;
    return bytes[0] | (std::uint32_t{bytes[1]} << 8) | (std::uint32_t{bytes[2]} << 16);
}

// --- System addresses ------------------------------------------------------------------------------

namespace
{
constexpr std::uint8_t ipv4_version = 4;
}

void BitStreamWriter::write(const Endpoint& endpoint, std::error_code& ec)
{
    // RakNet writes an IPv6 SystemAddress as version 6 and a raw sockaddr_in6, and only when built with
    // IPv6 support; a build without it writes the version byte alone. Neither is worth reproducing.
    if (!endpoint.address().is_v4())
    {
        ec = make_error_code(Errc::address_family_mismatch);
        return;
    }
    ec.clear();
    const auto address = endpoint.address().v4_bytes();
    const std::uint8_t bytes[7] = {ipv4_version,
                                   static_cast<std::uint8_t>(~address[0]),
                                   static_cast<std::uint8_t>(~address[1]),
                                   static_cast<std::uint8_t>(~address[2]),
                                   static_cast<std::uint8_t>(~address[3]),
                                   static_cast<std::uint8_t>(endpoint.port() >> 8),
                                   static_cast<std::uint8_t>(endpoint.port())};
    write_bytes(ByteView(bytes, sizeof bytes));
}

void write(BitStreamWriter& out, const Endpoint& endpoint)
{
    std::error_code ec;
    out.write(endpoint, ec);
    detail::throw_if(ec, "BitStreamWriter::write(Endpoint)");
}

void read(BitStreamReader& in, Endpoint& endpoint)
{
    if (in.read<std::uint8_t>() != ipv4_version)
    {
        in.fail();
        return;
    }
    std::uint8_t bytes[6] = {};
    if (!in.read_bytes(MutableByteView(bytes, sizeof bytes)))
        return;
    const auto port = static_cast<std::uint16_t>((bytes[4] << 8) | bytes[5]);
    endpoint = Endpoint(IpAddress::v4(static_cast<std::uint8_t>(~bytes[0]), static_cast<std::uint8_t>(~bytes[1]),
                                      static_cast<std::uint8_t>(~bytes[2]), static_cast<std::uint8_t>(~bytes[3])),
                        port);
}

// --- Length-prefixed strings -----------------------------------------------------------------------

namespace
{
constexpr std::size_t max_string_length = 0xFFFF; // the uint16 length prefix
}

void BitStreamWriter::write_string(std::string_view text, std::error_code& ec)
{
    if (text.size() > max_string_length)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    ec.clear();
    write(static_cast<std::uint16_t>(text.size()));
    write_aligned_bytes(ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void BitStreamWriter::write_string(std::string_view text)
{
    std::error_code ec;
    write_string(text, ec);
    detail::throw_if(ec, "BitStreamWriter::write_string");
}

std::string BitStreamReader::read_string()
{
    const std::size_t length = read<std::uint16_t>();
    align();
    if (!ok_ || bits_remaining() < length * 8)
    {
        fail();
        return {};
    }
    std::string text(length, '\0');
    read_bytes(MutableByteView(reinterpret_cast<std::uint8_t*>(&text[0]), length));
    return text;
}

// --- Wide strings ----------------------------------------------------------------------------------
// Ticket #5 asks for "the non-Huffman path for compressed strings". We read that as upstream 4.081's
// WriteCompressed(RakWString) / WriteCompressed(const wchar_t*), which serialize with
// RakWString::Serialize instead of the StringCompressor (RakWString.cpp:289-309): a uint16 count,
// then one uint16 per wchar_t. We take UTF-16 code units: exactly what upstream writes where wchar_t
// is UTF-16 (Windows), and for every Basic Multilingual Plane character where wchar_t is 32 bits;
// there upstream truncates characters beyond the BMP to 16 bits, which is not worth reproducing.
// (WriteAlignedBytesSafe, a compressed length then aligned bytes, is a raw-bytes helper rather than
// a string encoding and is not covered here.)

void BitStreamWriter::write_wide_string(std::u16string_view text, std::error_code& ec)
{
    if (text.size() > max_string_length)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    ec.clear();
    write(static_cast<std::uint16_t>(text.size()));
    for (const char16_t unit : text)
        write(static_cast<std::uint16_t>(unit));
}

void BitStreamWriter::write_wide_string(std::u16string_view text)
{
    std::error_code ec;
    write_wide_string(text, ec);
    detail::throw_if(ec, "BitStreamWriter::write_wide_string");
}

std::u16string BitStreamReader::read_wide_string()
{
    const std::size_t length = read<std::uint16_t>();
    if (!ok_ || bits_remaining() < length * 16)
    {
        fail();
        return {};
    }
    std::u16string text(length, u'\0');
    for (char16_t& unit : text)
        unit = static_cast<char16_t>(read<std::uint16_t>());
    return text;
}

} // namespace raknet
} // namespace pzynet
