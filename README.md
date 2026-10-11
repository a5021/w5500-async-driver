# w5500-async-driver

A lightweight, asynchronous **C99** driver for the **Wiznet W5500** hardware
network controller. One repository for every development stage (see
[docs/](docs/README.md)).

## Features

- **Three-layer architecture**: a platform interface (function-pointer HAL),
  a protocol core in plain C with no vendor headers, and an event-driven
  socket API.
- **Multi-context**: all chip state lives in a context (`w5500_handle_t`);
  there are no driver globals, so several W5500 devices can share one image.
- **Non-blocking API**: `connect`/`send`/`recv` return immediately and
  progress arrives through callbacks (`CONNECTED`, `DATA_RECEIVED`,
  `DISCONNECTED`, `SEND_DONE`, `ERROR`) from `w5500_poll()`.
- **Zero-copy**: receive data goes straight from the chip RX buffer into the
  application buffer, transmit data straight from the user buffer into chip
  TX memory, with no intermediate copies.
- **Timeouts instead of endless polling**: every register wait is bounded and
  returns `W5500_ERR_TIMEOUT` instead of hanging the MCU.
- **Thread safety**: optional `lock`/`unlock` hooks in the platform
  interface (a FreeRTOS port is included).
- **No heap in the socket layer**: hardware sockets are allocated from an
  array inside the context. The context itself can be static
  (`w5500_init_static`) or heap-allocated (`w5500_init`).
- **Bare-metal STM32 port (CMSIS)**: SPI/GPIO register access without HAL,
  DWT-based delays, pin mapping through macros.
- **Interrupt-friendly polling**: `w5500_irq_pending()` snapshots IR/SIR for
  INTn-driven designs, and `w5500_poll()` reads only sockets whose summary
  bit is set.
- **Link events**: `w5500_get_link_info()` (LNK/SPD/DPX) plus
  `w5500_set_link_cb()` for link-up, link-down and speed/duplex changes.
- **Socket options**: `w5500_socket_open_ex()` with TCP no-delay and UDP
  multicast/broadcast-block flags; TTL/TOS/MSS access, status read-back,
  a manual keep-alive probe, and `W5500_ERR_BUSY` when a send is in flight.
- **Chip tuning**: `w5500_set_retransmit()` (RTR/RCR), `w5500_set_ping_block()`,
  and an optional `delay_us` platform hook for 100 us command polling.
- **DHCP client** (`w5500_dhcp.h`): DISCOVER/OFFER/REQUEST/ACK, renew at 50%
  and re-acquire at 100% of the lease, no heap, tick driven; the lease is
  applied to the chip before the callback.
- **DNS resolver** (`w5500_dns.h`): A queries, compression-pointer-safe
  parsing, retries and timeouts, a separate layer over a regular UDP socket.
- **SNTP client** (`w5500_sntp.h`): mode-3 requests, Unix time from the
  transmit timestamp, retries and timeout.
- **Multicast**: `W5500_SOCK_F_MULTI` and `W5500_SOCK_F_IGMP_V2`
  (Sn_MR.MULTI/MC); asymmetric TX/RX buffers through
  `w5500_set_socket_buffers_ex()`.
- **Modbus TCP** (`w5500_modbus.h`): server (FC 3/4/6/16, up to 4 clients,
  register map through callbacks) and a polling client; the codec is fuzzed.

## Repository layout

```
├── include/              # Public API headers
│   ├── w5500.h           # Core: chip, network, timeouts, raw access
│   ├── w5500_sockets.h   # Event-driven socket API
│   ├── w5500_dhcp.h      # DHCP client (separate layer)
│   ├── w5500_dns.h       # DNS resolver (separate layer)
│   ├── w5500_sntp.h      # SNTP client (separate layer)
│   └── w5500_modbus.h    # Modbus TCP: server and client
├── src/                  # Core implementation (plain C99)
│   ├── w5500.c
│   ├── w5500_sockets.c
│   ├── w5500_dhcp.c
│   ├── w5500_dns.c
│   ├── w5500_sntp.c
│   ├── w5500_modbus.c
│   ├── w5500_internal.h  # Internal context (hidden from users)
│   └── w5500_registers.h # W5500 register map
├── ports/
│   ├── stm32/            # Bare-metal CMSIS port (SPI/GPIO/DWT)
│   └── freertos/         # Mutex hooks for multi-tasking
├── examples/
│   ├── stm32f4_baremetal/  # TCP client without an OS
│   ├── stm32f4_freertos/   # TCP echo server under FreeRTOS
│   ├── stm32f4_dhcp_dns/   # DHCP -> DNS -> TCP without an OS
│   └── stm32f4_modbus_server/ # Modbus TCP server (FC 3/4/6/16)
├── tests/                # Unit tests + W5500 emulator + fuzz harnesses
│   └── fuzz/             # 13 harnesses, corpus, dictionary (docs/fuzzing.md)
├── docs/                 # Architecture, API, protocols, fuzzing, stages
└── Makefile              # Host build for the core, tests and fuzzing
```

