#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace pzynet;
using Bytes = std::vector<std::uint8_t>;

namespace
{
const Endpoint loopback_any(IpAddress::loopback_v4(), 0);
}

TEST_CASE("UDP datagrams travel over loopback", "[socket][udp]")
{
    UdpSocket a, b;
    a.bind(loopback_any);
    b.bind(loopback_any);
    const Endpoint a_address = a.local_endpoint();
    const Endpoint b_address = b.local_endpoint();
    REQUIRE(a_address.port() != 0);
    REQUIRE(a_address.port() != b_address.port());

    const Bytes message{1, 2, 3, 4, 5};
    CHECK(a.send_to(message, b_address) == message.size());

    std::array<std::uint8_t, 64> buffer{};
    Endpoint from;
    const std::size_t size = b.receive_from(buffer, from);
    CHECK(Bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)) == message);
    CHECK(from == a_address);
}

TEST_CASE("a non-blocking UDP receive reports would-block", "[socket][udp]")
{
    UdpSocket socket;
    socket.bind(loopback_any);
    socket.set_non_blocking(true);

    std::array<std::uint8_t, 16> buffer{};
    Endpoint from;
    std::error_code ec;
    CHECK(socket.receive_from(buffer, from, ec) == 0);
    CHECK(ec == std::errc::operation_would_block);
    CHECK_THROWS_AS(socket.receive_from(buffer, from), std::system_error);
}

TEST_CASE("operations on a closed UDP socket fail cleanly", "[socket][udp]")
{
    UdpSocket socket;
    std::error_code ec;
    socket.send_to(Bytes{1}, Endpoint(IpAddress::loopback_v4(), 9), ec);
    CHECK(ec == make_error_code(Errc::not_open));
    socket.set_broadcast(true, ec);
    CHECK(ec == make_error_code(Errc::not_open));
    CHECK_THROWS_AS(socket.local_endpoint(), std::system_error);
}

TEST_CASE("an oversized datagram is truncated to the buffer", "[socket][udp]")
{
    UdpSocket a, b;
    a.bind(loopback_any);
    b.bind(loopback_any);
    a.send_to(Bytes(100, 0x55), b.local_endpoint());

    std::array<std::uint8_t, 10> buffer{};
    Endpoint from;
    std::error_code ec;
    CHECK(b.receive_from(buffer, from, ec) == buffer.size());
    CHECK_FALSE(ec);
    CHECK(buffer[9] == 0x55);
}

TEST_CASE("UDP socket options can be set", "[socket][udp]")
{
    UdpSocket socket;
    socket.open(IpAddress::Family::v4);
    CHECK_NOTHROW(socket.set_broadcast(true));
    CHECK_NOTHROW(socket.set_dont_fragment(true));
    CHECK_NOTHROW(socket.set_dont_fragment(false));
    CHECK_NOTHROW(socket.set_receive_buffer_size(128 * 1024));
    CHECK_NOTHROW(socket.set_send_buffer_size(64 * 1024));
    CHECK_NOTHROW(socket.set_multicast_ttl(2));
    CHECK_NOTHROW(socket.set_multicast_loopback(true));

    std::error_code ec;
    socket.join_multicast_group(IpAddress::v4(10, 0, 0, 1), IpAddress(), ec); // not a multicast address
    CHECK(ec == make_error_code(Errc::invalid_argument));
    socket.set_v6_only(false, ec); // not an IPv6 socket
    CHECK(ec == make_error_code(Errc::address_family_mismatch));
}

TEST_CASE("a moved-from socket is closed and the target owns the handle", "[socket][udp]")
{
    UdpSocket a;
    a.bind(loopback_any);
    const NativeSocket handle = a.native_handle();
    UdpSocket b = std::move(a);
    CHECK_FALSE(a.is_open());
    CHECK(b.is_open());
    CHECK(b.native_handle() == handle);
}

TEST_CASE("a dual-stack IPv6 UDP socket also carries IPv4", "[socket][udp][ipv6]")
{
    UdpSocket dual;
    std::error_code ec;
    dual.open(IpAddress::Family::v6, ec);
    if (ec)
        SKIP("IPv6 is not available on this machine");
    dual.set_v6_only(false);
    dual.bind(Endpoint(IpAddress::any_v6(), 0), ec);
    if (ec)
        SKIP("cannot bind an IPv6 socket on this machine");
    const std::uint16_t port = dual.local_endpoint().port();

    UdpSocket v4;
    v4.bind(loopback_any);
    v4.send_to(Bytes{42}, Endpoint(IpAddress::loopback_v4(), port));

    std::array<std::uint8_t, 8> buffer{};
    Endpoint from;
    REQUIRE(dual.receive_from(buffer, from) == 1);
    CHECK(buffer[0] == 42);
    CHECK(from == v4.local_endpoint()); // reported as plain IPv4, not ::ffff:127.0.0.1

    // ...and replies to the IPv4 endpoint without the caller mapping it.
    dual.send_to(Bytes{43}, from);
    REQUIRE(v4.receive_from(buffer, from) == 1);
    CHECK(buffer[0] == 43);
}
