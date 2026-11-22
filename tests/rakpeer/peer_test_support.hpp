// Helpers shared by the Peer tests: loopback Peers and a Network that drives them in either threading mode.

#pragma once

#include "pzynet/rakpeer/peer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace peer_test
{

using namespace pzynet;
using namespace pzynet::raknet;
using namespace std::chrono_literals;

using SteadyClock = std::chrono::steady_clock;

inline PeerConfig loopback_config(Threading threading, std::size_t max_incoming = 0)
{
    PeerConfig config;
    config.bind = Endpoint(IpAddress::loopback_v4(), 0);
    config.engine.max_incoming_connections = max_incoming;
    config.threading = threading;
    return config;
}

/// Peers under test. Waiting runs the network of every app-driven Peer that is not paused, so tests
/// read the same in both threading modes.
class Network
{
public:
    Peer& add(PeerConfig config)
    {
        peers_.push_back(std::make_unique<Peer>());
        peers_.back()->start(std::move(config));
        return *peers_.back();
    }

    void destroy(Peer& peer)
    {
        peers_.erase(std::find_if(peers_.begin(), peers_.end(), [&](const auto& p) { return p.get() == &peer; }));
    }

    /// Stops running the network of an app-driven Peer, so it falls silent.
    void pause(Peer& peer) { paused_.push_back(&peer); }

    void pump()
    {
        bool pumped = false;
        for (const auto& peer : peers_)
        {
            if (!peer->running() || std::find(paused_.begin(), paused_.end(), peer.get()) != paused_.end())
                continue;
            std::error_code ec;
            peer->update(1ms, ec); // Errc::running for background Peers
            pumped = pumped || !ec;
        }
        if (!pumped)
            std::this_thread::sleep_for(1ms);
    }

    /// The next Packet, or nullopt if none arrives within `limit`.
    std::optional<Packet> wait_for_packet(Peer& peer, std::chrono::milliseconds limit = 5s)
    {
        const auto give_up = SteadyClock::now() + limit;
        for (;;)
        {
            if (std::optional<Packet> packet = peer.receive())
                return packet;
            if (SteadyClock::now() >= give_up)
                return std::nullopt;
            pump();
        }
    }

    /// Adds a server and a client Peer and connects them; the connection Packets are consumed.
    std::pair<Peer&, Peer&> connected_pair(PeerConfig server_config, PeerConfig client_config)
    {
        Peer& server = add(std::move(server_config));
        Peer& client = add(std::move(client_config));
        client.connect(server.local_endpoint());
        REQUIRE(wait_for_packet(client));
        REQUIRE(wait_for_packet(server));
        return {server, client};
    }

    std::pair<Peer&, Peer&> connected_pair(Threading threading)
    {
        return connected_pair(loopback_config(threading, 4), loopback_config(threading));
    }

private:
    std::vector<std::unique_ptr<Peer>> peers_;
    std::vector<Peer*> paused_;
};

} // namespace peer_test
