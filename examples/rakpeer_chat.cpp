// The chat of raknet_chat.cpp, built on the Peer and the BitStream.
//   rakpeer_chat server [port]              (default port 60000)
//   rakpeer_chat client <host> [port]
//
// Every chat line travels as one reliable ordered message: ID_USER_PACKET_ENUM followed by the text,
// one byte per character. The server relays each line to all other clients. Either side talks to
// raknet_chat, or to a RakNet 4.x application that uses the same message layout.

#include "pzynet/raknet/bit_stream.hpp"
#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/rakpeer/peer.hpp"
#include "pzynet/socket/resolver.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

using namespace pzynet;
using namespace pzynet::raknet;

namespace
{

BitStreamWriter chat_message(const std::string& text)
{
    BitStreamWriter message;
    message.write(id::user_packet_enum);
    for (const char c : text)
        message.write(static_cast<std::uint8_t>(c));
    return message;
}

std::string chat_text(const Packet& packet)
{
    std::string text;
    BitStreamReader reader = packet.reader();
    while (reader.bits_remaining() >= 8)
        text.push_back(static_cast<char>(reader.read<std::uint8_t>()));
    return text;
}

int run_server(std::uint16_t port)
{
    PeerConfig config;
    config.bind = Endpoint(IpAddress::any_v4(), port);
    config.engine.max_incoming_connections = 32;
    config.engine.offline_ping_response = {'p', 'z', 'y', 'N', 'e', 't', ' ', 'c', 'h', 'a', 't'};
    Peer peer;
    peer.start(config); // the network runs on a background thread
    std::printf("chat server listening on %s\n", peer.local_endpoint().to_string().c_str());

    std::set<Guid> clients;
    for (;;)
    {
        std::optional<Packet> packet = peer.receive();
        if (!packet)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        switch (packet->id)
        {
        case id::new_incoming_connection:
            clients.insert(packet->guid);
            std::printf("+ %s\n", packet->address.to_string().c_str());
            break;
        case id::disconnection_notification:
        case id::connection_lost:
            clients.erase(packet->guid);
            std::printf("- %s\n", packet->address.to_string().c_str());
            break;
        case id::user_packet_enum:
            std::printf("%s\n", chat_text(*packet).c_str());
            for (const Guid client : clients)
                if (client != packet->guid)
                    peer.send(client, packet->data);
            break;
        default:
            break;
        }
    }
}

int run_client(const std::string& server_name, std::uint16_t port)
{
    const Endpoint server = resolve(server_name, port, ResolveFamily::v4).front();

    Peer peer;
    peer.start();
    std::printf("connecting to %s ...\n", server.to_string().c_str());
    peer.connect(server);

    // Console input arrives on its own thread, since reading a line blocks. The thread is detached
    // (it may sit in getline forever), so the state it shares with this one is reference-counted.
    struct Input
    {
        std::mutex mutex;
        std::deque<std::string> lines;
        bool closed = false;
    };
    const auto input = std::make_shared<Input>();
    std::thread([input] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            std::lock_guard<std::mutex> lock(input->mutex);
            input->lines.push_back(line);
        }
        std::lock_guard<std::mutex> lock(input->mutex);
        input->closed = true;
    }).detach();

    bool connected = false;
    bool leaving = false;
    for (;;)
    {
        while (std::optional<Packet> packet = peer.receive())
        {
            switch (packet->id)
            {
            case id::connection_request_accepted:
                connected = true;
                std::printf("connected; type to chat, end of input quits\n");
                break;
            case id::disconnection_notification:
            case id::connection_lost:
                std::printf("disconnected\n");
                return 0;
            case id::user_packet_enum: std::printf("> %s\n", chat_text(*packet).c_str()); break;
            default:
                if (!connected) // one of RakNet's connect-failure ids
                {
                    std::printf("could not connect (message id %d)\n", packet->id);
                    return 1;
                }
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(input->mutex);
            while (connected && !input->lines.empty())
            {
                peer.send(server, chat_message(input->lines.front()));
                input->lines.pop_front();
            }
            if (connected && input->closed && input->lines.empty() && !leaving)
            {
                // Graceful: queued lines are delivered first. The server may have gone meanwhile.
                std::error_code ignored;
                peer.disconnect(server, pzynet::transport::DisconnectMode::graceful, ignored);
                leaving = true;
            }
        }
        if (leaving)
        {
            // Closing locally produces no Packet; give the notification a moment to go out.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && std::strcmp(argv[1], "server") == 0)
            return run_server(static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 60000));
        if (argc >= 3 && std::strcmp(argv[1], "client") == 0)
            return run_client(argv[2], static_cast<std::uint16_t>(argc > 3 ? std::atoi(argv[3]) : 60000));
        std::fprintf(stderr, "usage: %s server [port]\n       %s client <host> [port]\n", argv[0], argv[0]);
        return 2;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
