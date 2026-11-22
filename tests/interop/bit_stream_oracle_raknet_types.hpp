#pragma once

// Steps and generators for RakNet's own types and strings (ticket #5), on the harness in
// bit_stream_oracle.hpp: 24-bit integers, GUIDs, IPv4 system addresses, length-prefixed strings and
// wide strings.
//
// Where the fork differs from upstream RakNet 4.081 (ADR-0003):
// - Wide strings: the fork deleted RakWString, so there is no RakNet function to call. wide_string()
//   runs upstream's RakWString::Serialize / Deserialize algorithm (RakWString.cpp:289-350: a
//   WriteCasted<unsigned short> count, then Write(uint16_t) per code unit) on the fork's BitStream,
//   which checks the primitives it is built from; the upstream vectors in
//   tests/raknet/bit_stream_raknet_types_test.cpp pin the format itself.
// - Strings: the fork's Write(std::string) writes size() bytes, upstream's RakString strlen() bytes.
//   The generators draw strings without zero bytes, where both agree; the embedded-zero case is
//   pinned by a unit test.

#include "bit_stream_oracle.hpp"

#include "RakNetTypes.h"

#include <array>
#include <cstring>
#include <string>

namespace oracle
{

/// A 24-bit integer (write_uint24 / Write(uint24_t)).
inline Step uint24(std::uint32_t value)
{
    value &= 0xFFFFFF;
    Step step;
    step.what = "uint24 " + std::to_string(value);
    step.write_ours = [value](rn::BitStreamWriter& out) { out.write_uint24(value); };
    step.write_theirs = [value](RakNet::BitStream& out) { out.Write(RakNet::uint24_t(value)); };
    step.read_ours = [value](rn::BitStreamReader& in) { return in.read_uint24() == value; };
    step.read_theirs = [value](RakNet::BitStream& in) {
        RakNet::uint24_t read(0xEEEEEEu);
        return in.Read(read) && read.val == value;
    };
    return step;
}

/// A GUID (write(Guid) / Write(RakNetGUID)).
inline Step guid(std::uint64_t value)
{
    Step step;
    step.what = "GUID " + std::to_string(value);
    step.write_ours = [value](rn::BitStreamWriter& out) { out.write(rn::Guid{value}); };
    step.write_theirs = [value](RakNet::BitStream& out) { out.Write(RakNet::RakNetGUID(value)); };
    step.read_ours = [value](rn::BitStreamReader& in) { return in.read<rn::Guid>() == rn::Guid{value}; };
    step.read_theirs = [value](RakNet::BitStream& in) {
        RakNet::RakNetGUID read;
        return in.Read(read) && read.g == value;
    };
    return step;
}

/// An IPv4 system address (write(Endpoint) / Write(SystemAddress)).
inline Step ipv4_address(std::array<std::uint8_t, 4> address, std::uint16_t port)
{
    const pzynet::Endpoint endpoint(pzynet::IpAddress::v4(address), port);
    Step step;
    step.what = "IPv4 address " + endpoint.to_string();
    step.write_ours = [endpoint](rn::BitStreamWriter& out) { out.write(endpoint); };
    step.write_theirs = [address, port](RakNet::BitStream& out) {
        RakNet::SystemAddress theirs;
        theirs.address.addr4.sin_family = AF_INET;
        std::memcpy(&theirs.address.addr4.sin_addr.s_addr, address.data(), 4);
        theirs.SetPortHostOrder(port);
        out.Write(theirs);
    };
    step.read_ours = [endpoint](rn::BitStreamReader& in) { return in.read<pzynet::Endpoint>() == endpoint; };
    step.read_theirs = [address, port](RakNet::BitStream& in) {
        RakNet::SystemAddress read;
        if (!in.Read(read) || read.GetIPVersion() != 4 || read.GetPort() != port)
            return false;
        return std::memcmp(&read.address.addr4.sin_addr.s_addr, address.data(), 4) == 0;
    };
    return step;
}

/// A length-prefixed string (write_string / the fork's Write(std::string), upstream's
/// Write(RakString)). Without zero bytes, where fork and upstream agree.
inline Step string(std::string text)
{
    Step step;
    step.what = "string of " + std::to_string(text.size()) + " bytes";
    step.write_ours = [text](rn::BitStreamWriter& out) { out.write_string(text); };
    step.write_theirs = [text](RakNet::BitStream& out) { out.Write(text); };
    step.read_ours = [text](rn::BitStreamReader& in) { return in.read_string() == text; };
    step.read_theirs = [text](RakNet::BitStream& in) {
        std::string read = "junk";
        return in.Read(read) && read == text;
    };
    return step;
}

/// A wide string (write_wide_string / upstream's RakWString::Serialize, emulated - see the top).
inline Step wide_string(std::u16string text)
{
    Step step;
    step.what = "wide string of " + std::to_string(text.size()) + " code units";
    step.write_ours = [text](rn::BitStreamWriter& out) { out.write_wide_string(text); };
    step.write_theirs = [text](RakNet::BitStream& out) {
        out.WriteCasted<unsigned short>(text.size());
        for (const char16_t unit : text)
            out.Write(static_cast<std::uint16_t>(unit));
    };
    step.read_ours = [text](rn::BitStreamReader& in) { return in.read_wide_string() == text; };
    step.read_theirs = [text](RakNet::BitStream& in) {
        std::size_t count = 0;
        if (!in.ReadCasted<unsigned short>(count) || count != text.size())
            return false;
        for (const char16_t unit : text)
        {
            std::uint16_t read = 0;
            if (!in.Read(read) || read != unit)
                return false;
        }
        return true;
    };
    return step;
}

inline std::string random_text(Rng& rng, std::size_t max_size)
{
    std::string text(random_size(rng, 0, max_size), '\0');
    for (char& c : text)
        c = static_cast<char>(random_size(rng, 1, 255)); // no zero bytes
    return text;
}

inline std::u16string random_wide_text(Rng& rng, std::size_t max_size)
{
    std::u16string text(random_size(rng, 0, max_size), u'\0');
    for (char16_t& unit : text)
        unit = static_cast<char16_t>(rng());
    return text;
}

/// 24-bit integers, GUIDs, IPv4 addresses, strings (empty ones included) and wide strings.
inline std::vector<StepGenerator> raknet_type_generators()
{
    return {
        [](Rng& rng) { return uint24(static_cast<std::uint32_t>(rng())); },
        [](Rng& rng) { return guid(rng()); },
        [](Rng& rng) {
            const Bytes b = random_bytes(rng, 4);
            return ipv4_address({b[0], b[1], b[2], b[3]}, static_cast<std::uint16_t>(rng()));
        },
        [](Rng& rng) { return string(random_text(rng, 12)); },
        [](Rng& rng) { return wide_string(random_wide_text(rng, 6)); },
    };
}

} // namespace oracle
