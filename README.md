# pzyNet

A C++17 network library for Windows and Linux, in layers that can be used on their own:

| Target | What it is | Depends on |
|---|---|---|
| `pzynet::core` | Byte views, byte/bit readers and writers, IP address and endpoint types, error helpers | — |
| `pzynet::socket` | UDP and TCP sockets, readiness poller, DNS resolver | core |
| `pzynet::transport` | Protocol-neutral connection interface (`Engine`) and a UDP driver (`UdpHost`) | core, socket |
| `pzynet::raknet` | The RakNet 4.x wire protocol as a `transport::Engine` | core (+ transport headers) |

The RakNet layer speaks the wire protocol of stock RakNet 4.x (protocol version 6): a pzyNet peer can
connect to, or accept connections from, an unmodified RakNet application. It implements none of RakNet's
API and none of its plugins.

## Building

Requires CMake ≥ 3.24 and a C++17 compiler (tested: MSVC 2026, GCC 15, Clang 21). Catch2 3.x is found on
the system or downloaded automatically.

```bash
cmake --preset msvc            # Windows; on Linux: gcc-debug, gcc-release or clang-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

| Option | Default | Meaning |
|---|---|---|
| `PZYNET_BUILD_TESTS` | on when top-level | build the Catch2 suite |
| `PZYNET_BUILD_EXAMPLES` | on when top-level | build `examples/` |
| `PZYNET_WARNINGS_AS_ERRORS` | off | `/WX` / `-Werror` |
| `PZYNET_RAKNET_INTEROP_DIR` | empty | path to a RakNet checkout; enables the live interop tests |
| `BUILD_SHARED_LIBS` | off | standard CMake switch |

Consume it with `add_subdirectory`/`FetchContent`, or install it and use
`find_package(pzynet COMPONENTS socket raknet)`, then link `pzynet::socket`, `pzynet::raknet`, …

## Error handling

Every fallible call exists twice: `f(args)` throws `std::system_error`; `f(args, std::error_code&)` never
throws. Operating-system errors keep their native code; a non-blocking call that cannot complete reports
`std::errc::operation_would_block` on both platforms. Library-specific conditions are `pzynet::Errc`.

## Sockets

```cpp
#include "pzynet/socket/udp_socket.hpp"

pzynet::UdpSocket socket;
socket.bind({pzynet::IpAddress::any_v4(), 7777});

std::array<std::uint8_t, 1500> buffer;
pzynet::Endpoint from;
std::size_t size = socket.receive_from(buffer, from);
socket.send_to({buffer.data(), size}, from);
```

`UdpSocket`, `TcpStream`, `TcpListener` are move-only RAII handles. An IPv6 socket with
`set_v6_only(false)` is dual-stack and transparently maps IPv4 endpoints. `Poller` waits on many sockets
(`WSAPoll` / `epoll`) and can be woken from another thread; `resolve()` wraps `getaddrinfo`. UDP options
include broadcast, multicast membership/TTL/loopback and the don't-fragment flag. See
[examples/udp_echo.cpp](examples/udp_echo.cpp) and [examples/tcp_echo.cpp](examples/tcp_echo.cpp).

## Transport and RakNet

A protocol is a `transport::Engine`: a pure state machine that is fed datagrams and the current time and
hands back datagrams to send and events for the application. It owns no socket, thread or clock, which
makes it deterministic to test. `transport::UdpHost` runs an engine over a UDP socket, either driven by
your loop (`update()` / `wait_event()`) or by a background thread (`start_thread()`).

```cpp
#include "pzynet/raknet/raknet_engine.hpp"
#include "pzynet/transport/udp_host.hpp"

using namespace pzynet;

raknet::Config config;
config.max_incoming_connections = 32;          // 0 (the default) makes a pure client
raknet::RakNetEngine engine(config);
transport::UdpHost host(engine);
host.bind({IpAddress::any_v4(), 60000});

for (;;) {
    transport::Event event;
    if (!host.wait_event(event, std::chrono::milliseconds(100)))
        continue;
    if (auto* message = std::get_if<transport::Message>(&event))
        host.send(message->connection, message->data);          // echo, reliable ordered by default
}
```

A client calls `host.connect(endpoint, password)` and waits for `Connected` or `ConnectFailed`.
`SendOptions` select reliability (the five RakNet reliabilities), ordering channel (0–31), priority and
an optional receipt token (RakNet's `*_WITH_ACK_RECEIPT`). Unconnected ping/pong and "advertise system"
are available through `RakNetEngine::ping()` / `advertise()`; RakNet-specific state (GUIDs, MTU,
round-trip time, clock offset) through `connection_info()`. From another thread, reach the engine with
`host.with_engine(...)`. [examples/raknet_chat.cpp](examples/raknet_chat.cpp) is a complete program.

Messages are passed through untouched. When the other side is a RakNet application, start payloads with a
message id it understands (`raknet::id::user_packet_enum`, 134, is the first one free for applications).
Payloads must not start with an id the protocol consumes itself; `send()` rejects those.

### Compatibility and limits

- Interoperates with RakNet 4.x built with default settings: no `LIBCAT_SECURITY`, no
  `RAKNET_SUPPORT_IPV6`, 64-bit time, protocol version 6. A server that demands security is reported as
  `ConnectFailure::unsupported`.
- There is no encryption or authentication on the wire — that is RakNet's protocol. Use it on trusted
  networks or under a tunnel.
- Unlike RakNet, incoming data is bounded: message size (`Config::max_message_size`), reassembly and
  reordering buffers are limited, and a peer exceeding them is disconnected (`protocol_violation`).
- Internals (timers, queueing, datagram packing) are not copied from RakNet, only what is visible on the
  wire is.

## Tests

- `pzynet_tests`: unit tests for every layer. The RakNet engine is tested without sockets, through an
  in-memory network with a fake clock (loss, duplication, reordering, MTU limits, hostile input), and
  its encoders against byte-exact vectors.
- `pzynet_interop_tests` (built when `PZYNET_RAKNET_INTEROP_DIR` is set): real loopback sessions between
  pzyNet and a RakNet peer built from that checkout, in both directions — handshake, passwords, all
  reliabilities, ordering, split messages, bulk transfer, disconnects, time-outs, keep-alive, ping/pong,
  advertise, and several RakNet clients against one pzyNet server.

```bash
cmake --preset msvc -DPZYNET_RAKNET_INTEROP_DIR=D:/path/to/RakNet
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```
