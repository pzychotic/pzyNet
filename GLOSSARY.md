# pzyNet

A layered network library whose RakNet layers let a pzyNet program talk to unmodified RakNet 4.x
applications.

## Transport

**Engine**:
A datagram protocol modelled as a state machine that is fed datagrams and time, and produces datagrams
to send and events for the application.
_Avoid_: Protocol handler, stack

**Datagram**:
One UDP payload as it travels on the wire.
_Avoid_: Packet (that word is reserved for the Peer level)

**Connection**:
An established or in-progress session between this engine and one remote endpoint.
_Avoid_: Link, session (outside the engine internals)

**Message**:
One application payload delivered over a connection with a chosen reliability, channel and priority.
_Avoid_: Packet (that word is reserved for the Peer level), frame

## RakNet application layer

**BitStream**:
RakNet's serialization format for message contents: bit-packed, with single-bit booleans and its own
compressed and string encodings.
_Avoid_: Buffer, serializer

**Peer**:
A RakNet-style endpoint that owns a socket and a RakNet engine and offers RakNet's application-level
behaviour.
_Avoid_: RakPeer, host (the transport-level driver is the host)
(`rakpeer` is only the name of the library target and directory that hold the Peer, per ADR-0002; the
domain term and the type stay "Peer".)

**Message id**:
The first byte of every RakNet message, naming what the rest of it means; ids below 134 belong to
RakNet itself, the rest to the application.
_Avoid_: Opcode, packet type

**Timestamped message**:
A message that starts with `ID_TIMESTAMP` and an 8-byte send time, followed by its real message id.

**Packet**:
One item from a Peer's receive queue: either a received message or a connection event, named by its
message id. Used only at the Peer level.
_Avoid_: Packet for a datagram or a transport-level message

**GUID**:
The 64-bit identifier a RakNet system chooses for itself and announces to the systems it connects to.
_Avoid_: Peer id, system id
