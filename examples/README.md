# Examples

| Example | Demonstrates | Checked in CI |
|---------|--------------|---------------|
| [stm32f4_baremetal](stm32f4_baremetal/main.c) | Static IP configuration, TCP client, reconnect after `DISCONNECTED`, zero-copy receive loop | yes (`make examples`) |
| [stm32f4_freertos](stm32f4_freertos/main.c) | TCP echo server under FreeRTOS, RTOS mutex hooks, one network task owning `w5500_poll()` | no (needs FreeRTOS headers) |
| [stm32f4_dhcp_dns](stm32f4_dhcp_dns/main.c) | DHCP lease -> DNS resolve -> TCP request, static protocol storage, link-loss recovery | yes (`make examples`) |
| [stm32f4_modbus_server](stm32f4_modbus_server/main.c) | Modbus TCP server (FC 3/4/6/16), callback register map, live values in FC 4, restarts on link-up | yes (`make examples`) |

## Building

The firmware examples are not built by the host Makefile: they need the CMSIS
device headers and a startup file from your firmware project. `make examples`
runs `clang -fsyntax-only` over the three CMSIS-free examples, which is what
CI checks; the FreeRTOS example additionally needs the FreeRTOS headers.

To use one in a project:

1. Add `include/`, the needed `src/*.c` files and `ports/stm32/w5500_port_stm32.c`.
2. Copy the example's `main.c` body into your application.
3. Adjust the `W5500_STM32_*` macros if your wiring differs from
   SPI1: PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI.
4. Call `w5500_poll()` (and the protocol polls) from a single loop or task.

## Common pitfalls

- `w5500_poll()` must run regularly; callbacks and link events only fire from
  there.
- Protocol instances need caller storage (`w5500_dhcp_size()`, ...). Keep it
  `static` or part of your application object; never on a transient stack.
- A socket handle is invalid after `DISCONNECTED`/`ERROR`; reopen from the
  callback instead of reusing it.
- Only one `send` may be in flight per socket; send the next chunk from
  `SEND_DONE`, not immediately after the first call.
