#include "pzynet/core/address.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

using pzynet::Endpoint;
using pzynet::IpAddress;

TEST_CASE("IPv4 text round-trips", "[core][address]")
{
    const auto ip = IpAddress::parse("192.168.1.42");
    REQUIRE(ip);
    CHECK(ip->is_v4());
    CHECK(ip->v4_bytes() == std::array<std::uint8_t, 4>{192, 168, 1, 42});
    CHECK(ip->to_string() == "192.168.1.42");
    CHECK(*ip == IpAddress::v4(192, 168, 1, 42));
}

TEST_CASE("malformed IPv4 text is rejected", "[core][address]")
{
    for (const char* text : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1..2.3", "1.2.3.4 ", "a.b.c.d", "01.2.3.4",
                             "1.2.3.-4"})
    {
        INFO(text);
        CHECK_FALSE(IpAddress::parse(text));
    }
}

TEST_CASE("IPv6 text parses and is printed in canonical form", "[core][address]")
{
    struct Case
    {
        const char* input;
        const char* canonical;
    };
    for (const Case& c : {Case{"::", "::"}, Case{"::1", "::1"}, Case{"2001:db8::1", "2001:db8::1"},
                          Case{"2001:0DB8:0000:0000:0000:0000:0000:0001", "2001:db8::1"},
                          Case{"fe80::1%7", "fe80::1%7"}, Case{"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
                          Case{"1::", "1::"}, Case{"1:0:0:2:0:0:0:3", "1:0:0:2::3"},
                          Case{"::ffff:10.1.2.3", "::ffff:10.1.2.3"}, Case{"1:0:2:3:4:5:6:7", "1:0:2:3:4:5:6:7"}})
    {
        INFO(c.input);
        const auto ip = IpAddress::parse(c.input);
        REQUIRE(ip);
        CHECK(ip->is_v6());
        CHECK(ip->to_string() == c.canonical);
        CHECK(IpAddress::parse(ip->to_string()) == ip);
    }
}

TEST_CASE("malformed IPv6 text is rejected", "[core][address]")
{
    for (const char* text : {":", ":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1::2::3", "12345::", "g::1",
                             "1:2:3:4:5:6:7:", ":1:2:3:4:5:6:7", "::1.2.3", "1.2.3.4::", "fe80::1%eth0",
                             "1:2:3:4:5:6:7::8"})
    {
        INFO(text);
        CHECK_FALSE(IpAddress::parse(text));
    }
}

TEST_CASE("address classification", "[core][address]")
{
    CHECK(IpAddress::any_v4().is_unspecified());
    CHECK(IpAddress::any_v6().is_unspecified());
    CHECK(IpAddress::loopback_v4().is_loopback());
    CHECK(IpAddress::loopback_v6().is_loopback());
    CHECK(IpAddress::v4(239, 1, 2, 3).is_multicast());
    CHECK(IpAddress::parse("ff02::1")->is_multicast());
    CHECK_FALSE(IpAddress::v4(10, 0, 0, 1).is_multicast());

    const IpAddress v4 = IpAddress::v4(10, 1, 2, 3);
    const IpAddress mapped = v4.to_v4_mapped();
    CHECK(mapped.is_v4_mapped());
    CHECK(mapped.unmapped() == v4);
    CHECK(v4.unmapped() == v4);
    CHECK(v4 != mapped);
}

TEST_CASE("endpoint text round-trips", "[core][address]")
{
    const auto v4 = Endpoint::parse("127.0.0.1:60000");
    REQUIRE(v4);
    CHECK(v4->address() == IpAddress::loopback_v4());
    CHECK(v4->port() == 60000);
    CHECK(v4->to_string() == "127.0.0.1:60000");

    const auto v6 = Endpoint::parse("[::1]:80");
    REQUIRE(v6);
    CHECK(v6->address() == IpAddress::loopback_v6());
    CHECK(v6->to_string() == "[::1]:80");

    for (const char* text : {"127.0.0.1", "127.0.0.1:", "127.0.0.1:65536", "::1:80", "[::1]", "[::1]80",
                             "[127.0.0.1]:80", ":80"})
    {
        INFO(text);
        CHECK_FALSE(Endpoint::parse(text));
    }
}

TEST_CASE("endpoints work as hash keys", "[core][address]")
{
    std::unordered_set<Endpoint> set;
    set.insert(Endpoint(IpAddress::loopback_v4(), 1));
    set.insert(Endpoint(IpAddress::loopback_v4(), 2));
    set.insert(Endpoint(IpAddress::loopback_v4(), 1));
    set.insert(Endpoint(IpAddress::loopback_v6(), 1));
    CHECK(set.size() == 3);
    CHECK(set.count(Endpoint(IpAddress::loopback_v4(), 2)) == 1);
}
