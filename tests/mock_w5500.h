/**
 * @file mock_w5500.h
 * @brief Host-side W5500 emulator for unit tests (SPI state machine +
 *        register/buffer model + socket command side effects).
 */
#ifndef MOCK_W5500_H
#define MOCK_W5500_H

#include <stdint.h>
#include "w5500.h"

#define MOCK_MAX_SOCKS      4u
#define MOCK_MAX_BUF_BYTES  (16u * 1024u)
#define MOCK_CAPTURE_MAX    2048u

/** Emulator state, inspectable by tests. */
typedef struct {
    /* raw memory images */
    uint8_t  common[0x40];
    uint8_t  sreg[MOCK_MAX_SOCKS][0x30];
    uint8_t  tx_mem[MOCK_MAX_SOCKS][MOCK_MAX_BUF_BYTES];
    uint8_t  rx_mem[MOCK_MAX_SOCKS][MOCK_MAX_BUF_BYTES];

    /* captured data of the last SEND per socket */
    uint8_t  last_sent[MOCK_MAX_SOCKS][MOCK_CAPTURE_MAX];
    uint16_t last_sent_len[MOCK_MAX_SOCKS];

    /* SPI parser */
    uint8_t  in_cs;
    uint8_t  phase;      /* 0=addr_hi 1=addr_lo 2=ctrl 3=data */
    uint16_t addr;
    uint8_t  bsb;
    uint8_t  rwb;

    /* behavior knobs */
    uint8_t  cr_stuck;   /* 1: Sn_CR never self-clears (bus hang model) */
    uint8_t  connect_ok; /* 1: CONNECT succeeds instantly (default) */
    uint8_t  send_stall; /* 1: SEND accepted but never completes */
    uint8_t  phycfgr;

    /* stats */
    uint32_t lock_count;
    uint32_t unlock_count;
    uint32_t elapsed_ms;
} mock_w5500_t;

extern mock_w5500_t g_mock;

/** Reset the emulator to post-reset chip defaults. */
void mock_w5500_reset(void);

/** Platform interface bound to the emulator (lock counters included). */
void mock_w5500_plat(w5500_plat_if_t *plat);

/** Same as mock_w5500_plat(), but with spi_transfer_burst enabled. */
void mock_w5500_plat_burst(w5500_plat_if_t *plat);

/** Put received data into socket RX buffer and set Sn_IR.RECV. */
void mock_w5500_inject_rx(uint8_t sock, const uint8_t *data, uint16_t len);

/** Make the chip behave as if a TCP client connected to our socket. */
void mock_w5500_remote_connect(uint8_t sock, const uint8_t peer_ip[4],
                               uint16_t peer_port);

/** Make the chip behave as if the peer sent FIN (Sn_IR.DISCON). */
void mock_w5500_remote_disconnect(uint8_t sock);

/** Complete a previously stalled SEND (capture TX data, set SEND_OK). */
void mock_w5500_complete_send(uint8_t sock);

/** Read an emulated register byte (bypasses SPI; for assertions). */
uint8_t mock_w5500_reg(uint8_t sock, uint8_t offset);

#endif /* MOCK_W5500_H */
