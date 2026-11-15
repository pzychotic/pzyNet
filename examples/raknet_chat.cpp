// Minimal chat over the RakNet-compatible transport.
//   raknet_chat server [port]              (default port 60000)
//   raknet_chat client <host> [port]
//
// Every chat line travels as one reliable ordered message: ID_USER_PACKET_ENUM followed by the text.
// The server relays each line to all other clients. Because the wire protocol is RakNet's, either
// side can be replaced by a RakNet 4.x application that uses the same message layout.

#include "pzynet/raknet/message_ids.hpp"
#include "pzynet/raknet/raknet_engine.hpp"
#include "pzynet/socket/resolver.hpp"
#include "pzynet/transport/udp_host.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using namespace pzynet;
using namespace pzynet::transport;

namespace
{

std::vector<std::uint8_t> chat_message(const std::string& text)
{
    std::vector<std::uint8_t> message{raknet::id::user_packet_enum};
    message.insert(message.end(), text.begin(), text.end());
    return message;
}

std::string chat_text(const Message& message)
{
    return std::string(message.data.begin() + 1, message.data.end());
}

int run_server(std::uint16_t port)
{
    raknet::Config config;
    config.max_incoming_connections = 32;
    config.offline_ping_response = {'p', 'z', 'y', 'N', 'e', 't', ' ', 'c', 'h', 'a', 't'};
    raknet::RakNetEngine engine(config);
    UdpHost host(engine);
    host.bind(Endpoint(IpAddress::any_v4(), port));
    std::printf("chat server listening on %s\n", host.local_endpoint().to_string().c_str());

    std::set<ConnectionId> clients;
    for (;;)
    {
        Event event;
        if (!host.wait_event(event, std::chrono::milliseconds(1000))) // app-driven: this pumps the network
            continue;

        if (const auto* connected = std::get_if<Connected>(&event))
        {
            clients.insert(connected->connection);
            std::printf("+ %s\n", connected->remote.to_string().c_str());
        }
        else if (const auto* disconnected = std::get_if<Disconnected>(&event))
        {
            clients.erase(disconnected->connection);
            std::printf("- %s\n", disconnected->remote.to_string().c_str());
        }
        else if (const auto* message = std::get_if<Message>(&event))
        {
            if (message->data.empty() || message->data[0] != raknet::id::user_packet_enum)
                continue;
            std::printf("%s\n", chat_text(*message).c_str());
            for (ConnectionId client : clients)
                if (client != message->connection)
                    host.send(client, message->data);
        }
    }
}

int run_client(const std::string& server_name, std::uint16_t port)
{
    const Endpoint server = resolve(server_name, port, ResolveFamily::v4).front();

    raknet::RakNetEngine engine;
    UdpHost host(engine);
    host.bind(Endpoint(IpAddress::any_v4(), 0));
    host.start_thread(); // the network runs in the background; this thread only handles events

    std::printf("connecting to %s ...\n", server.to_string().c_str());
    const ConnectionId connection = host.connect(server);

    // Console input arrives on its own thread, since reading a line blocks. The thread is detached
    // (it may sit in getline forever), so the state it shares with this one is reference-counted.
    struct Input
    {
        std::mutex mutex;
        std::deque<std::string> lines;
        bool closed = false;
    };
    const auto shared_input = std::make_shared<Input>();
    std::thread([shared_input] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            std::lock_guard<std::mutex> lock(shared_input->mutex);
            shared_input->lines.push_back(line);
        }
        std::lock_guard<std::mutex> lock(shared_input->mutex);
        shared_input->closed = true;
    }).detach();
    std::mutex& lines_mutex = shared_input->mutex;
    std::deque<std::string>& lines = shared_input->lines;
    bool& input_closed = shared_input->closed;

    bool connected = false;
    for (;;)
    {
        Event event;
        if (host.wait_event(event, std::chrono::milliseconds(50)))
        {
            if (std::holds_alternative<Connected>(event))
            {
                connected = true;
                std::printf("connected; type to chat, end of input quits\n");
            }
            else if (const auto* failed = std::get_if<ConnectFailed>(&event))
            {
                std::printf("could not connect (reason %d)\n", static_cast<int>(failed->reason));
                return 1;
            }
            else if (std::holds_alternative<Disconnected>(event))
            {
                std::printf("disconnected\n");
                return 0;
            }
            else if (const auto* message = std::get_if<Message>(&event))
                std::printf("> %s\n", chat_text(*message).c_str());
        }

        std::lock_guard<std::mutex> lock(lines_mutex);
        while (connected && !lines.empty())
        {
            host.send(connection, chat_message(lines.front()));
            lines.pop_front();
        }
        if (input_closed && lines.empty())
        {
            host.disconnect(connection); // graceful: queued lines are delivered first
            input_closed = false;
        }
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
