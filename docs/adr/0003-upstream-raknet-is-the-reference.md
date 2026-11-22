# Upstream RakNet 4.081 defines compatibility; the local fork is only a test oracle

Interop tests build against a local RakNet fork whose source has been modernised (e.g. `std::string`
in place of `RakString`, no `char*` overloads). Compatibility is defined by the bytes stock upstream
RakNet 4.081 puts on the wire, because that is what deployed applications run. Where the fork's
behaviour or documentation differs from upstream, upstream wins and the case is pinned by a
hand-verified test vector rather than by the fork-based interop test.
