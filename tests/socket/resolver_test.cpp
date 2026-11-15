#include "pzynet/socket/resolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace pzynet;

TEST_CASE("address literals resolve to themselves", "[socket][resolver]")
{
    const auto v4 = resolve("127.0.0.1", 8080);
    REQUIRE(v4.size() == 1);
    CHECK(v4[0] == Endpoint(IpAddress::loopback_v4(), 8080));

    std::error_code ec;
    const auto v6 = resolve("::1", 53, ResolveFamily::v6, ec);
    if (!ec)
    {
        REQUIRE(v6.size() == 1);
        CHECK(v6[0] == Endpoint(IpAddress::loopback_v6(), 53));
    }
}

TEST_CASE("localhost resolves to a loopback address", "[socket][resolver]")
{
    const auto endpoints = resolve("localhost", 1234, ResolveFamily::v4);
    REQUIRE_FALSE(endpoints.empty());
    CHECK(std::all_of(endpoints.begin(), endpoints.end(), [](const Endpoint& ep) {
        return ep.address().is_v4() && ep.address().is_loopback() && ep.port() == 1234;
    }));
}

TEST_CASE("an unknown host reports host_not_found", "[socket][resolver]")
{
    // ".invalid" is reserved (RFC 2606) and never resolves.
    std::error_code ec;
    const auto endpoints = resolve("no-such-host.invalid", 80, ResolveFamily::any, ec);
    CHECK(endpoints.empty());
    CHECK(ec == make_error_code(Errc::host_not_found));
    CHECK_THROWS_AS(resolve("no-such-host.invalid", 80), std::system_error);
}
