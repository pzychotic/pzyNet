#pragma once

// An in-memory network for driving RakNetEngine instances deterministically: a fake clock, a
// configurable link (latency, jitter, loss, duplication, size limit) and no sockets at all.

#include "pzynet/raknet/raknet_engine.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <vector>

namespace test
{

using Bytes = std::vector<std::uint8_t>;
using namespace std::chrono_literals;

struct Peer
{
    Peer(pzynet::raknet::Config config, const pzynet::Endpoint& address_) : engine(std::move(config)), address(address_)
    {
    }

    pzynet::raknet::RakNetEngine engine;
    pzynet::Endpoint address;
    std::vector<pzynet::transport::Event> events; // everything the engine reported, in order

    template <class T>
    std::vector<T> all() const
    {
        std::vector<T> found;
        for (const auto& event : events)
            if (const T* typed = std::get_if<T>(&event))
                found.push_back(*typed);
        return found;
    }
    template <class T>
    std::size_t count() const
    {
        return all<T>().size();
    }
    std::vector<Bytes> messages() const
    {
        std::vector<Bytes> found;
        for (const auto& message : all<pzynet::transport::Message>())
            found.push_back(message.data);
        return found;
    }
};

class Network
{
public:
    struct Datagram
    {
        pzynet::Endpoint from, to;
        Bytes data;
        bool dont_fragment = false;
    };

    // link properties
    std::chrono::microseconds latency{5000};
    std::chrono::microseconds jitter{0}; // uniform extra delay; non-zero reorders datagrams
    double loss = 0.0;
    double duplication = 0.0;
    std::size_t path_mtu = 65535; // IP datagram size the link carries (payload + 28)

    /// Sees every datagram put on the wire, before the link properties apply.
    std::function<void(const Datagram&)> tap;

    pzynet::TimePoint now = pzynet::TimePoint() + std::chrono::hours(1);
    std::size_t datagrams_sent = 0;

    Peer& add(pzynet::raknet::Config config = {})
    {
        const auto index = static_cast<std::uint8_t>(peers_.size() + 1);
        if (config.guid == 0)
            config.guid = 0x1000u + index; // deterministic
        peers_.push_back(std::make_unique<Peer>(std::move(config),
                                                pzynet::Endpoint(pzynet::IpAddress::v4(10, 0, 0, index), 60000)));
        return *peers_.back();
    }

    /// Puts a hand-made datagram on the wire as if `from` had sent it.
    void inject(const pzynet::Endpoint& from, const pzynet::Endpoint& to, Bytes data)
    {
        in_flight_.emplace(now + latency, Datagram{from, to, std::move(data), false});
    }

    void step(std::chrono::microseconds dt = 1ms)
    {
        now += dt;

        while (!in_flight_.empty() && in_flight_.begin()->first <= now)
        {
            const Datagram datagram = std::move(in_flight_.begin()->second);
            in_flight_.erase(in_flight_.begin());
            for (auto& peer : peers_)
                if (peer->address == datagram.to)
                    peer->engine.handle_datagram(datagram.from, datagram.data, now);
        }

        for (auto& peer : peers_)
        {
            peer->engine.advance(now);

            pzynet::transport::Transmit transmit;
            while (peer->engine.poll_transmit(transmit))
            {
                Datagram datagram{peer->address, transmit.to, std::move(transmit.data), transmit.dont_fragment};
                ++datagrams_sent;
                if (tap)
                    tap(datagram);
                if (datagram.data.size() + 28 > path_mtu)
                    continue; // too big for the path (RakNet's probes never rely on fragmentation)
                if (chance(loss))
                    continue;
                const int copies = chance(duplication) ? 2 : 1;
                for (int i = 0; i < copies; ++i)
                    in_flight_.emplace(now + latency + random_jitter(), datagram);
            }

            pzynet::transport::Event event;
            while (peer->engine.poll_event(event))
                peer->events.push_back(std::move(event));
        }
    }

    void run(std::chrono::microseconds duration, std::chrono::microseconds dt = 1ms)
    {
        const pzynet::TimePoint end = now + duration;
        while (now < end)
            step(dt);
    }

    /// Steps until `done()` holds; false if `limit` of simulated time passes first.
    template <class Predicate>
    bool run_until(Predicate done, std::chrono::microseconds limit = 30s, std::chrono::microseconds dt = 1ms)
    {
        const pzynet::TimePoint end = now + limit;
        while (!done())
        {
            if (now >= end)
                return false;
            step(dt);
        }
        return true;
    }

private:
    bool chance(double probability)
    {
        return probability > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(random_) < probability;
    }
    std::chrono::microseconds random_jitter()
    {
        if (jitter.count() <= 0)
            return std::chrono::microseconds(0);
        return std::chrono::microseconds(std::uniform_int_distribution<std::int64_t>(0, jitter.count())(random_));
    }

    std::vector<std::unique_ptr<Peer>> peers_;
    std::multimap<pzynet::TimePoint, Datagram> in_flight_;
    std::mt19937 random_{20261115};
};

} // namespace test
