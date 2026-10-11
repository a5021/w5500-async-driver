# Technical specification (Stage 1): asynchronous W5500 driver in plain C

## 1. Project goal

Build an optimized, lightweight and portable library in **C99/C11** for the
**WizNet W5500** hardware network controller. The library must support
multi-tasking environments (RTOS), expose a non-blocking API, support the
zero-copy concept with DMA, and be easy to adapt to different MCU
architectures (the primary target platform is **STM32**).

---

## 2. Architecture requirements and layer structure

The library must be split into three independent execution layers.

### Layer 1: hardware abstraction (HAL / platform interface)

*   **Platform isolation:** this layer must not contain TCP/IP or socket
    logic. Its only job is to drive the physical MCU interfaces.
*   **Callback abstraction:** all MCU-specific wiring is provided through a
    structure of function pointers passed at initialization time:
    ```c
    typedef struct {
        void (*spi_select)(void);
        void (*spi_deselect)(void);
        uint8_t (*spi_transfer_byte)(uint8_t byte);
        void (*spi_transfer_burst)(uint8_t *tx_buf, uint8_t *rx_buf, uint16_t len);
        void (*delay_ms)(uint32_t ms);
        void (*lock)(void);   /* optional RTOS mutex hook */
        void (*unlock)(void); /* optional RTOS mutex hook */
    } w5500_plat_if_t;
    ```

### Layer 2: protocol core (W5500 core)

*   **Plain C:** standard C (C99 or C11) only, no vendor headers
    (`stm32f4xx_hal.h`, `esp_system.h`, ...).
*   **Multi-context:** globals for chip state are forbidden. Every core
    function takes a pointer to the instance context as its first argument:
    ```c
    w5500_result_t w5500_set_ip(w5500_handle_t handle, const uint8_t *ip);
    ```
*   **Register access:** direct W5500 memory access through macros and enums
    describing the chip register map (common registers and socket registers).

### Layer 3: application asynchronous API (socket layer)

*   **Encapsulation (opaque pointers):** socket and context structures are
    hidden inside `.c` files; users only see opaque handles:
    ```c
    typedef struct w5500_context *w5500_handle_t;
    typedef struct w5500_socket  *w5500_socket_t;
    ```
*   **Reactive model (event-driven):** the main network API is built on
    non-blocking calls plus callbacks:
    *   `W5500_EVENT_CONNECTED` - TCP connection established.
    *   `W5500_EVENT_DATA_RECEIVED` - new data in the chip buffer.
    *   `W5500_EVENT_DISCONNECTED` - the peer closed the connection.
*   Every callback receives a `void *user_arg` for application context.

---

## 3. Reliability and performance requirements

1.  **No infinite polling:** every internal wait on chip register readiness
    must be bounded by a software timeout. A stuck SPI bus or a failed chip
    must produce `W5500_ERR_TIMEOUT`, never a hung MCU.
2.  **Thread safety:** SPI transactions and socket allocation critical
    sections inside the core must be protected by the `lock`/`unlock`
    functions supplied by the RTOS through the Layer 1 interface.
3.  **Data path optimization (DMA and zero-copy):**
    *   Layer 1 must expose efficient burst transfers
      (`spi_transfer_burst`) so an MCU DMA controller can be used.
    *   Reads from the W5500 socket RX buffer must go directly into the
      application buffer, bypassing intermediate driver arrays.
4.  **Strict error handling:** every function that performs I/O or resource
    configuration returns `w5500_result_t`, never `void`.

---

## 4. Repository structure requirements

The project must keep a clear directory tree that separates independent
modules:

```
├── include/              # Public API headers
│   ├── w5500.h           # Chip and context management API
│   └── w5500_sockets.h   # Event-driven socket API
├── src/                  # Core library sources
│   ├── w5500.c           # Core and register access implementation
│   ├── w5500_registers.h # Internal W5500 register map
│   └── w5500_sockets.c   # Socket management logic
├── ports/                # Platform and OS ports
│   ├── stm32/            # STM32 SPI/GPIO/DWT port (CMSIS)
│   │   ├── w5500_port_stm32.c
│   │   └── w5500_port_stm32.h
│   └── freertos/         # Synchronization (mutex) implementation
│       └── w5500_port_rtos.c
└── examples/             # Usage examples
    └── stm32f4_freertos/ # STM32 + FreeRTOS demo project
```

Note: the implemented tree grew with later stages (extra protocol modules,
more examples); see the repository root README for the current layout.

---

## 5. Stage 1 acceptance criteria

1.  The core builds with GCC (`-Wall -Wextra -std=c99`) without warnings or
    errors.
2.  The chip version register `VERSIONR` (address `0x0039`) reads back as
    `0x04` through the Layer 1 abstraction on an STM32 bench.
3.  Network parameters (IP, MAC, gateway) and socket buffer memory are
    configured correctly without any driver globals.
