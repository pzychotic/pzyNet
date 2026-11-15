#include "pzynet/socket/poller.hpp"
#include "pzynet/socket/tcp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <vector>

using namespace pzynet;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

namespace
{
const Endpoint loopback_any(IpAddress::loopback_v4(), 0);

Bytes read_exactly(TcpStream& stream, std::size_t count)
{
    Bytes data(count);
    std::size_t have = 0;
    while (have < count)
    {
        const std::size_t n = stream.receive(MutableByteView(data.data() + have, count - have));
        REQUIRE(n > 0);
        have += n;
    }
    return data;
}
} // namespace

TEST_CASE("TCP echo over loopback", "[socket][tcp]")
{
    TcpListener listener;
    listener.listen(loopback_any);
    const Endpoint server_address = listener.local_endpoint();

    TcpStream client;
    client.connect(server_address);
    Endpoint peer;
    TcpStream server = listener.accept(peer);
    REQUIRE(server.is_open());
    CHECK(peer == client.local_endpoint());
    CHECK(client.remote_endpoint() == server_address);
    CHECK_NOTHROW(client.set_no_delay(true));
    CHECK_NOTHROW(client.set_keep_alive(true));

    const Bytes request{'p', 'i', 'n', 'g'};
    client.send_all(request);
    CHECK(read_exactly(server, request.size()) == request);
    server.send_all(request);
    CHECK(read_exactly(client, request.size()) == request);

    // Closing the sending side is seen by the peer as a zero-length read.
    client.shutdown(Shutdown::send);
    std::array<std::uint8_t, 8> buffer{};
    std::error_code ec;
    CHECK(server.receive(buffer, ec) == 0);
    CHECK_FALSE(ec);
}

TEST_CASE("connecting to a closed port fails", "[socket][tcp]")
{
    // Grab a free port, then close the listener so nothing is listening there.
    TcpListener listener;
    listener.listen(loopback_any);
    const Endpoint address = listener.local_endpoint();
    listener.close();

    TcpStream client;
    std::error_code ec;
    client.connect(address, ec);
    CHECK(ec);
    CHECK_THROWS_AS(TcpStream().connect(address), std::system_error);
}

TEST_CASE("non-blocking accept and connect integrate with the poller", "[socket][tcp][poller]")
{
    TcpListener listener;
    listener.listen(loopback_any);
    listener.set_non_blocking(true);

    std::error_code ec;
    TcpStream none = listener.accept(ec);
    CHECK(ec == std::errc::operation_would_block);
    CHECK_FALSE(none.is_open());

    TcpStream client;
    client.open(IpAddress::Family::v4);
    client.set_non_blocking(true);
    client.connect(listener.local_endpoint(), ec);
    if (ec) // loopback connects may complete immediately
        REQUIRE(ec == std::errc::operation_in_progress);

    Poller poller;
    poller.add(listener, PollEvents::readable);
    poller.add(client, PollEvents::writable);

    bool accepted = false, connected = false;
    TcpStream server;
    std::vector<PollEvent> events;
    for (int i = 0; i < 50 && !(accepted && connected); ++i)
    {
        poller.wait(events, 100ms);
        for (const PollEvent& event : events)
        {
            if (event.socket == listener.native_handle() && has(event.events, PollEvents::readable))
            {
                server = listener.accept();
                accepted = server.is_open();
                poller.modify(listener, PollEvents::none);
            }
            if (event.socket == client.native_handle() && has(event.events, PollEvents::writable))
            {
                client.finish_connect();
                connected = true;
                poller.modify(client, PollEvents::readable);
            }
        }
    }
    REQUIRE(accepted);
    REQUIRE(connected);

    // Nothing to read yet: the non-blocking client reports would-block and the poller stays quiet.
    std::array<std::uint8_t, 8> buffer{};
    CHECK(client.receive(buffer, ec) == 0);
    CHECK(ec == std::errc::operation_would_block);
    CHECK(poller.wait(events, 0ms) == 0);

    server.send_all(Bytes{7});
    REQUIRE(poller.wait(events, 2000ms) == 1);
    CHECK(events[0].socket == client.native_handle());
    CHECK(has(events[0].events, PollEvents::readable));
    CHECK(client.receive(buffer) == 1);
    CHECK(buffer[0] == 7);

    poller.remove(client);
    poller.remove(listener);
}
