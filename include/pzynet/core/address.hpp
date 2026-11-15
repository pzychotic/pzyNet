#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pzynet
{

/// An IPv4 or IPv6 address. Plain value type with no platform dependencies.
class IpAddress
{
public:
    enum class Family : std::uint8_t
    {
        v4,
        v6
    };

    /// 0.0.0.0
    constexpr IpAddress() noexcept = default;

    static constexpr IpAddress v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept
    {
        IpAddress ip;
        ip.bytes_[0] = a;
        ip.bytes_[1] = b;
        ip.bytes_[2] = c;
        ip.bytes_[3] = d;
        return ip;
    }
    static constexpr IpAddress v4(const std::array<std::uint8_t, 4>& b) noexcept { return v4(b[0], b[1], b[2], b[3]); }
    static constexpr IpAddress v6(const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept
    {
        IpAddress ip;
        ip.family_ = Family::v6;
        ip.bytes_ = bytes;
        ip.scope_ = scope_id;
        return ip;
    }

    static constexpr IpAddress any_v4() noexcept { return IpAddress(); }
    static constexpr IpAddress any_v6() noexcept { return v6({}); }
    static constexpr IpAddress loopback_v4() noexcept { return v4(127, 0, 0, 1); }
    static constexpr IpAddress loopback_v6() noexcept
    {
        return v6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    }
    static constexpr IpAddress broadcast_v4() noexcept { return v4(255, 255, 255, 255); }

    /// Parses dotted-quad IPv4 or RFC 4291 IPv6 text (optionally with a numeric "%scope").
    static std::optional<IpAddress> parse(std::string_view text) noexcept;

    constexpr Family family() const noexcept { return family_; }
    constexpr bool is_v4() const noexcept { return family_ == Family::v4; }
    constexpr bool is_v6() const noexcept { return family_ == Family::v6; }

    /// The four address bytes in network order. Only meaningful for is_v4().
    constexpr std::array<std::uint8_t, 4> v4_bytes() const noexcept
    {
        return {bytes_[0], bytes_[1], bytes_[2], bytes_[3]};
    }
    /// The sixteen address bytes in network order. Only meaningful for is_v6().
    constexpr const std::array<std::uint8_t, 16>& v6_bytes() const noexcept { return bytes_; }
    constexpr std::uint32_t scope_id() const noexcept { return scope_; }

    bool is_unspecified() const noexcept;
    bool is_loopback() const noexcept;
    bool is_multicast() const noexcept;
    /// True for ::ffff:a.b.c.d
    bool is_v4_mapped() const noexcept;
    /// IPv4 -> ::ffff:a.b.c.d; IPv6 addresses are returned unchanged.
    IpAddress to_v4_mapped() const noexcept;
    /// ::ffff:a.b.c.d -> a.b.c.d; everything else is returned unchanged.
    IpAddress unmapped() const noexcept;

    std::string to_string() const;

    friend bool operator==(const IpAddress& a, const IpAddress& b) noexcept
    {
        return a.family_ == b.family_ && a.scope_ == b.scope_ && a.bytes_ == b.bytes_;
    }
    friend bool operator!=(const IpAddress& a, const IpAddress& b) noexcept { return !(a == b); }
    friend bool operator<(const IpAddress& a, const IpAddress& b) noexcept
    {
        if (a.family_ != b.family_)
            return a.family_ < b.family_;
        if (a.bytes_ != b.bytes_)
            return a.bytes_ < b.bytes_;
        return a.scope_ < b.scope_;
    }

private:
    std::array<std::uint8_t, 16> bytes_{}; // IPv4 uses the first four
    std::uint32_t scope_ = 0;
    Family family_ = Family::v4;
};

/// An address plus a port (host byte order).
class Endpoint
{
public:
    constexpr Endpoint() noexcept = default;
    constexpr Endpoint(const IpAddress& address, std::uint16_t port) noexcept : address_(address), port_(port) {}

    /// Parses "a.b.c.d:port" or "[v6]:port".
    static std::optional<Endpoint> parse(std::string_view text) noexcept;

    constexpr const IpAddress& address() const noexcept { return address_; }
    constexpr std::uint16_t port() const noexcept { return port_; }

    std::string to_string() const;

    friend bool operator==(const Endpoint& a, const Endpoint& b) noexcept
    {
        return a.port_ == b.port_ && a.address_ == b.address_;
    }
    friend bool operator!=(const Endpoint& a, const Endpoint& b) noexcept { return !(a == b); }
    friend bool operator<(const Endpoint& a, const Endpoint& b) noexcept
    {
        return a.address_ != b.address_ ? a.address_ < b.address_ : a.port_ < b.port_;
    }

private:
    IpAddress address_;
    std::uint16_t port_ = 0;
};

} // namespace pzynet

template <>
struct std::hash<pzynet::IpAddress>
{
    std::size_t operator()(const pzynet::IpAddress& ip) const noexcept;
};

template <>
struct std::hash<pzynet::Endpoint>
{
    std::size_t operator()(const pzynet::Endpoint& ep) const noexcept;
};
