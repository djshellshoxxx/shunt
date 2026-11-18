# Architecture and Design Rationale

Sources: README, `docs/specs/ES-01` to `ES-03`, `docs/DIFFERENTIATION.md`, `docs/research/`.

- **Layers.** `net` (packets, device table, claim machine, master tracker, PCAPNG, sockets), `clock` (clock engine), `log` (tracklist). The libraries are built as separate CMake targets.
- **Clock engine per player.** State is kept per player and a tempo estimator runs over it. A reset is a hard re-phase (ES-03 sections 2 to 4). Bar phase and a state machine follow (sections 5 and 6).
- **Publishing policy is separate.** ES-03 section 7 defines what is published, and ES-04 consumes it.
- **Lock-free handoff.** `net/include/shunt/net/SpscQueue.h` is a single-producer, single-consumer queue, so the network thread and consumers do not share locks. *(inferred from the header name.)*
- **Clean-room protocol.** The README says the protocol is written from publicly documented packet analysis, and that no EPL or GPL code is used.
- **Test-first simulation.** The simulator lets network behaviour be tested without hardware (`tools/shuntsim`).
