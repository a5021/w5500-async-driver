# Host-side W5500 emulator (`tests/mock_w5500.*`)

The emulator implements a chip-level model of the W5500: the SPI frame
decoder, the common/register/buffer memory images, and socket command side
effects (OPEN, LISTEN, CONNECT, DISCON, CLOSE, SEND, RECV). It is used by
the unit tests and all fuzz harnesses — no real silicon is required.

## Memory model

| Region | Size | Purpose |
|--------|------|---------|
| `common[0x40]` | 64 B | Common registers (VERSIONR, MR, IR, PHYCFGR, RTR, RCR, SIR, …) |
| `sreg[4][0x30]` | 4×48 B | Per-socket registers (Sn_MR, Sn_CR, Sn_IR, Sn_SR, …) |
| `tx_mem[4][16 KiB]` | 64 KiB | TX buffer memory for sockets 0..3 |
| `rx_mem[4][16 KiB]` | 64 KiB | RX buffer memory for sockets 0..3 |

`tx_mem`/`rx_mem` are indexed with `addr & (size - 1)`, so writes that wrap
the buffer edge behave like the real chip's circular buffers.

## SPI frame decoder

The emulator is driven byte-by-byte through `mock_w5500_plat()`:

| Phase | Byte meaning | Effect |
|-------|--------------|--------|
| 0 | Address high | `addr = byte << 8` |
| 1 | Address low | `addr = addr \| byte` |
| 2 | Control byte | `bsb = (byte >> 3) & 0x1F`, `rwb = (byte >> 2) & 1` |
| 3 | Data | read/write at `addr`, then `addr++` |

Deselect (CS high) resets the phase to 0. An idle bus (CS high, any byte)
returns `0xFF`. Reads return the current register/buffer value; writes
apply socket command side effects where relevant (Sn_CR commands execute,
Sn_IR is write-1-to-clear, Sn_TX_FSR/Sn_RX_RSR are computed reads).

## Behavior knobs

All knobs are `uint8_t` on `g_mock` and default to 0 unless noted:

| Knob | Meaning |
|------|---------|
| `cr_stuck` | Sn_CR never self-clears (simulates a hung chip); OPEN/LISTEN/CONNECT/etc. block until timeout |
| `connect_ok` | CONNECT succeeds instantly when 1 (default); fails with timeout when 0 |
| `send_stall` | SEND accepted but never completes (no SEND_OK) |
| `phycfgr` | Value returned for PHYCFGR reads (LNK/SPD/DPX bits) |

Knobs are preserved across `mock_w5500_reset()` except `connect_ok`, which
is forced to 1. The fuzz fixture `fuzz_open_handle()` zeroes all knobs
before each input so they never leak between fuzz cases.

## SPI transaction counters

`bus_reads` counts every SPI read transaction observed by the emulator.
This is used by `tests/test_w5500.c` (`test_poll_read_budget`) to assert
that `w5500_poll()` does not read `Sn_SR` unnecessarily for UDP sockets.

## Mock helpers for tests

| Helper | Purpose |
|--------|---------|
| `mock_w5500_reset()` | Reset emulator to post-reset defaults (knobs preserved) |
| `mock_w5500_plat(&plat)` | Fill a `w5500_plat_if_t` bound to the emulator (byte-loop path) |
| `mock_w5500_plat_burst(&plat)` | Same but with `spi_transfer_burst` enabled |
| `mock_w5500_inject_rx(sock, data, len)` | Deposit bytes into socket RX and set `Sn_IR.RECV` |
| `mock_w5500_remote_connect(sock, ip, port)` | Simulate a TCP peer connecting; sets ESTABLISHED + CON |
| `mock_w5500_remote_disconnect(sock)` | Simulate peer FIN; sets CLOSE_WAIT + DISCON |
| `mock_w5500_complete_send(sock)` | Complete a stalled SEND: capture TX data, advance TX_RD, set SEND_OK |
| `mock_w5500_reg(sock, offset)` | Read a socket register directly (bypasses SPI) |

## Using the emulator in new tests

1. Call `mock_w5500_reset()` then `mock_w5500_plat(&plat)` to build the
   platform interface.
2. `w5500_init(&h, &plat)` and `w5500_set_timeout(h, 50u)` for fast
   failure injection.
3. Drive chip behavior with `mock_w5500_inject_rx()`,
   `mock_w5500_remote_connect()` / `mock_w5500_remote_disconnect()`,
   or by poking `g_mock.sreg[id][offset]` directly.
4. Pump the driver with `w5500_poll(h)` and assert with `CHECK`/`CHECK_EQ`.
5. Verify lock balance with `CHECK_EQ(g_mock.lock_count, g_mock.unlock_count)`.

## Knobs used by fuzz harnesses

- `fuzz_socket_lifecycle`: `cr_stuck` bit flags that OPEN may time out.
- `fuzz_send_recv`: `cr_stuck` and `send_stall` set after setup.
- `fuzz_poll`: `cr_stuck` applied after socket setup.
- `fuzz_reg_access`: `cr_stuck` affects Sn_CR command completion.
- `fuzz_net_config`: never sets knobs; relies on pristine state from
  `fuzz_open_handle()`.

## Limitations

- Only one socket can be injected into at a time via `inject_rx`.
- `last_sent` capture is limited to `MOCK_CAPTURE_MAX` (2048) bytes per
  socket.
- The emulator does not model ARP, ICMP, or electrical behavior; use the
  `stm32f4_*` examples for real hardware validation.