## Quick start

```c
#include <string.h>

#include "w5500.h"
#include "w5500_sockets.h"
#include "w5500_port_stm32.h"

static w5500_handle_t  nic;
static w5500_socket_t  sock;

static const uint8_t mac[6]  = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t ip[4]   = {192, 168, 1, 42};
static const uint8_t mask[4] = {255, 255, 255, 0};
static const uint8_t gw[4]   = {192, 168, 1, 1};
static const uint8_t srv[4]  = {192, 168, 1, 10};

static void on_event(w5500_socket_t s, w5500_event_t ev,
                     w5500_result_t status, void *arg)
{
    uint8_t buf[128];
    uint16_t n;

    (void)arg;
    if (ev == W5500_EVENT_CONNECTED) {
        (void)w5500_socket_send(s, (const uint8_t *)"hello", 5, &n);
    } else if (ev == W5500_EVENT_DATA_RECEIVED) {
        while (w5500_socket_recv(s, buf, sizeof buf, &n) == W5500_OK && n) {
            /* process n bytes */
        }
    } else if (ev == W5500_EVENT_DISCONNECTED || ev == W5500_EVENT_ERROR) {
        (void)status; /* the handle was released by the driver */
    }
}

int main(void)
{
    w5500_plat_if_t plat;

    w5500_port_stm32_init();
    memset(&plat, 0, sizeof plat);
    w5500_port_stm32_fill_plat(&plat); /* lock/unlock stay NULL (bare metal) */

    if (w5500_init(&nic, &plat) != W5500_OK) {
        for (;;) { }
    }
    if (w5500_check(nic) != W5500_OK) { /* wiring or SPI problem */
        for (;;) { }
    }

    (void)w5500_set_mac(nic, mac);
    (void)w5500_set_ip(nic, ip);
    (void)w5500_set_subnet_mask(nic, mask);
    (void)w5500_set_gateway(nic, gw);

    w5500_socket_open(nic, W5500_PROTO_TCP, 5000, on_event, NULL, &sock);
    w5500_socket_connect(sock, srv, 5000);

    for (;;) {
        w5500_poll(nic);                    /* call periodically */
        w5500_port_stm32_delay_ms(10);
    }
}
```

Full examples: [examples/stm32f4_baremetal](examples/stm32f4_baremetal/main.c),
[examples/stm32f4_freertos](examples/stm32f4_freertos/main.c),
[examples/stm32f4_dhcp_dns](examples/stm32f4_dhcp_dns/main.c),
[examples/stm32f4_modbus_server](examples/stm32f4_modbus_server/main.c).
Firmware examples are syntax-checked by CI through `make examples`
(`clang -fsyntax-only`), because the STM32 port needs no vendor headers.
See [examples/README.md](examples/README.md) for a per-example overview.

## DHCP and DNS quick start

The full "get a lease -> resolve a name -> open TCP" chain lives in
[examples/stm32f4_dhcp_dns](examples/stm32f4_dhcp_dns/main.c). Skeleton:

```c
static uint8_t dhcp_mem[2048], dns_mem[2048];
static w5500_dhcp_t *dhcp;
static w5500_dns_t  *dns;
static uint8_t dns_active;

static void on_dhcp(w5500_dhcp_t *d, w5500_dhcp_event_t ev, void *arg)
{
    uint8_t dns_srv[4];

    if (ev == W5500_DHCP_EVENT_FAILED) {          /* no server answered */
        (void)w5500_dhcp_start(d, now_ms());
        return;
    }
    /* The lease is already applied to the chip: SIPR/SUBR/GAR are set. */
    (void)w5500_dhcp_get_config(d, NULL, NULL, NULL, dns_srv);
    (void)w5500_dns_set_server(dns, dns_srv);
    dns_active = (w5500_dns_start(dns, "example.com", now_ms()) == W5500_OK);
}

static void on_dns(w5500_dns_t *d, const uint8_t ip[4],
                   w5500_result_t status, void *arg)
{
    dns_active = 0;
    if (status == W5500_OK) {
        /* ip resolved: open a socket and connect */
    }
}

w5500_dhcp_init(&dhcp, dhcp_mem, sizeof dhcp_mem, h, mac, on_dhcp, NULL);
w5500_dns_init(&dns, dns_mem, sizeof dns_mem, h,
               (const uint8_t[4]){0, 0, 0, 0}, on_dns, NULL);
w5500_dhcp_start(dhcp, now_ms());

for (;;) {
    w5500_poll(h);                   /* socket events */
    w5500_dhcp_poll(dhcp, now_ms()); /* renew and retransmits */
    if (dns_active) {
        w5500_dns_poll(dns, now_ms()); /* only while a query is in flight */
    }
}
```

