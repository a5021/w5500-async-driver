# Host-side W5500 emulator (`mock_w5500`)

The emulator implements the W5500 SPI state machine, register/buffer memory
model and socket command side effects. It is used by `tests/test_w5500.c`,
`tests/test_protocols.c`, `tests/test_modbus.c` and all fuzz harnesses. It is
**not** part of the driver — only of the test suite.

## Memory model

| Region | Address | Size | Meaning |
|--------|---------|------|---------|
| `common` | `0x0000`..`0x003F` | 64 B | Common registers (MR, GAR, SUBR, SHAR, SIPR, IR, SIR, RTR, RCR, PHYCFGR, VERSIONR, ...) |
| `sreg[4][0x30]` | socket block | 4×48 B | Per-socket registers (Sn_MR..Sn_KPALVTR) |
| `tx_mem[4]` | per socket | 16 KB | TX data buffer per socket |
| `rx_mem[4]` | per socket | 16 KB | RX data buffer per socket |

The emulator only keeps raw register images and data buffers; it does not
model Ethernet framing, ARP, ICMP, or TCP retransmission timing.

## Buffer size semantics

Buffer sizes are stored in `Sn_TXBUF_SIZE` / `Sn_RXBUF_SIZE` as a power-of-2
KB code (1, 2, 4, 8 or 16). `buf_size_kb()` clamps any other value (reachable
through raw `w5500_write_regs()` fuzzing) to the 2 KB power-on default. All TX
and RX address masks are `(size - 1)`, so an invalid size silently wraps to
2 KB instead of indexing past the 16 KB buffer arrays.

## SPI state machine

The emulator parses the W5500 SPI frame per byte:

| Phase | Byte meaning | Effect |
|-------|--------------|--------|
| 0 | Address high | `addr = byte << 8` |
| 1 | Address low | `addr |= byte` |
| 2 | Control | `bsb = (byte >> 3) & 0x1F`, `rwb = (byte >> 2) & 1` |
| 3 | Data | read or write at `addr`; `addr++` afterwards |

- Between frames: `spi_select()` resets phase to 0 and sets CS; `spi_deselect()`
  clears CS and phase.
- Idle bus (CS high) returns `0xFF` on `spi_transfer_byte()`.
- On read (`RWB=1`) the data byte is returned; on write (`RWB=0`) the data
  byte is discarded and written to memory.
- `bus_reads` counter increments on every SPI read byte — used by tests to
  assert the poll read budget.

`spi_transfer_burst()` defaults to a byte loop; `mock_w5500_plat_burst()` wires
the same state machine for burst transfers.

## Behavior knobs

All knobs default to 0 except `connect_ok` (default 1). They survive
`mock_w5500_reset()` (only `connect_ok` is forced back to 1), so they can be
set before a reset to model a chip condition that persists across resets.

| Knob | Field | Effect |
|------|-------|--------|
| `cr_stuck` | `g_mock.cr_stuck` | When writing `Sn_CR`, the value is stored but never self-clears and `mock_handle_cmd()` is not called. Models a bus hang where the chip never accepts the command. |
| `connect_ok` | `g_mock.connect_ok` | `CONNECT` on a TCP socket in INIT state succeeds (`ESTABLISHED` + `CON` IR) when 1; fails (`CLOSED` + `TIMEOUT` IR) when 0. |
| `send_stall` | `g_mock.send_stall` | `SEND` command is accepted but `mock_do_send()` is never called: TX data stays in the buffer and `SEND_OK` is never set. Use `mock_w5500_complete_send()` to finish it manually. |
| `phycfgr` | `g_mock.phycfgr` | Raw `PHYCFGR` byte; bit0 = link on, bit1 = 100 Mbps, bit2 = full duplex. |

`mock_w5500_reset()` zeroes all memory and counters, then restores
`cr_stuck`, lock counters and `elapsed_ms`, forces `connect_ok = 1`, and seeds
post-reset chip defaults: `VERSIONR = 0x04`, `PHYCFGR = 0x01`, `RTR = 0x07D0`,
`RCR = 8`.

## Socket command side effects (`mock_handle_cmd`)

| Command | Behavior |
|---------|----------|
| `OPEN` | Sets `Sn_SR` to `INIT` (TCP) or `UDP` (UDP); resets TX/RD, TX/WR, RX/RD, RX/WR pointers |
| `LISTEN` | If `Sn_SR == INIT`, sets `LISTEN`; otherwise sets `CLOSED` + `TIMEOUT` IR |
| `CONNECT` | If `Sn_SR == INIT`: success → `ESTABLISHED` + `CON` IR (when `connect_ok`); failure → `CLOSED` + `TIMEOUT` IR |
| `DISCON` | Sets `CLOSED` + `DISCON` IR |
| `CLOSE` | Sets `CLOSED`; clears entire `Sn_IR` |
| `SEND` | Captures TX data into `last_sent[]`, advances `Sn_TX_RD` to `Sn_TX_WR`, sets `SENDOK` IR. With `send_stall`, accepted but never completes |
| `RECV` | No-op (RX data is injected directly via `mock_w5500_inject_rx()`) |
| Other | Ignored |

Writing `Sn_IR` is write-1-to-clear: `Sn_IR &= ~val`.

## Test helpers

| Function | Purpose |
|----------|---------|
| `mock_w5500_reset()` | Reset emulator to post-reset defaults |
| `mock_w5500_plat(&plat)` | Fill platform hooks (byte-loop path, lock counters) |
| `mock_w5500_plat_burst(&plat)` | Same, but with burst transfer enabled |
| `mock_w5500_inject_rx(sock, data, len)` | Deposit bytes into socket RX buffer and set `RECV` IR |
| `mock_w5500_remote_connect(sock, peer_ip, peer_port)` | Set `DIPR`/`DPORT`, `Sn_SR = ESTABLISHED`, set `CON` IR |
| `mock_w5500_remote_disconnect(sock)` | Set `Sn_SR = CLOSE_WAIT`, set `DISCON` IR |
| `mock_w5500_complete_send(sock)` | Finish a stalled SEND: capture TX data, advance `TX_RD`, set `SENDOK` |
| `mock_w5500_reg(sock, offset)` | Read an emulated socket register byte (bypasses SPI) |

`mock_w5500_plat()` also wires `lock`/`unlock` counters (`lock_count`,
`unlock_count`) and `delay_ms` (adds to `elapsed_ms`).

## Writing new tests against the emulator

1. Call `mock_w5500_reset()` before each test case (done by `fixture_open()`).
2. Build the platform with `mock_w5500_plat()` or `_burst()`.
3. Open a driver handle with `w5500_init()` / `w5500_init_static()`.
4. Drive the chip through the public API; assert on `g_mock.*` fields directly
   for register-level expectations.
5. Use `mock_w5500_inject_rx()` to simulate received data and
   `mock_w5500_remote_connect()` / `mock_w5500_remote_disconnect()` to
   simulate peer events.
6. Call `w5500_poll()` to dispatch events; check `g_evt[]` log or
   `g_mock.lock_count == g_mock.unlock_count` for lock balance.

The emulator is single-threaded and not ISR-safe — it models the chip, not the
MCU. Do not call driver functions from inside a mock callback.