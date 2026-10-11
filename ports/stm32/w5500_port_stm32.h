/**
 * @file w5500_port_stm32.h
 * @brief Bare-metal CMSIS port for STM32F4: SPI + GPIO + DWT delay.
 *
 * Replaces the STM32 HAL with direct register access through the vendor
 * CMSIS device header (e.g. stm32f4xx.h). The device header must be on
 * the include path of your project (CMSIS pack / CubeMX output).
 *
 * All pin/peripheral selections are macros with sensible defaults
 * (SPI1 on PA4..PA7) and can be overridden either with -D flags or by
 * defining W5500_PORT_STM32_USER_CONFIG to your own config header.
 */
#ifndef W5500_PORT_STM32_H
#define W5500_PORT_STM32_H

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- peripheral / pin selection (override as needed) --------------- */

#ifndef W5500_STM32_SPI
#define W5500_STM32_SPI             SPI1
#endif
#ifndef W5500_STM32_SPI_RCC_INIT
#define W5500_STM32_SPI_RCC_INIT    do { RCC->APB2ENR |= RCC_APB2ENR_SPI1EN; } while (0)
#endif
#ifndef W5500_STM32_GPIO_RCC_INIT
#define W5500_STM32_GPIO_RCC_INIT   do { RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; } while (0)
#endif

#ifndef W5500_STM32_GPIO_PORT_SCK
#define W5500_STM32_GPIO_PORT_SCK   GPIOA
#endif
#ifndef W5500_STM32_GPIO_PIN_SCK
#define W5500_STM32_GPIO_PIN_SCK    5u   /* PA5, AF5 */
#endif
#ifndef W5500_STM32_GPIO_PORT_MISO
#define W5500_STM32_GPIO_PORT_MISO  GPIOA
#endif
#ifndef W5500_STM32_GPIO_PIN_MISO
#define W5500_STM32_GPIO_PIN_MISO   6u   /* PA6, AF5 */
#endif
#ifndef W5500_STM32_GPIO_PORT_MOSI
#define W5500_STM32_GPIO_PORT_MOSI  GPIOA
#endif
#ifndef W5500_STM32_GPIO_PIN_MOSI
#define W5500_STM32_GPIO_PIN_MOSI   7u   /* PA7, AF5 */
#endif
#ifndef W5500_STM32_GPIO_PORT_CS
#define W5500_STM32_GPIO_PORT_CS    GPIOA
#endif
#ifndef W5500_STM32_GPIO_PIN_CS
#define W5500_STM32_GPIO_PIN_CS     4u   /* PA4, software CS */
#endif

/** SPI baud-rate divider in CR1.BR (0..7 => fPCLK/2 .. fPCLK/256). */
#ifndef W5500_STM32_SPI_BR_DIV
#define W5500_STM32_SPI_BR_DIV      3u   /* fPCLK/16 */
#endif

/** Alternate-function number for the SPI pins (SPI1 => 5). */
#ifndef W5500_STM32_SPI_AF
#define W5500_STM32_SPI_AF          5u
#endif

/**
 * @brief Enable clocks, configure SPI in mode 0 (CPOL=0, CPHA=0, 8-bit,
 *        MSB first, master, software NSS) and drive CS high.
 *
 * Call once before w5500_init(). SystemCoreClock must be valid (it is
 * provided by the CMSIS device startup file).
 */
void w5500_port_stm32_init(void);

/** Platform callbacks for w5500_plat_if_t (lock/unlock left untouched). */
void w5500_port_stm32_fill_plat(w5500_plat_if_t *plat);

/** DWT cycle-counter based blocking delay; usable standalone. */
void w5500_port_stm32_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* W5500_PORT_STM32_H */
