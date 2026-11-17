# Fuzzing the W5500 driver

Sixteen libFuzzer harnesses run against the chip emulator
(`tests/mock_w5500.c`) with ASan/UBSan. Every property is checked with
`abort()`, so libFuzzer stops and stores the failing input as a normal crash.

## Harnesses

| Target | What it fuzzes | Key invariants |
|--------|----------------|----------------|
| `fuzz-reg-access` | `w5500_read_regs`/`write_regs` with arbitrary `addr/bsb/len` | round-trip with the emulator's documented exceptions (MR, Sn_CR, Sn_IR, FSR/RSR, kind-0), `len == 0 -> ERR_PARAM` |
| `fuzz-socket-lifecycle` | OPEN/CLOSE/LISTEN/CONNECT/... scripts over four slots | `in_use` <-> `Sn_SR` balance, NOMEM on slot exhaustion, consistency after every call |
| `fuzz-poll` | `w5500_poll()` with IR/SR/PHYCFGR pokes, RX injections and re-entrant callbacks | callback-side slot mirror, safe re-entry from callbacks, `SIR` equals OR of `Sn_IR`, link callback fires only on a real change, lock balance |
| `fuzz-send-recv` | `send/sendto/recv/rx_available/tx_free/get_peer` and NULL probes | `*sent <= len`, timeout or `ERR_BUSY` => `*sent == 0`, `*recvd <= pending`, RX_RSR matches the injection bookkeeping |
| `fuzz-net-config` | network setters/getters, SETBUF matrix, timeout, link, SW_RESET, NULL probes | raw register image equals the API view, neighbouring bytes untouched, `rc == OK <=> kb in {1,2,4}` |
| `fuzz-context` | `init`/`init_static`/`deinit` with a 0xA5 canary | nothing is written when `mem_size < context_size`; the canary survives `deinit` |
| `fuzz-spi-frame` | `w5500_ctrl_byte`, the emulator frame decoder, full write->read frames | exact `(BSB<<3)\|(RWB<<2)\|OM` layout, select->addr->ctrl->data->deselect framing |
| `fuzz-dhcp` | DHCP reply decoder (`w5500_dhcp_parse_packet`) on arbitrary bytes | no OOB, option and cookie bounds, message type != 0 on success, lease inside the clamp range, deterministic |
| `fuzz-dns` | DNS response codecs: legacy first-A decoder and CNAME-aware multi-A decoder (`w5500_dns_parse_response_ex`) with compression pointers and a caller-supplied name | no OOB and no pointer loops, output untouched on failure, a matching id required, deterministic multi-A decode |
| `fuzz-dhcp-state` | a live DHCP client: injections, clock steps, stop/start | BOUND/RENEWED implies the lease is applied to the chip, FAILED implies a closed socket, poll after stop returns `ERR_STATE`, lock balance |
| `fuzz-dns-state` | a live DNS resolver: replies, clock steps, re-queries | exactly one callback per query, idle state inside the callback, callbacks never outnumber starts |
| `fuzz-sntp` | SNTP decoder (`w5500_sntp_parse_response`) on arbitrary bytes | no OOB, server mode only, zero timestamp rejected, output untouched on failure, deterministic |
| `fuzz-sntp-state` | a live SNTP client: replies, clock steps, stop/re-request | documented statuses only, poll after stop returns `ERR_STATE`, callbacks never outnumber starts, lock balance |
| `fuzz-modbus` | Modbus TCP codecs: request (`w5500_mb_parse_adu`) and response (`w5500_mb_parse_response`) | no OOB, MBAP length consistency, count/byte-count rules for FC3/4/16, txid must match, deterministic |
| `fuzz-mqtt` | MQTT 3.1.1 codec: remaining-length decoder, parser and builders | no OOB, header length 2..5, `consumed` inside the input, topic/payload alias the input, deterministic, builders reject short buffers and round-trip |
| `fuzz-mqtt-state` | a live MQTT client: broker packets, clock steps, publish/subscribe, stop/reconnect | documented statuses only, callbacks carry topic/payload only for PUBLISH, lock balance |

Shared fixtures: `tests/fuzz/fuzz_common.h` (emulator reset per input, a
fixed 4 ms timeout - the timeout itself is **not** fuzzed so an input cannot
hang inside a wait loop), `tests/fuzz/fuzz_runner.c` - a standalone runner
for builds without libFuzzer.

Driver internals are visible to the harnesses through
`-DW5500_TEST_HARNESS` (`w5500_ctrl_byte`, context slots).

## Running

```sh
make fuzz-all                    # all 16 targets, libFuzzer + ASan/UBSan, 120 s
make fuzz-reg-access             # one target
FUZZ_MODE=asan make fuzz-all     # standalone runner + ASan/UBSan (no libFuzzer)
FUZZ_TIME=60 make fuzz-all       # seconds per target
FUZZ_MODE=none make fuzz-reg-access # plain build, replay the checked-in corpus
```

`FUZZ_MODE=fuzzer` (default) needs Clang with libFuzzer. On Windows the
libFuzzer link step needs an MSVC STL; without it use `FUZZ_MODE=asan`,
which runs every target once over the corpus (regression mode), while the
authoritative deep runs happen in CI on Ubuntu.

## Corpus and dictionary

- `tests/fuzz/corpus/*.bin` - seed inputs (zero/ff patterns, register and SPI
  frames, API scripts, DHCP offer, DHCP/ DNS state scripts, DNS answer,
  SNTP reply, Modbus request/reply, and the regression seed found by CI).
- `tests/fuzz/w5500.dict` - command dictionary: BSB/RWB control-byte
  encodings, register addresses, Sn_CR commands, Sn_IR bits, buffer sizes,
  IP/MAC/port spikes.
- Fuzzing writes into `build/fuzz/corpus/<target>/`, never into the checkout.

## CI

`.github/workflows/ci.yml`:

- **host-tests** - `make test` (GCC) and `make examples` (syntax check);
- **fuzz** - `make FUZZ_CC=clang FUZZ_TIME=5 fuzz-all` (libFuzzer), with an
  apt package cache and a `build/fuzz/corpus` cache (per-commit key plus a
  restore-key prefix) so coverage accumulates between runs.

`.github/workflows/nightly.yml` (schedule and manual dispatch) runs the same
`fuzz-all` with `FUZZ_TIME=300`, reuses the corpus cache, uploads the grown
corpus as an artifact and, on failure, the `crash-*` artifacts.

## Deliberately not fuzzed

- **Double `w5500_deinit()`** - a documented use-after-free.
- **The timeout value** - large values turn wait loops into extremely long
  inputs; the harnesses pin it to 4 ms instead.
- **Real SPI** - covered by the emulator; `fuzz-spi-frame` checks the frame
  format, not bus electronics.
