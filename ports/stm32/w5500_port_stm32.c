/**
 * @file w5500_port_stm32.c
 * @brief Bare-metal CMSIS implementation of the W5500 platform interface
 *        for STM32F4 (direct SPI/GPIO register access, DWT delay).
 */
#include <stdint.h>

#if defined(W5500_PORT_STM32_USER_CONFIG)
#include W5500_PORT_STM32_USER_CONFIG
#endif

#include "stm32f4xx.h"

#include "w5500_port_stm32.h"

/* ------------------------------------------------------------------ */
/* GPIO helpers                                                        */
/* ------------------------------------------------------------------ */

static void gpio_config_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    /* Alternate function, push-pull, high speed, no pull. */
    port->MODER = (port->MODER & ~(3u << (2u * pin))) | (1u << (2u * pin));
    port->OTYPER &= ~(1u << pin);
    port->OSPEEDR |= (3u << (2u * pin));
    port->PUPDR &= ~(3u << (2u * pin));

    if (pin < 8u) {
        port->AFR[0] = (port->AFR[0] & ~(0xFu << (4u * pin))) |
                       (af << (4u * pin));
    } else {
        uint32_t idx = pin - 8u;
        port->AFR[1] = (port->AFR[1] & ~(0xFu << (4u * idx))) |
                       (af << (4u * idx));
    }
}

static void gpio_config_out(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3u << (2u * pin))) | (1u << (2u * pin));
    port->OTYPER &= ~(1u << pin);
    port->OSPEEDR |= (3u << (2u * pin));
    port->PUPDR &= ~(3u << (2u * pin));
    port->BSRR = (1u << pin); /* deassert (CS idle high) */
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void w5500_port_stm32_init(void)
{
    W5500_STM32_GPIO_RCC_INIT;
    W5500_STM32_SPI_RCC_INIT;

    gpio_config_af(W5500_STM32_GPIO_PORT_SCK, W5500_STM32_GPIO_PIN_SCK,
                   W5500_STM32_SPI_AF);
    gpio_config_af(W5500_STM32_GPIO_PORT_MISO, W5500_STM32_GPIO_PIN_MISO,
                   W5500_STM32_SPI_AF);
    gpio_config_af(W5500_STM32_GPIO_PORT_MOSI, W5500_STM32_GPIO_PIN_MOSI,
                   W5500_STM32_SPI_AF);
    gpio_config_out(W5500_STM32_GPIO_PORT_CS, W5500_STM32_GPIO_PIN_CS);

    /* Master, software NSS, mode 0 (CPOL=0 CPHA=0), 8-bit, MSB first. */
    W5500_STM32_SPI->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                           ((W5500_STM32_SPI_BR_DIV & 7u) << SPI_CR1_BR_Pos);
    W5500_STM32_SPI->CR2 = 0u;
    W5500_STM32_SPI->CR1 |= SPI_CR1_SPE;

    /* DWT cycle counter for w5500_port_stm32_delay_ms(). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ------------------------------------------------------------------ */
/* Delay                                                               */
/* ------------------------------------------------------------------ */

void w5500_port_stm32_delay_ms(uint32_t ms)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = ms * (SystemCoreClock / 1000u);

    while ((uint32_t)(DWT->CYCCNT - start) < ticks) {
        /* busy wait */
    }
}

/* ------------------------------------------------------------------ */
/* Platform callbacks                                                  */
/* ------------------------------------------------------------------ */

static void plat_spi_select(void)
{
    W5500_STM32_GPIO_PORT_CS->BSRR = (1u << (W5500_STM32_GPIO_PIN_CS + 16u));
}

static void plat_spi_deselect(void)
{
    W5500_STM32_GPIO_PORT_CS->BSRR = (1u << W5500_STM32_GPIO_PIN_CS);
}

static uint8_t plat_spi_transfer_byte(uint8_t byte)
{
    while ((W5500_STM32_SPI->SR & SPI_SR_TXE) == 0u) {
        /* wait for TX buffer empty */
    }
    W5500_STM32_SPI->DR = byte;
    while ((W5500_STM32_SPI->SR & SPI_SR_RXNE) == 0u) {
        /* wait for RX byte */
    }
    return (uint8_t)W5500_STM32_SPI->DR;
}

/*
 * Default burst path is a plain byte loop. For DMA, ignore this hook and
 * assign your own spi_transfer_burst after w5500_port_stm32_fill_plat().
 */
static void plat_spi_transfer_burst(uint8_t *tx_buf, uint8_t *rx_buf,
                                    uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint8_t tx = (tx_buf != NULL) ? tx_buf[i] : 0u;
        uint8_t rx = plat_spi_transfer_byte(tx);
        if (rx_buf != NULL) {
            rx_buf[i] = rx;
        }
    }
}

void w5500_port_stm32_fill_plat(w5500_plat_if_t *plat)
{
    if (plat == NULL) {
        return;
    }
    plat->spi_select = plat_spi_select;
    plat->spi_deselect = plat_spi_deselect;
    plat->spi_transfer_byte = plat_spi_transfer_byte;
    plat->spi_transfer_burst = plat_spi_transfer_burst;
    plat->delay_ms = w5500_port_stm32_delay_ms;
    /* lock/unlock are left as-is: bind FreeRTOS hooks via
     * ports/freertos/w5500_port_rtos.h when an RTOS is used. */
}