`w5500_poll()`, `w5500_dhcp_poll()` and `w5500_dns_poll()` do not block and
must be called from one task (or under one mutex). DHCP applies the lease
before invoking the callback, so sockets may be opened from inside it.
More details: [docs/protocols.md](docs/protocols.md).

## Building and testing on the host

The core builds with any C99 compiler; tests run against a software W5500
emulator (no hardware required):

```sh
make test      # three unit-test binaries (-Wall -Wextra -std=c99 -Werror)
make examples  # syntax-check the firmware examples
make lib       # build/libw5500.a
make fuzz-all  # 13 fuzz harnesses (Clang + libFuzzer + ASan/UBSan)
```

Fuzzing details: [docs/fuzzing.md](docs/fuzzing.md).
`ports/stm32` and `ports/freertos` are not part of the host build: they need
the CMSIS device headers and FreeRTOS headers respectively ([ports/README.md](ports/README.md)).

## Integration into firmware

1. Add `include/` and `src/*.c` to your project.
2. Include `ports/stm32/w5500_port_stm32.c` (STM32F4, CMSIS) or write your
   own port that implements `w5500_plat_if_t`.
3. Default pins: SPI1 - PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI, SPI mode 0,
   divider BR=3 (fPCLK/16). Override with `W5500_STM32_*` macros (see
   [ports/stm32/w5500_port_stm32.h](ports/stm32/w5500_port_stm32.h)).
4. With an RTOS: call `w5500_rtos_lock_init()` and `w5500_rtos_bind_plat(&plat)`,
   then call `w5500_poll()` from a single network task.
5. For DMA: after `w5500_port_stm32_fill_plat()`, replace
   `plat.spi_transfer_burst` with your own DMA implementation.

## Link recovery

`w5500_poll()` samples PHYCFGR when nothing else interrupts and calls
`w5500_set_link_cb()` (the callback runs outside the driver lock):

```c
static void on_link(w5500_handle_t h, const w5500_link_info_t *info,
                    void *user)
{
    app_t *app = user;

    if (!info->up) {
        app->net_up = false;   /* link lost: sockets will fail on their own */
        return;
    }
    if (!app->net_up) {
        app->net_up = true;
        /* Link is back: old TCP sockets already reported DISCONNECTED/ERROR
           and were released, so the service restarts from a clean slate. */
        app_restart_service(h);
    }
}

w5500_set_link_cb(h, on_link, app);
```

`w5500_link_info_t` also reports the negotiated speed (`speed_100`) and
duplex (`full_duplex`).

## Error codes

| Code | Meaning | Typical cause |
|------|---------|---------------|
| `W5500_OK` | success | - |
| `W5500_ERR_PARAM` | invalid argument | NULL, `len == 0`, unknown flag, illegal buffer size |
| `W5500_ERR_TIMEOUT` | chip did not answer in time | Sn_CR did not self-clear, Sn_SR did not reach the expected value |
| `W5500_ERR_SPI` | platform failure | `spi_select`/`deselect`/`transfer_byte` not set |
| `W5500_ERR_STATE` | operation invalid in this state | `send` on UDP, keep-alive probe not on ESTABLISHED |
| `W5500_ERR_NOMEM` | no free slot | all four sockets are occupied |
| `W5500_ERR_CHIP` | wrong chip on the bus | VERSIONR != 0x04 or RTR != 0x07D0 |
| `W5500_ERR_BUSY` | resource busy | another `send` before `SEND_DONE` |

## Documentation

- [docs/architecture.md](docs/architecture.md) - layers, execution model,
  locking, polling and timeout semantics, design invariants.
- [docs/api.md](docs/api.md) - API reference by module.
- [docs/protocols.md](docs/protocols.md) - DHCP, DNS, SNTP and Modbus TCP:
  lifecycle, timing, storage and integration recipes.
- [docs/fuzzing.md](docs/fuzzing.md) - harnesses, corpus, run modes, CI.
- [docs/README.md](docs/README.md) - stage tracker and document index.
- [examples/README.md](examples/README.md) - what each example demonstrates.
- [ports/README.md](ports/README.md) - platform ports and their contracts.
- [CONTRIBUTING.md](CONTRIBUTING.md) - build, test and contribution workflow.

## Project stages

| Stage | Status | Description |
|-------|--------|-------------|
| 1 | done | Core + async sockets + STM32/FreeRTOS ports + tests |
| 2 | done | Fuzzing: 7 harnesses, corpus/dictionary, CI |
| 3 | done | Link events, INTn/SIR polling, `open_ex` flags, `ERR_BUSY`, RTR/RCR, `delay_us` |
| 4 | done | Per-socket buffers, zero-length UDP, link-recovery example |
| 5 | done | DHCP/DNS as a separate layer |
| 6 | done | Modbus TCP server/client + fuzzing |

See [docs/README.md](docs/README.md).
Stage 1 specification: [docs/stages/stage1.md](docs/stages/stage1.md).

## License

MIT - see [LICENSE](LICENSE).
