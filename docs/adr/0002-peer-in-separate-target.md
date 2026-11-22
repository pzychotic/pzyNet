# The RakNet Peer lives in its own target, `pzynet::rakpeer`

`pzynet::raknet` stays a protocol state machine that owns no socket, thread or clock and depends only
on core, so it can be tested deterministically. The RakNet-style Peer needs a socket and a background
thread, so it goes in a separate target, `pzynet::rakpeer`, which depends on `raknet`, `transport` and
`socket`. The RakNet BitStream, being a pure serialization format, stays in `pzynet::raknet`.
