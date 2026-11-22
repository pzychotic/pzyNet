# pzyNet

A C++17 network library for Windows and Linux, in layers that can be used on their own:

| Target | What it is | Depends on |
|---|---|---|
| `pzynet::core` | Byte views, byte/bit readers and writers, IP address and endpoint types, error helpers | — |
| `pzynet::socket` | UDP and TCP sockets, readiness poller, DNS resolver | core |
| `pzynet::transport` | Protocol-neutral connection interface (`Engine`) and a UDP driver (`UdpHost`) | core, socket |
| `pzynet::raknet` | The RakNet 4.x wire protocol as a `transport::Engine`, and RakNet's BitStream format | core (+ transport headers) |
| `pzynet::rakpeer` | A RakNet-style `Peer`: socket, engine and network thread, with RakNet's application semantics | raknet, transport, socket |

The RakNet layer speaks the wire protocol of stock RakNet 4.x (protocol version 6): a pzyNet program can
connect to, or accept connections from, an unmodified RakNet application. On top of it, the BitStream
and the Peer give RakNet's application-level behaviour in pzyNet's own C++17 style; they reproduce what
RakNet does, not its API names, and none of its plugins.

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
`find_package(pzynet COMPONENTS socket raknet rakpeer)`, then link `pzynet::socket`, `pzynet::rakpeer`, …

## Error handling

Every fallible call exists twice: `f(args)` throws `std::system_error`; `f(args, std::error_code&)` never
throws. Operating-system errors keep their native code; a non-blocking call that cannot complete reports
`std::errc::operation_would_block` on both platforms. Library-specific conditions are `pzynet::Errc`:
a configuration value over its limit (a password, an offline ping response) is `Errc::invalid_argument`,
data to be sent over its limit is `Errc::message_too_large`, and a call on a Peer that is not running is
`Errc::not_open`.

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
round-trip time, clock offset, time-out, external address) through `connection_info()`; time-outs per
connection or for all with `set_timeout()`. From another thread, reach the engine with
`host.with_engine(...)`. [examples/raknet_chat.cpp](examples/raknet_chat.cpp) is a complete program.

Messages are passed through untouched. When the other side is a RakNet application, start payloads with a
message id it understands (`raknet::id::user_packet_enum`, 134, is the first one free for applications).
Payloads must not start with an id the protocol consumes itself; `send()` rejects those.

## BitStream

`raknet::BitStreamWriter` and `raknet::BitStreamReader` read and write RakNet's BitStream format bit for
bit: values are packed most significant bit first with no regard for byte boundaries, a `bool` takes one
bit, and integers and enums of every width, `float` and `double` are written in the stream's byte order.
That is big-endian by default,
as in stock RakNet; pass `raknet::ByteOrder::little_endian` to talk to a RakNet build with
`__BITSTREAM_NATIVE_END`.

```cpp
#include "pzynet/raknet/bit_stream.hpp"

raknet::BitStreamWriter out;
out.write(std::uint8_t{raknet::id::user_packet_enum + 1}); // message id
out.write(true);                                           // one bit
out.write(std::int32_t{-42});                              // four bytes, not byte-aligned

raknet::BitStreamReader in(out.data());
in.read<std::uint8_t>();
bool flag = in.read<bool>();
std::int32_t value = in.read<std::int32_t>();
if (!in.ok()) { /* the message was too short */ }
```

The writer owns its buffer (`data()`, `bit_count()`); the reader is a non-owning view. A read past the
end returns zero and latches `ok() == false`, so a message can be read in full and checked once.

