/**
 * @file w5500_modbus_internal.h
 * @brief Modbus TCP ADU codec shared by the server, client and fuzzers.
 */
#ifndef W5500_MODBUS_INTERNAL_H
#define W5500_MODBUS_INTERNAL_H

#include <stdint.h>

#include "w5500_modbus.h"

/** Largest legal Modbus TCP frame: 7-byte MBAP + 253-byte PDU. */
#define W5500_MB_FRAME_MAX 260u

/** Server listening sockets the library supports (one per client). */
#define MODBUS_MAX_CLIENTS 4u

/* Modbus exception codes. */
#define W5500_MB_EX_ILLEGAL_FUNCTION 1u
#define W5500_MB_EX_ILLEGAL_ADDRESS  2u
#define W5500_MB_EX_ILLEGAL_VALUE    3u
#define W5500_MB_EX_SERVER_FAILURE   4u

/** Decoded request/reply ADU (only the fields the handler needs). */
typedef struct {
    uint16_t txid;
    uint8_t  unit;
    uint8_t  fc;
    uint16_t addr;        /**< starting register / value address */
    uint16_t count;       /**< register count (1 for FC6) */
    uint16_t value;      /**< FC6 write value */
    const uint8_t *data;  /**< FC16 register payload (big-endian pairs) */
    uint16_t data_len;    /**< FC16 payload length in bytes */
    uint16_t frame_len;   /**< total bytes consumed from the stream */
} w5500_mb_adu_t;

/**
 * @brief Parse one Modbus TCP ADU.
 *
 * Validates the MBAP header (protocol id 0, sane length), the function code
 * layout and, for FC16, the byte-count/register-count agreement. Unknown
 * function codes parse successfully with only txid/unit/fc set, so the
 * server can answer with an exception.
 *
 * @return 0 on success, -1 when the frame is malformed or truncated.
 */
int w5500_mb_parse_adu(const uint8_t *buf, uint16_t len, w5500_mb_adu_t *out);

/**
 * @brief Parse one Modbus TCP response/reply.
 *
 * Validates the MBAP header, the transaction id and the reply layout for
 * FC3/4 (byte count == 2 * register count) and FC6/16 (echo). Exception
 * replies (fc | 0x80) parse successfully; the exception code lands in
 * @c value.
 *
 * @return 0 on success, -1 when malformed, truncated or not our txid.
 */
int w5500_mb_parse_response(const uint8_t *buf, uint16_t len, uint16_t txid,
                            w5500_mb_adu_t *out);

/** Build an exception reply (returns 9, or 0 if buf is too small). */
uint16_t w5500_mb_build_exception(uint8_t *buf, uint16_t cap, uint16_t txid,
                                  uint8_t unit, uint8_t fc, uint8_t code);

/** Build an FC3/FC4 reply: MBAP + fc + byte count + big-endian registers. */
uint16_t w5500_mb_build_read_response(uint8_t *buf, uint16_t cap,
                                      uint16_t txid, uint8_t unit, uint8_t fc,
                                      const uint16_t *regs, uint16_t count);

/** Build an FC6/FC16 echo reply (address + value/count). */
uint16_t w5500_mb_build_write_response(uint8_t *buf, uint16_t cap,
                                       uint16_t txid, uint8_t unit, uint8_t fc,
                                       uint16_t addr, uint16_t arg);

#endif /* W5500_MODBUS_INTERNAL_H */
