# Architecture

The driver is split into three layers that never leak into each other:

| Layer | Files | Responsibility |
|-------|-------|----------------|
| 1. Platform | `w5500_plat_if_t` hooks, `ports/*` | SPI select/deselect/byte/burst, delays, optional RTOS lock |
| 2. Core | `src/w5500.c`, `src/w5500_registers.h` | Register access, framing, timeouts, network configuration |
| 3. Sockets | `src/w5500_sockets.c`, `include/w5500_sockets.h` | Slot allocation, state machine, events, data path |

Protocol modules (`dhcp`, `dns`, `sntp`, `modbus`) sit **on top of layer 3**
and use the public socket API only; the core does not know about them.

## Context and lifetime

- All state lives in `struct w5500_context`, reachable through the opaque
  `w5500_handle_t`. There are no driver globals, so several chips can be
  driven from one image.
- `w5500_init()` allocates the context; `w5500_init_static()` uses caller
  storage of `w5500_context_size()` bytes. `w5500_deinit()` frees only
  contexts it allocated.
- The platform interface is copied into the context by value; the hooks
  themselves must stay valid forever.
- A double `w5500_deinit()` is a documented use-after-free and is not
  defended against.

## Sockets and states

Four hardware sockets are tracked in a fixed array inside the context.

- `w5500_socket_open[_ex]()` claims the first free slot *before* touching the
  chip and returns a pointer to it. Exhausting the slots returns
  `W5500_ERR_NOMEM`.
- Each slot carries a software state (`FREE`, `INITIATED`, `LISTENING`,
  `CONNECTING`, `ESTABLISHED`, `CLOSING`) that gates the API; the hardware
  `Sn_SR` remains the source of truth for events.
- On `DISCONNECTED` or a fatal `ERROR` the driver closes the chip socket and
  releases the slot **before** the callback runs. A released handle must not
  be reused; reopen from the callback instead.
- `w5500_socket_close()` drops the connection immediately (no FIN);
  `w5500_socket_disconnect()` performs a graceful close and completes through
  `DISCONNECTED`.

## Execution model

`w5500_poll()` is the only event pump:

1. It reads the chip-wide `SIR` (one socket summary bit per slot) and the
   common `IR` under the driver lock.
2. Only sockets with their SIR bit set have `Sn_IR` read. TCP sockets without
   a pending bit fall back to a single `Sn_SR` read so passive closes are still
   noticed; UDP sockets skip it entirely (UDP has no passive close).
3. Events are queued into a fixed array (`W5500_POLL_MAX_EVENTS = 16`, four
   per socket). Events beyond the cap are dropped - a slot can produce at
   most four per call, so this only matters for abnormal chip behaviour.
4. The lock is released and the callbacks run in the poll caller's context.
   Re-entering the driver from a callback is safe and exercised by
   `fuzz-poll`.
5. When neither `IR` nor `SIR` was pending, `PHYCFGR` is sampled. A change in
   `LNK/SPD/DPX` updates the stored snapshot and fires the link callback
   (also outside the lock). The first sample only establishes the baseline.

Half-close handling: when the peer sends data followed by FIN, the chip
raises `RECV` and `DISCON` together. The driver keeps the slot alive, delivers
`DATA_RECEIVED`, and only closes the socket, releases the slot and reports
`DISCONNECTED` after the RX buffer has been drained (normally on the next
poll). A fatal `TIMEOUT` still wins: it closes the socket immediately and any
buffered RX is discarded.

Lost-interrupt recovery: if `SEND_OK` is missed (chip reset, a link flap in
the middle of a send), the socket would otherwise stay `ERR_BUSY` forever.
When a SEND is pending and `Sn_TX_RD == Sn_TX_WR`, the driver treats the frame
as delivered, clears the guard and emits `SEND_DONE` itself.

Ordering rule: call `w5500_poll()` before the protocol polls
(`w5500_dhcp_poll()`, ...) in the same iteration, so pending `SEND_DONE`
events are consumed before a protocol tries to transmit again.

## Locking contract

- `lock`/`unlock` are optional; when NULL they are no-ops (single-threaded
  bare metal).
- Every public entry point acquires the lock around chip I/O and context
  mutation, including nested helpers.
- **Callbacks run with the lock released.** Applications may call any API
  from a callback. `fuzz_*` harnesses assert the lock/unlock balance after
  every input.
- The FreeRTOS port provides one global non-recursive mutex and is intended
  for a single driver instance; multi-chip users supply their own hooks.

