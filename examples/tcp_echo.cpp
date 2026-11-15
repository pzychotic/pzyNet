// Single-threaded TCP echo server for many clients, using non-blocking sockets and the poller.
//   tcp_echo [port]        (default 7777)

#include "pzynet/socket/poller.hpp"
#include "pzynet/socket/tcp.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>

int main(int argc, char** argv)
{
    using namespace pzynet;
    try
    {
        const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 7777);

        TcpListener listener;
        listener.listen(Endpoint(IpAddress::any_v4(), port));
        listener.set_non_blocking(true);
        std::printf("echoing TCP on %s\n", listener.local_endpoint().to_string().c_str());

        Poller poller;
        poller.add(listener, PollEvents::readable);
        std::map<NativeSocket, TcpStream> clients;
        std::vector<PollEvent> events;
        std::array<std::uint8_t, 4096> buffer;

        for (;;)
        {
            poller.wait(events, std::chrono::milliseconds(-1));
            for (const PollEvent& event : events)
            {
                if (event.socket == listener.native_handle())
                {
                    Endpoint peer;
                    std::error_code ec;
                    TcpStream stream = listener.accept(peer, ec);
                    if (ec)
                        continue; // would-block, or the client already went away
                    std::printf("+ %s\n", peer.to_string().c_str());
                    poller.add(stream, PollEvents::readable);
                    const NativeSocket handle = stream.native_handle();
                    clients.emplace(handle, std::move(stream));
                    continue;
                }

                TcpStream& client = clients.at(event.socket);
                std::error_code ec;
                const std::size_t size = client.receive(buffer, ec);
                if (ec == std::errc::operation_would_block)
                    continue;
                if (ec || size == 0)
                {
                    std::printf("- client closed\n");
                    poller.remove(client);
                    clients.erase(event.socket);
                    continue;
                }
                // Blocking-style write for brevity; a real server would buffer and wait for writable.
                client.set_non_blocking(false);
                client.send_all(ByteView(buffer.data(), size), ec);
            }
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
