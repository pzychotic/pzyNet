#include "pzynet/socket/poller.hpp"
#include "pzynet/socket/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace pzynet;
using namespace std::chrono_literals;

namespace
{
const Endpoint loopback_any(IpAddress::loopback_v4(), 0);
}

TEST_CASE("the poller reports a readable UDP socket", "[socket][poller]")
{
    UdpSocket receiver, sender;
    receiver.bind(loopback_any);
    sender.bind(loopback_any);

    Poller poller;
    poller.add(receiver, PollEvents::readable);
    std::vector<PollEvent> events;

    CHECK(poller.wait(events, 0ms) == 0);

    sender.send_to(std::vector<std::uint8_t>{1}, receiver.local_endpoint());
    REQUIRE(poller.wait(events, 2000ms) == 1);
    CHECK(events[0].socket == receiver.native_handle());
    CHECK(has(events[0].events, PollEvents::readable));
    CHECK_FALSE(has(events[0].events, PollEvents::writable));
}

TEST_CASE("the poller times out when nothing happens", "[socket][poller]")
{
    UdpSocket socket;
    socket.bind(loopback_any);
    Poller poller;
    poller.add(socket, PollEvents::readable);

    std::vector<PollEvent> events;
    const auto start = std::chrono::steady_clock::now();
    CHECK(poller.wait(events, 50ms) == 0);
    CHECK(std::chrono::steady_clock::now() - start >= 40ms);
}

TEST_CASE("wake() interrupts a blocking wait from another thread", "[socket][poller]")
{
    Poller poller;
    std::vector<PollEvent> events;

    std::thread waker([&poller] {
        std::this_thread::sleep_for(50ms);
        poller.wake();
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(poller.wait(events, 10000ms) == 0);
    CHECK(std::chrono::steady_clock::now() - start < 5000ms);
    waker.join();

    // A wake-up is consumed by the wait it interrupts.
    CHECK(poller.wait(events, 0ms) == 0);
}

TEST_CASE("poller registration errors are reported", "[socket][poller]")
{
    UdpSocket socket;
    socket.bind(loopback_any);
    Poller poller;

    std::error_code ec;
    poller.modify(socket, PollEvents::readable, ec);
    CHECK(ec); // not registered yet
    poller.add(socket, PollEvents::readable, ec);
    CHECK_FALSE(ec);
    poller.add(socket, PollEvents::readable, ec);
    CHECK(ec); // registered twice
    poller.remove(socket, ec);
    CHECK_FALSE(ec);
    CHECK_THROWS_AS(poller.remove(socket), std::system_error);
}