## Data path

- **Zero-copy both ways**: `send`/`sendto` write user memory straight into
  the chip TX buffer, `recv`/`recvfrom` read chip RX memory straight into the
  application buffer.
- **Partial sends**: `send` accepts at most the free TX space and reports it
  in `*sent`. A full TX buffer is not an error: it returns `W5500_OK` with
  `*sent == 0`; retry later.
- **One SEND in flight per socket**: a second `send` before `SEND_DONE`
  returns `W5500_ERR_BUSY` with `*sent == 0`. Sending again from the
  `SEND_DONE` callback is the intended pattern.
- **Error precedence**: `STATE` (wrong protocol/state) before `PARAM` (NULL,
  zero length) before `BUSY`.
- **UDP datagrams**: the W5500 prepends an 8-byte header to every datagram
  in RX memory (source IP 4, source port 2, datagram length 2); it is included
  in `Sn_RX_RSR`. `recvfrom()` reads and skips that header, uses its peer
  address/port and limits the read to one datagram's payload. A partial read
  keeps the rest of the datagram in the slot (`udp_remaining`), so the next
  `recv()` continues the payload without re-reading the header. TCP instead
  latches the peer in `Sn_DIPR`/`Sn_DPORT` before consuming data.
  A zero-length UDP datagram raises `DATA_RECEIVED` with `Sn_RX_RSR == 8`
  (header only), so `recv` returns 0 bytes - treat that as an empty datagram,
  not an error.
- **Buffer layout**: `Sn_TXBUF_SIZE`/`Sn_RXBUF_SIZE` split 16 KB per
  direction. Changing the layout invalidates every open socket.

## Timeouts and delays

- `w5500_set_timeout()` bounds every chip wait (default 1000 ms, 0 is
  rejected).
- Without `delay_us`, waits step in 1 ms increments using `delay_ms` (or just
  counting when the hook is NULL).
- With `delay_us`, waits step in 100 us increments, giving much faster
  `open`/`listen`/command completion.
- `w5500_read16_stable()` re-reads free-space counters until two samples
  agree and gives up with `W5500_ERR_TIMEOUT` after eight mismatches.

## Interrupt-driven designs

`w5500_irq_pending()` returns a snapshot of `IR` and `SIR` without servicing
anything. The intended INTn pattern is: the EXTI ISR sets a flag, the network
task checks the flag, calls `w5500_poll()` and clears it. `w5500_poll()`
itself never sleeps and never waits for the chip beyond the command timeout.

## Memory policy

- No heap in the core or socket layer; the context (including its four
  slots) is a single allocation supplied by the caller or by `w5500_init`.
- Protocol modules are also caller-allocated: each exposes `*_size()` and an
  `_init(&ptr, mem, size, ...)` that stores all state, packet buffers and
  results inside that block (DHCP/DNS/SNTP roughly 1.2 KB each, Modbus server
  about 2.2 KB, Modbus client about 1 KB).
- Nothing is reference-counted; lifetime is the caller's responsibility.

## Design invariants

These are the properties the test suite and fuzzers enforce:

1. No globals; two contexts never share state.
2. Lock balance: every `lock` is paired with exactly one `unlock`, even on
   error paths.
3. Slots are released on `DISCONNECTED`/`ERROR` and only then.
4. Lengths: `*sent <= len`, `*recvd <= len`, `*recvd <= pending RX bytes`;
   free-space counters never exceed the configured buffer size.
5. Register round-trips match the emulator's documented exceptions (MR reset,
   Sn_CR command, Sn_IR write-1-to-clear, computed FSR/RSR, absent blocks).
6. A read on a valid handle only fails for `len == 0`.
7. Protocol callbacks fire exactly once per completed request.
8. Data followed by a peer FIN is delivered before `DISCONNECTED`; the slot
   stays claimable until the RX buffer is empty.
9. A lost `SEND_OK` cannot wedge a socket: the driver recovers the guard from
   the TX counters and emits `SEND_DONE`.

## Testing strategy

- `tests/mock_w5500.c` emulates the SPI state machine, register/buffer memory
  and socket side effects (including knobs for stuck commands and stalled
  sends).
- `tests/test_w5500.c`, `tests/test_protocols.c`, `tests/test_modbus.c` cover
  the core, the protocol layers and Modbus against that emulator.
- `tests/fuzz/` (13 harnesses) fuzzes register access, framing, event
  dispatch, the data path, context lifetime and all protocol codecs and state
  machines. See `docs/fuzzing.md`.