Beyond scalars: `write_bits()` / `read_bits()` take raw bits (with RakNet's right-aligned tail by default),
`write_bytes()` and `write_aligned_bytes()` raw bytes, `align()` pads to a byte boundary, and the reader
can `skip_bits()` / `skip_bytes()`. Your own types go through the stream once you declare free functions
next to them, found by argument-dependent lookup:

```cpp
namespace game
{
struct Position { std::int16_t x; float y; };
void write(raknet::BitStreamWriter& out, const Position& p) { out.write(p.x); out.write(p.y); }
void read(raknet::BitStreamReader& in, Position& p) { in.read(p.x); in.read(p.y); }
}

out.write(game::Position{3, 1.5f});
game::Position p = in.read<game::Position>(); // or: in.read(p) returns ok()
```

A hook that finds an invalid value can call `in.fail()`.

RakNet's own types are built in, with RakNet's encodings: `write(raknet::Guid)` (its 64-bit value),
`write(Endpoint)` (RakNet's IPv4 SystemAddress; an IPv6 endpoint is an error, `address_family_mismatch`),
`write_uint24()` (byte-aligned, little-endian), `write_string()` (RakString: 16-bit length, then the
bytes from the next byte boundary) and `write_wide_string()` (RakWString, which upstream also uses for
its non-Huffman `WriteCompressed` of wide strings: 16-bit count, then 16-bit code units), each with its
`read` counterpart. `write_huffman_string()` / `read_huffman_string()` are RakNet's compressed strings
(`WriteCompressed(RakString)`): Huffman-coded with RakNet's StringCompressor and its English frequency
table, after the code bit count as a compressed integer.

RakNet's space-saving encodings have their own calls, each with a matching read: `write_compressed()`
(integers without their zero bytes, floats and doubles in [-1, 1] as 16 and 32 bits), `write_delta()` and
`write_compressed_delta()` (one bit for an unchanged value), `write_float16(value, min, max)` and
`write_ranged(value, min, max)` (the fewest bits that hold `max - min`). They reproduce RakNet's bytes
exactly, quirks included: in a big-endian stream a compressed integer drops *trailing* zero bytes, so
small numbers only shrink in little-endian streams, and negative numbers are never shortened.

RakNet's quantized game-math helpers live in `pzynet/raknet/bit_stream_geometry.hpp`, over
`raknet::Vector3<T>`, `Quaternion<T>` and `Matrix3<T>` (float or double components):
`write_normalized_vector()` (6 bytes), `write_vector()` (length plus direction, 4 or 10 bytes),
`write_normalized_quaternion()` and `write_orthogonal_matrix()` (52 bits each), and the matching
`read_normalized_vector<T>(in)`, `read_vector<T>(in)`, ... They write what RakNet's `WriteNormVector`,
`WriteVector`, `WriteNormQuat` and `WriteOrthMatrix` write, and read back RakNet's values exactly.

```cpp
raknet::write_orthogonal_matrix(out, raknet::Matrix3<float>{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}});
raknet::Matrix3<float> rotation = raknet::read_orthogonal_matrix<float>(in);
```

## Peer

`raknet::Peer` is a RakNet endpoint in a few lines: it owns the socket and the RakNet engine and runs the
network on a background thread (or, with `Threading::app_driven`, whenever you call `update()`).
Everything it has to report arrives through `receive()` as a `raknet::Packet`, named by its RakNet
message id, so a program dispatches on the first byte as RakNet programs do.

On Windows, short waits are rounded up to the system timer tick, about 15.6 ms by default. An app-driven
loop that calls `update(1ms)` then runs the network far less often than it asks for, and an unreliable
send with an acknowledgement receipt can be reported lost (`id::snd_receipt_loss`) although it arrived,
because its receipt deadline passes before the acknowledgement is processed. RakNet behaves the same.
Raise the timer resolution with `timeBeginPeriod(1)` (winmm) for the life of the loop, as real-time
applications do; the tests do so too.

```cpp
#include "pzynet/rakpeer/peer.hpp"

using namespace pzynet;

raknet::PeerConfig config;
config.bind = {IpAddress::any_v4(), 60000};
config.engine.max_incoming_connections = 32;   // the raknet::Config of the engine
raknet::Peer peer;
peer.start(config);

for (;;) {
    while (std::optional<raknet::Packet> packet = peer.receive()) {
        switch (packet->id) {
        case raknet::id::new_incoming_connection: /* packet->guid, packet->address */ break;
        case raknet::id::disconnection_notification: break;
        case raknet::id::user_packet_enum: {
            raknet::BitStreamReader in = packet->reader(); // positioned after the message id
            bool flag = in.read<bool>();
            peer.send(packet->guid, packet->data);          // echo, reliable ordered by default
            break;
        }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

- `connect(endpoint, password)` reports its outcome as `id::connection_request_accepted` or one of
  RakNet's failure ids (`connection_attempt_failed`, `no_free_incoming_connections`, `invalid_password`,
  `connection_banned`, `incompatible_protocol_version`, `already_connected`, `ip_recently_connected`,
  `remote_system_requires_public_key`). An accepted connection on the other side is
  `id::new_incoming_connection`. `PeerConfig::engine.incoming_password` sets the password incoming
  connections must present.
- `connect(host, port, password)` takes a host name instead, resolved synchronously to its first IPv4
  address; a name that does not resolve fails the call (`Errc::host_not_found`) and produces no Packet.
- Remote systems are named by `raknet::Guid` or by `Endpoint`. `send()` takes a `BitStreamWriter` or raw
  bytes starting with the message id, with a reliability, channel and priority, and returns the send's
  receipt number. Addressing a system that is not connected fails with `Errc::not_connected`.
- `connections()` lists the connected systems with GUID, address, round-trip time, MTU and direction;
  `guid_of(endpoint)` and `address_of(guid)` look one up.
- Without a connection: `ping(endpoint, only_if_accepting)` yields an `id::unconnected_pong` Packet
  (`[id][u32 ping time][response]`), answered with `set_offline_ping_response()`;
  `advertise(endpoint, data)` arrives as `id::advertise_system`. `set_max_incoming_connections()` and
  `set_incoming_password()` change those settings while running.
- A remote close arrives as `id::disconnection_notification`, a time-out as `id::connection_lost`.
  `disconnect()` notifies the remote system but, as in RakNet, produces no Packet locally; messages
  already received from that system are still delivered. Disconnecting a system that is not connected
  fails with `Errc::not_connected`.
- `ban(address, duration)` refuses an IPv4 address (`"192.168.*"` wildcards allowed; zero duration
  bans permanently) with `id::connection_banned`; `unban()`, `clear_bans()` and `is_banned()` complete
  RakNet's ban list. `set_limit_ip_connection_frequency(true)` (or `PeerConfig::engine`) refuses an IP
  that connected less than 100 ms before with `id::ip_recently_connected`. As in RakNet, this host's
  own addresses are exempt: those its host name resolves to, a specific bound address, and the external
  address the first handshake reported (`RakNetEngine` takes the local ones as `Config::local_addresses`).
- `statistics(remote)` returns a `raknet::ConnectionStats` for a connected remote: bytes and messages
  sent and received, resent datagrams, datagrams presumed lost, and the send queue in messages and bytes
  (the engine offers the same through `RakNetEngine::connection_stats()`). It is a small diagnostic
  set, not RakNet's `RakNetStatistics`.
- `set_timeout(remote, ms)` overrides the time-out for one connection, `set_timeout(ms)` sets it for all
  (RakNet's `SetTimeoutTime`). `external_address(remote)` is this Peer's address as that remote system
  reported it during the handshake (RakNet's `GetExternalID`).
- `shutdown(linger)` with a positive linger closes every connection gracefully and waits, at most
  that long, until the remote systems have acknowledged the pending reliable data and their
  disconnection notification (in either threading mode). `shutdown()` with zero linger, and the
  destructor, stop at once without notifying anyone; the remotes see a lost connection.
- Timestamped messages work as in RakPeer: send `id::timestamp`, `peer.time()` (8 bytes) and then the
  real message id. On receipt the time is converted into the receiver's clock using the connection's
  measured clock offset; it is rewritten in the data and available as `packet->timestamp`, and
  `packet->reader()` starts at the real message id. Other Packets have no timestamp.
- `PeerConfig::byte_order` sets the byte order of the readers that Packets hand out (and of the
  timestamp in timestamped messages); `PeerConfig::clock` replaces the steady clock the engine and
  `time()` see (useful in tests).

[examples/rakpeer_chat.cpp](examples/rakpeer_chat.cpp) is the chat example rebuilt on the Peer and the
BitStream; it talks to [examples/raknet_chat.cpp](examples/raknet_chat.cpp) and to RakNet applications
that use the same layout.

## Compatibility and limits

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
  its encoders and the BitStream against byte-exact vectors. Peers are tested in pairs over loopback.
- `pzynet_interop_tests` (built when `PZYNET_RAKNET_INTEROP_DIR` is set): real loopback sessions between
  pzyNet and a RakNet peer built from that checkout, in both directions — handshake, passwords, all
  reliabilities, ordering, split messages, bulk transfer, disconnects, time-outs, keep-alive, ping/pong,
  advertise, and several RakNet clients against one pzyNet server. The Peer is checked against a
  RakPeer, and BitStream messages against RakNet's own BitStream.

```bash
cmake --preset msvc -DPZYNET_RAKNET_INTEROP_DIR=D:/path/to/RakNet
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```
