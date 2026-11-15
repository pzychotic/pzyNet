#include "pzynet/core/address.hpp"

#include <cstdio>

namespace pzynet
{
namespace
{

constexpr std::array<std::uint8_t, 12> v4_mapped_prefix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

bool parse_decimal(std::string_view text, std::uint32_t max, std::uint32_t& out) noexcept
{
    if (text.empty() || text.size() > 10)
        return false;
    if (text.size() > 1 && text[0] == '0') // no leading zeros: "010" is ambiguous (octal?)
        return false;
    std::uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > max)
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_v4(std::string_view text, std::array<std::uint8_t, 4>& out) noexcept
{
    for (int i = 0; i < 4; ++i)
    {
        const std::size_t dot = text.find('.');
        const bool last = i == 3;
        if (last != (dot == std::string_view::npos))
            return false;
        std::uint32_t octet = 0;
        if (!parse_decimal(text.substr(0, dot), 255, octet))
            return false;
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(octet);
        if (!last)
            text.remove_prefix(dot + 1);
    }
    return true;
}

bool parse_hex_group(std::string_view text, std::uint16_t& out) noexcept
{
    if (text.empty() || text.size() > 4)
        return false;
    unsigned value = 0;
    for (char c : text)
    {
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<unsigned>(c - 'A') + 10;
        else
            return false;
        value = value * 16 + digit;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_v6(std::string_view text, std::array<std::uint8_t, 16>& out, std::uint32_t& scope) noexcept
{
    scope = 0;
    if (const std::size_t percent = text.find('%'); percent != std::string_view::npos)
    {
        if (!parse_decimal(text.substr(percent + 1), 0xFFFFFFFFu, scope))
            return false;
        text = text.substr(0, percent);
    }

    std::array<std::uint16_t, 8> head{}, tail{};
    std::size_t head_count = 0, tail_count = 0;
    bool compressed = false; // seen "::"

    std::size_t pos = 0;
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':')
    {
        compressed = true;
        pos = 2;
    }
    else if (!text.empty() && text[0] == ':')
        return false;

    while (pos < text.size())
    {
        std::size_t end = text.find(':', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view token = text.substr(pos, end - pos);

        std::uint16_t groups[2];
        std::size_t group_count = 1;
        if (token.find('.') != std::string_view::npos)
        {
            // Embedded IPv4 is only valid as the final 32 bits.
            std::array<std::uint8_t, 4> v4{};
            if (end != text.size() || !parse_v4(token, v4))
                return false;
            groups[0] = static_cast<std::uint16_t>((v4[0] << 8) | v4[1]);
            groups[1] = static_cast<std::uint16_t>((v4[2] << 8) | v4[3]);
            group_count = 2;
        }
        else if (!parse_hex_group(token, groups[0]))
            return false;

        for (std::size_t i = 0; i < group_count; ++i)
        {
            if (head_count + tail_count >= 8)
                return false;
            if (compressed)
                tail[tail_count++] = groups[i];
            else
                head[head_count++] = groups[i];
        }

        pos = end;
        if (pos == text.size())
            break;
        if (pos + 1 < text.size() && text[pos + 1] == ':')
        {
            if (compressed)
                return false;
            compressed = true;
            pos += 2;
        }
        else
        {
            ++pos;
            if (pos == text.size()) // trailing single ':'
                return false;
        }
    }

    const std::size_t total = head_count + tail_count;
    if (compressed ? total > 7 : total != 8)
        return false;

    std::array<std::uint16_t, 8> all{};
    for (std::size_t i = 0; i < head_count; ++i)
        all[i] = head[i];
    for (std::size_t i = 0; i < tail_count; ++i)
        all[8 - tail_count + i] = tail[i];
    for (std::size_t i = 0; i < 8; ++i)
    {
        out[i * 2] = static_cast<std::uint8_t>(all[i] >> 8);
        out[i * 2 + 1] = static_cast<std::uint8_t>(all[i]);
    }
    return true;
}

bool has_v4_mapped_prefix(const std::array<std::uint8_t, 16>& bytes) noexcept
{
    for (std::size_t i = 0; i < v4_mapped_prefix.size(); ++i)
        if (bytes[i] != v4_mapped_prefix[i])
            return false;
    return true;
}

std::string format_v4(const std::uint8_t* b)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buffer;
}

} // namespace

std::optional<IpAddress> IpAddress::parse(std::string_view text) noexcept
{
    if (text.find(':') == std::string_view::npos)
    {
        std::array<std::uint8_t, 4> v4_bytes{};
        if (!parse_v4(text, v4_bytes))
            return std::nullopt;
        return IpAddress::v4(v4_bytes);
    }
    std::array<std::uint8_t, 16> v6_bytes{};
    std::uint32_t scope = 0;
    if (!parse_v6(text, v6_bytes, scope))
        return std::nullopt;
    return IpAddress::v6(v6_bytes, scope);
}

bool IpAddress::is_unspecified() const noexcept
{
    const std::size_t count = is_v4() ? 4 : 16;
    for (std::size_t i = 0; i < count; ++i)
        if (bytes_[i] != 0)
            return false;
    return true;
}

bool IpAddress::is_loopback() const noexcept
{
    if (is_v4())
        return bytes_[0] == 127;
    if (is_v4_mapped())
        return bytes_[12] == 127;
    return *this == IpAddress::v6(loopback_v6().v6_bytes(), scope_);
}

bool IpAddress::is_multicast() const noexcept
{
    return is_v4() ? (bytes_[0] & 0xF0) == 0xE0 : bytes_[0] == 0xFF;
}

bool IpAddress::is_v4_mapped() const noexcept
{
    return is_v6() && has_v4_mapped_prefix(bytes_);
}

IpAddress IpAddress::to_v4_mapped() const noexcept
{
    if (is_v6())
        return *this;
    std::array<std::uint8_t, 16> mapped{};
    for (std::size_t i = 0; i < v4_mapped_prefix.size(); ++i)
        mapped[i] = v4_mapped_prefix[i];
    for (std::size_t i = 0; i < 4; ++i)
        mapped[12 + i] = bytes_[i];
    return IpAddress::v6(mapped);
}

IpAddress IpAddress::unmapped() const noexcept
{
    if (!is_v4_mapped())
        return *this;
    return IpAddress::v4(bytes_[12], bytes_[13], bytes_[14], bytes_[15]);
}

std::string IpAddress::to_string() const
{
    if (is_v4())
        return format_v4(bytes_.data());

    std::string text;
    if (is_v4_mapped())
        text = "::ffff:" + format_v4(bytes_.data() + 12);
    else
    {
        std::array<unsigned, 8> groups{};
        for (std::size_t i = 0; i < 8; ++i)
            groups[i] = (static_cast<unsigned>(bytes_[i * 2]) << 8) | bytes_[i * 2 + 1];

        // RFC 5952: compress the longest run of two or more zero groups, leftmost on a tie.
        std::size_t best_start = 8, best_length = 1;
        for (std::size_t i = 0; i < 8;)
        {
            if (groups[i] != 0)
            {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < 8 && groups[j] == 0)
                ++j;
            if (j - i > best_length)
            {
                best_start = i;
                best_length = j - i;
            }
            i = j;
        }

        for (std::size_t i = 0; i < 8;)
        {
            if (i == best_start)
            {
                text += "::";
                i += best_length;
                continue;
            }
            if (!text.empty() && text.back() != ':')
                text += ':';
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "%x", groups[i]);
            text += buffer;
            ++i;
        }
    }
    if (scope_ != 0)
        text += '%' + std::to_string(scope_);
    return text;
}

std::optional<Endpoint> Endpoint::parse(std::string_view text) noexcept
{
    std::string_view host, port_text;
    if (!text.empty() && text[0] == '[')
    {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':')
            return std::nullopt;
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
        if (host.find(':') == std::string_view::npos) // brackets are for IPv6 only
            return std::nullopt;
    }
    else
    {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        if (host.find(':') != std::string_view::npos) // bare IPv6 with a port is ambiguous
            return std::nullopt;
    }

