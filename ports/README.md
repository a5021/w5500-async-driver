# Platform ports

A port supplies the `w5500_plat_if_t` hooks. The core never touches MCU
registers directly.

## STM32 bare-metal port (`ports/stm32`)

Files: `w5500_port_stm32.c`, `w5500_port_stm32.h`. Target: STM32F4 with
CMSIS device headers; no HAL.

Initialization:

```c
w5500_port_stm32_init();            /* clocks, SPI1 mode 0, CS high */
w5500_port_stm32_fill_plat(&plat);  /* select/deselect/byte/burst/delays */
```

Defaults (all overridable with `-D` macros or a
`W5500_PORT_STM32_USER_CONFIG` header):

| Macro | Default |
|-------|---------|
| `W5500_STM32_SPI` | `SPI1` |
| `W5500_STM32_SPI_RCC_INIT` / `W5500_STM32_GPIO_RCC_INIT` | enable SPI1 and GPIOA clocks |
| `W5500_STM32_GPIO_PORT_SCK` / `_PIN_SCK` | GPIOA / 5 |
| `W5500_STM32_GPIO_PORT_MISO` / `_PIN_MISO` | GPIOA / 6 |
| `W5500_STM32_GPIO_PORT_MOSI` / `_PIN_MOSI` | GPIOA / 7 |
| `W5500_STM32_GPIO_PORT_CS` / `_PIN_CS` | GPIOA / 4 (software CS) |
| `W5500_STM32_SPI_BR_DIV` | 3 (fPCLK/16) |
| `W5500_STM32_SPI_AF` | 5 |

What `fill_plat()` installs:

- `spi_select` / `spi_deselect` - CS via BSRR.
- `spi_transfer_byte` - polled TXE/RXNE byte exchange.
- `spi_transfer_burst` - a plain byte loop by default. For DMA, overwrite
  `plat.spi_transfer_burst` after `fill_plat()`; either `tx_buf` or `rx_buf`
  may be NULL (TX-only/RX-only), which matches DMA ping-pong usage.
- `delay_ms` / `delay_us` - DWT cycle-counter busy waits
  (`SystemCoreClock / 1000` and `/ 1000000` ticks). `delay_us` enables the
  100 us command polling path; drop it if you prefer 1 ms steps.
- `lock` / `unlock` are deliberately left untouched: bind FreeRTOS hooks or
  leave them NULL for bare metal.

Not provided by the port: INTn pin setup and a hardware reset line. For
INTn, configure the EXTI in your application and have the ISR set a flag;
the task then calls `w5500_poll()` (optionally after `w5500_irq_pending()`).

## FreeRTOS port (`ports/freertos`)

Files: `w5500_port_rtos.c`, `w5500_port_rtos.h`. Synchronization only.

```c
if (w5500_rtos_lock_init() != W5500_OK) { /* out of memory */ }
w5500_port_stm32_fill_plat(&plat);
w5500_rtos_bind_plat(&plat);   /* sets lock/unlock */
w5500_init(&h, &plat);
```

- One global, plain (non-recursive) mutex; `xSemaphoreTake` with
  `portMAX_DELAY`.
- Intended for a single driver instance. With several W5500 chips, provide
  your own lock closures per context.
- Call `w5500_poll()` and all protocol polls from one network task. Callbacks
  run in that task with the mutex released, so they may call the API and may
  block on application primitives.
- Delete the mutex with `w5500_rtos_lock_deinit()` after `w5500_deinit()`.

## Writing a custom port

Required hooks:

| Hook | Contract |
|------|----------|
| `spi_select` | Assert CS (active low) |
| `spi_deselect` | Deassert CS |
| `spi_transfer_byte` | Full-duplex exchange of one byte |
| `delay_ms` | Blocking delay (waits use it) |

Optional hooks:

| Hook | Contract |
|------|----------|
| `spi_transfer_burst` | Transfer `len` bytes; `tx_buf` or `rx_buf` may be NULL |
| `lock` / `unlock` | Must be non-recursive-safe: the driver never nests them |
| `delay_us` | Microsecond delay; enables 100 us command polling |

Rules:

- The driver itself is not ISR-safe. Run `w5500_poll()` and API calls in task
  context; use the INTn pin only to wake that task.
- Hooks must not call back into the driver.
- Keep the platform structure alive for the whole context lifetime (it is
  copied by value, the pointed-to functions must stay valid).
