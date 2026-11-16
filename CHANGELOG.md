# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning: [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- MQTT 3.1.1 client (`w5500_mqtt.h`): QoS 0 publish/subscribe over one TCP
  socket, caller-owned storage, tick-driven poll, keepalive PINGREQ and
  auto-reconnect on link loss; codec and state-machine fuzzing
  (`fuzz-mqtt`, `fuzz-mqtt-state`).
- Basic HTTP examples: shared codec (`examples/http/`), STM32 HTTP client
  (DHCP -> DNS -> GET -> parsed status/headers/body), STM32 HTTP server
  (port 80, 200/404), host demo in CI, `tests/test_http.c`.
- Stronger tests: unstable FSR/RSR counter (read16_stable timeout), UDP
  header error paths (truncated/inconsistent), partial datagrams, two-step
  DHCP renew (unicast + NAK), Modbus callback exception 04, SNTP LI=3.
  `fuzz-send-recv` now fuzzes the unstable-counter knob.
- SNTP client (`w5500_sntp.h`): mode-3 request, Unix time, 3 attempts 2 s
  apart, `fuzz-sntp` harness.
- IGMPv2 for UDP multicast: `W5500_SOCK_F_IGMP_V2` flag (Sn_MR.MC, requires
  `W5500_SOCK_F_MULTI`).
- `w5500_set_socket_buffers_ex()` - independent TX/RX layouts per socket.
- Nightly `Nightly fuzz` workflow: `FUZZ_TIME=300`, corpus and crash
  artifact uploads.
- Modbus TCP (`w5500_modbus.h`): server for FC 3/4/6/16 with up to 4 clients
  and a callback-based register map, plus a polling client; `fuzz-modbus`;
  `examples/stm32f4_modbus_server`; `make examples` in CI.
- Documentation set: `docs/architecture.md`, `docs/api.md`,
  `docs/protocols.md`, `examples/README.md`, `ports/README.md`,
  `CONTRIBUTING.md`.
- `examples/host_mock_demo`: the full DHCP -> DNS -> TCP flow executed
  against the emulator; `make examples` now builds and runs it in CI.
- `examples/stm32f4_bench` plus `w5500_port_stm32_cycles()`: cycle-count
  measurements for check, send staging and idle polling.
- CI: gcc and clang matrix for `make test`/`make examples`, plus a gcov
  coverage artifact.

### Fixed
- Data followed by a peer FIN is no longer lost: the driver keeps the socket
  alive, delivers `DATA_RECEIVED`, and reports `DISCONNECTED` only after the
  RX buffer is drained.
- A lost `SEND_OK` interrupt can no longer wedge a socket in `ERR_BUSY`: the
  TX read/write counters are used to finish the send and emit `SEND_DONE`.
- A fatal `TIMEOUT` no longer emits a stale `DATA_RECEIVED` for data that is
  being discarded; the error event is the only event for that socket.

### Fixed
- `open_ex`: Sn_MR option flags (ND/MULTI/BCASTB/MC) no longer leak into the
  software protocol tag, which made `connect/send/listen` on a flagged socket
  return `W5500_ERR_STATE` by mistake.
- The Modbus client parses replies with a dedicated parser
  (`mb_parse_response`): a reply has a different MBAP layout than a request.

### Changed
- UDP sockets no longer read `Sn_SR` during `w5500_poll()`: UDP has no
  passive close, so one SPI read per socket per poll is saved.

## [0.5.0] - 2026-11-15

### Added
- Core: `w5500_irq_pending()` and SIR-based polling - `w5500_poll()` reads
  only sockets whose SIR summary bit is set.
- Link events: `w5500_get_link_info()` (LNK/SPD/DPX) and
  `w5500_set_link_cb()`; PHYCFGR is sampled when no other interrupt is
  pending.
- `w5500_check()` also verifies the factory RTR value;
  `w5500_set_ping_block()`.
- `w5500_socket_open_ex()` with no-delay/multicast/broadcast-block flags;
  TTL/TOS, MSS, socket status, manual keep-alive probe (SEND_KEEP).
- `W5500_ERR_BUSY`: one SEND in flight per socket.
- `w5500_set_retransmit()`/`w5500_get_retransmit()` (RTR/RCR).
- Optional `delay_us` platform hook - 100 us steps while waiting for Sn_CR;
  DWT implementation in the STM32 port.
- `w5500_set_socket_buffers()` - per-socket buffer layout.
- DHCP client (`w5500_dhcp.h`) and DNS resolver (`w5500_dns.h`) as a
  separate layer: no heap, tick driven, over a regular UDP socket.
- `examples/stm32f4_dhcp_dns`: DHCP -> DNS -> TCP without an OS.

### Changed
- `send`/`sendto` error precedence: `STATE` -> `PARAM` -> `BUSY`.

### Fixed
- `fuzz-reg-access`: round-trip expectations account for socket register
  aliasing (`off % 0x30`), found by CI.
- `mock_w5500`: SIR is computed as the OR of per-socket `Sn_IR` (as on real
  silicon).
- `send(NULL, ...)` on a busy socket returns `PARAM`, not `BUSY`.

### Fuzzing
- 9 harnesses: `fuzz-dhcp` and `fuzz-dns` added, plus seeds for a DHCP
  offer, a DNS answer and the error-precedence regression.

## [0.4.0] - Stage 2

### Added
- Fuzzing: 7 libFuzzer harnesses, corpus, dictionary, ASan/UBSan modes,
  CI (host-tests + fuzz).

## [0.3.0] - Stage 1

### Added
- Asynchronous W5500 core in C99: global-free context, zero-copy,
  non-blocking sockets with events, STM32 (bare-metal CMSIS) and FreeRTOS
  ports, unit tests against a chip emulator.