    std::uint32_t port = 0;
    if (!parse_decimal(port_text, 65535, port))
        return std::nullopt;
    const std::optional<IpAddress> address = IpAddress::parse(host);
    if (!address)
        return std::nullopt;
    return Endpoint(*address, static_cast<std::uint16_t>(port));
}

std::string Endpoint::to_string() const
{
    if (address_.is_v6())
        return '[' + address_.to_string() + "]:" + std::to_string(port_);
    return address_.to_string() + ':' + std::to_string(port_);
}

} // namespace pzynet

namespace
{
std::size_t fnv1a(const std::uint8_t* data, std::size_t size, std::size_t seed) noexcept
{
    std::uint64_t hash = 14695981039346656037ull ^ seed;
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return static_cast<std::size_t>(hash);
}
} // namespace

std::size_t std::hash<pzynet::IpAddress>::operator()(const pzynet::IpAddress& ip) const noexcept
{
    const std::size_t seed = (static_cast<std::size_t>(ip.scope_id()) << 1) | (ip.is_v6() ? 1u : 0u);
    return fnv1a(ip.v6_bytes().data(), ip.v6_bytes().size(), seed);
}

std::size_t std::hash<pzynet::Endpoint>::operator()(const pzynet::Endpoint& ep) const noexcept
{
    return std::hash<pzynet::IpAddress>()(ep.address()) * 31 + ep.port();
}
