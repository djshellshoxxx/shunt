# Codebase Map: where to find what

| I want to... | Look in |
|---|---|
| Packet formats | `net/include/shunt/net/Packets.h`, `net/src/Packets.cpp` |
| Device table and claiming | `net/include/shunt/net/DeviceTable.h`, `ClaimMachine.h` |
| Master tracking | `net/include/shunt/net/MasterTracker.h`, `net/src/MasterTracker.cpp` |
| Network stack and sockets | `net/src/NetworkStack.cpp`, `net/src/PosixSocket.cpp`, `Socket.h` |
| Capture files | `net/src/Pcapng.cpp`, `Pcapng.h` |
| Clock engine | `clock/src/ClockEngine.cpp`, `clock/include/shunt/clock/ClockEngine.h` |
| Tracklist logging | `log/src/Tracklist.cpp`, `log/include/shunt/log/Tracklist.h` |
| CLI | `tools/shunt_cli/main.cpp` |
| Simulator | `tools/shuntsim/main.cpp` |
| Tests | `tests/` |
| Specs and research | `docs/specs/`, `docs/research/`, `docs/PROJECT-PLAN.md` |
| CI | `.github/workflows/ci.yml` |
