// UDP echo server using only the socket layer.
//   udp_echo [port]        (default 7777; listens on IPv4 and IPv6)

#include "pzynet/socket/udp_socket.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv)
{
    using namespace pzynet;
    try
    {
        const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 7777);

        UdpSocket socket;
        socket.open(IpAddress::Family::v6);
        socket.set_v6_only(false); // dual-stack: IPv4 peers show up as plain IPv4 endpoints
        socket.bind(Endpoint(IpAddress::any_v6(), port));
        std::printf("echoing UDP datagrams on %s\n", socket.local_endpoint().to_string().c_str());

        std::array<std::uint8_t, 65536> buffer;
        for (;;)
        {
            Endpoint from;
            const std::size_t size = socket.receive_from(buffer, from);
            std::printf("%zu bytes from %s\n", size, from.to_string().c_str());
            socket.send_to(ByteView(buffer.data(), size), from);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
