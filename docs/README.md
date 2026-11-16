# Project stages

This repository covers every development stage of the driver. Specifications
(the original requirements) live in this directory; the implementation and
its tests are in `src/`, `include/` and `tests/`.

| Stage | Status | Description |
|-------|--------|-------------|
| 1 | done | Driver core, register map, asynchronous socket API, STM32 bare-metal port, FreeRTOS port, unit tests with a chip emulator |
| 2 | done | Fuzzing: 7 libFuzzer harnesses, corpus/dictionary, CI (host-tests + fuzz) |
| 3 | done | Core hardening: SIR-based polling and `w5500_irq_pending()`, link events, VERSIONR+RTR chip probe, `open_ex` flags, TTL/TOS/MSS/status/keep-alive probe, `ERR_BUSY` for an in-flight SEND, RTR/RCR tuning, `delay_us` |
| 4 | done | Per-socket buffer layout (`w5500_set_socket_buffers`), zero-length UDP semantics, link-recovery example and error-code table |
| 5 | done | Protocol layer: DHCP client, DNS resolver, plus `fuzz-dhcp`/`fuzz-dns` |
| 6 | done | Modbus TCP: server (FC 3/4/6/16, up to 4 clients, register map via callbacks) and polling client, `fuzz-modbus`, example checked in CI |

## Specifications

- [stage1.md](stages/stage1.md) - Stage 1 requirements: asynchronous W5500
  driver in plain C.

## Guides

- [architecture.md](architecture.md) - three-layer design, execution model,
  locking, polling, timeouts, design invariants.
- [api.md](api.md) - API reference grouped by module.
- [protocols.md](protocols.md) - DHCP, DNS, SNTP and Modbus TCP usage:
  lifecycle, timing, storage and integration recipes.
- [fuzzing.md](fuzzing.md) - harnesses, corpus and dictionary, run modes,
  CI and the nightly campaign.
- [tests/mock_w5500.md](../tests/mock_w5500.md) - the W5500 emulator used by
  unit tests and fuzz harnesses: SPI state machine, register/buffer model,
  socket command side effects, behavior knobs.

Related documents in the repository root:
[examples/README.md](../examples/README.md),
[ports/README.md](../ports/README.md),
[CONTRIBUTING.md](../CONTRIBUTING.md).
