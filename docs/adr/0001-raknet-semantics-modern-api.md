# RakNet semantics behind a modern API, not a drop-in RakNet API

The RakNet application layer (BitStream, Peer) matches RakNet on the wire and in what an application
can do with it (message ids, reliabilities, receipts, timestamps, serialization format), but uses
pzyNet's own C++17 style: snake_case, value types instead of raw pointers and out-parameters,
`error_code`/throwing pairs, `Endpoint` instead of `SystemAddress`. We do not reproduce
`RakPeerInterface`'s names or signatures, so porting a RakNet application means rewriting call sites,
not logic. A thin drop-in shim could be layered on top later if a port needs it.

## Considered Options

- **Drop-in replacement** (`RakPeerInterface`, `BitStream::Write`, `Packet*`): rejected because it would
  import RakNet's ownership and out-parameter style into a codebase that otherwise avoids it.
