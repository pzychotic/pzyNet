#include "pzynet/transport/udp_host.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <vector>

using namespace pzynet;
using namespace pzynet::transport;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

namespace
{

// The simplest possible protocol: no handshake, every datagram is one message, and the "connection"
// is just the peer's address. Enough to exercise the driver without any real protocol behind it.
class DatagramEngine final : public Engine
{
public:
    bool echo = false;
    int advance_calls = 0;

    void handle_datagram(const Endpoint& from, ByteView data, TimePoint) override
    {
        if (echo)
            transmits_.push_back(Transmit{from, Bytes(data.begin(), data.end()), false});
        Message message;
        message.connection = id_for(from);
        message.data.assign(data.begin(), data.end());
        events_.push_back(std::move(message));
    }
    void advance(TimePoint) override { ++advance_calls; }
    bool poll_transmit(Transmit& out) override { return pop(transmits_, out); }
    bool poll_event(Event& out) override { return pop(events_, out); }
    std::optional<TimePoint> next_deadline() const override { return std::nullopt; }

    ConnectionId connect(const Endpoint& to, ByteView, std::error_code& ec) override
    {
        ec.clear();
        const ConnectionId id = id_for(to);
        events_.push_back(Connected{id, to, false});
        return id;
    }
    void disconnect(ConnectionId connection, DisconnectMode) override
    {
        events_.push_back(Disconnected{connection, peers_.at(connection - 1), DisconnectReason::closed_locally});
    }
    void send(ConnectionId connection, ByteView data, const SendOptions&, std::error_code& ec) override
    {
        if (connection == 0 || connection > peers_.size())
        {
            ec = make_error_code(Errc::unknown_connection);
            return;
        }
        ec.clear();
        transmits_.push_back(Transmit{peers_[connection - 1], Bytes(data.begin(), data.end()), false});
    }

private:
    template <class T>
    static bool pop(std::deque<T>& queue, T& out)
    {
        if (queue.empty())
            return false;
        out = std::move(queue.front());
        queue.pop_front();
        return true;
    }
    ConnectionId id_for(const Endpoint& peer)
    {
        for (std::size_t i = 0; i < peers_.size(); ++i)
            if (peers_[i] == peer)
                return i + 1;
        peers_.push_back(peer);
        return peers_.size();
    }

    std::vector<Endpoint> peers_;
    std::deque<Transmit> transmits_;
    std::deque<Event> events_;
};

const Endpoint loopback_any(IpAddress::loopback_v4(), 0);

// Pumps both hosts until `client` produces an event of type T.
template <class T>
T pump_until(UdpHost& client, UdpHost& server)
{
    std::optional<T> result;
    const auto give_up = std::chrono::steady_clock::now() + 5s;
    while (!result && std::chrono::steady_clock::now() < give_up)
    {
        if (!server.threaded())
            server.update(1ms);
        Event event;
        if (client.wait_event(event, 5ms))
            if (T* wanted = std::get_if<T>(&event))
                result = std::move(*wanted);
    }
    REQUIRE(result);
    return std::move(*result);
}

} // namespace

TEST_CASE("an app-driven host moves datagrams between engine and socket", "[transport][udp_host]")
{
    DatagramEngine server_engine, client_engine;
    server_engine.echo = true;
    UdpHost server(server_engine), client(client_engine);
    server.bind(loopback_any);
    client.bind(loopback_any);

    const ConnectionId id = client.connect(server.local_endpoint());
    Event event;
    REQUIRE(client.poll_event(event));
    CHECK(std::get<Connected>(event).connection == id);

    client.send(id, Bytes{1, 2, 3});
    const Message reply = pump_until<Message>(client, server);
    CHECK(reply.connection == id);
    CHECK(reply.data == Bytes{1, 2, 3});
    CHECK(server_engine.advance_calls > 0);

    // The server saw the message too.
    REQUIRE(server.poll_event(event));
    CHECK(std::get<Message>(event).data == Bytes{1, 2, 3});
}

TEST_CASE("a threaded host runs the engine in the background", "[transport][udp_host]")
{
    DatagramEngine server_engine, client_engine;
    server_engine.echo = true;
    UdpHost server(server_engine), client(client_engine);
    server.bind(loopback_any);
    client.bind(loopback_any);
    server.start_thread();
    client.start_thread();
    REQUIRE(server.threaded());

    std::error_code ec;
    server.update(0ms, ec);
    CHECK(ec == make_error_code(Errc::running));

    const ConnectionId id = client.connect(server.local_endpoint());
    Event event;
    REQUIRE(client.wait_event(event, 5000ms));
    REQUIRE(std::holds_alternative<Connected>(event));

    for (std::uint8_t i = 0; i < 20; ++i)
    {
        client.send(id, Bytes{i});
        REQUIRE(client.wait_event(event, 5000ms));
        CHECK(std::get<Message>(event).data == Bytes{i});
    }

    // Protocol-specific access goes through with_engine(), under the host's lock.
    const int calls = server.with_engine([](Engine& engine) {
        return static_cast<DatagramEngine&>(engine).advance_calls;
    });
    CHECK(calls > 0);

    client.stop();
    server.stop();
    CHECK_FALSE(client.threaded());
    client.stop(); // idempotent
}

TEST_CASE("host errors are reported through both error styles", "[transport][udp_host]")
{
    DatagramEngine engine;
    UdpHost host(engine);

    std::error_code ec;
    host.update(0ms, ec);
    CHECK(ec == make_error_code(Errc::not_open));
    CHECK_THROWS_AS(host.start_thread(), std::system_error);

    host.bind(loopback_any);
    host.bind(loopback_any, ec);
    CHECK(ec == make_error_code(Errc::already_open));

    host.send(99, Bytes{1}, SendOptions(), ec);
    CHECK(ec == make_error_code(Errc::unknown_connection));
    CHECK_THROWS_AS(host.send(99, Bytes{1}), std::system_error);

    Event event;
    CHECK_FALSE(host.wait_event(event, 20ms));
}
