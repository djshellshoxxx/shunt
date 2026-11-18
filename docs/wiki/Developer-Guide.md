# Developer Guide

## Build and test (checked on Linux)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # 2 of 2 pass: ShuntTests, shuntsim_localhost
```

Tests live in `tests/` (`test_clock.cpp`, `test_log.cpp`, `test_net.cpp`, `test_packets.cpp`) with a small framework in
`tests/TestFramework.h`. CI: `.github/workflows/ci.yml`.

## Tools

- `tools/shunt_cli`: command-line front end.
- `tools/shuntsim`: network simulator; its `shuntsim_localhost` test runs in ctest.

## Rules

- Protocol work starts from the packet and clock specs (`docs/specs/ES-01` to `ES-03`).
- The legal section in the README says the protocol is written from public packet analysis, with no code from EPL or GPL projects. Keep that true.
