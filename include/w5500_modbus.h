/**
 * @file w5500_modbus.h
 * @brief Modbus TCP server and client for the async W5500 driver.
 *
 * Both ends live in caller-provided storage (no heap) and are serviced from
 * the same task as w5500_poll(). The server answers FC 3/4 (read), 6 (write
 * single) and 16 (write multiple); registers are accessed through user
 * callbacks, so the library owns no register map.
 */
#ifndef W5500_MODBUS_H
#define W5500_MODBUS_H

#include <stddef.h>
#include <stdint.h>

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Function codes understood by the server (and usable by the client). */
#define W5500_MB_FC_READ_HOLDING   3u  /**< Read holding registers */
#define W5500_MB_FC_READ_INPUT     4u  /**< Read input registers */
#define W5500_MB_FC_WRITE_SINGLE   6u  /**< Write one holding register */
#define W5500_MB_FC_WRITE_MULTIPLE 16u /**< Write several holding registers */

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

typedef struct w5500_modbus_server w5500_modbus_server_t;

/**
 * @brief Serve read requests (FC 3/4).
 * @param count  Registers to read, 1..125; fill @p regs host-endian.
 * @return W5500_OK on success, W5500_ERR_PARAM for an illegal address/range
 *         (answered as exception 2), anything else as exception 4.
 */
typedef w5500_result_t (*w5500_mb_read_cb)(uint8_t unit, uint8_t fc,
                                           uint16_t addr, uint16_t count,
                                           uint16_t *regs, void *user_arg);

/**
 * @brief Serve write requests (FC 6/16).
 * @param regs  Host-endian values, @p count of them.
 */
typedef w5500_result_t (*w5500_mb_write_cb)(uint8_t unit, uint16_t addr,
                                            uint16_t count,
                                            const uint16_t *regs,
                                            void *user_arg);

/** Bytes required for one server instance. */
size_t w5500_modbus_server_size(void);

/**
 * @brief Initialize a server.
 * @param port         TCP port to listen on (usually 502).
 * @param max_clients  Listening sockets to open, 1..4: that many clients can
 *                     be served simultaneously.
 * @param read_cb      Required: FC 3/4 handler.
 * @param write_cb     Required: FC 6 (count == 1) and FC 16 handler.
 */
w5500_result_t w5500_modbus_server_init(w5500_modbus_server_t **out_srv,
                                        void *mem,
                                        size_t mem_size,
                                        w5500_handle_t chip,
                                        uint16_t port,
                                        uint8_t max_clients,
                                        w5500_mb_read_cb read_cb,
                                        w5500_mb_write_cb write_cb,
                                        void *user_arg);

/** Open the listening sockets. */
w5500_result_t w5500_modbus_server_start(w5500_modbus_server_t *srv);

/**
 * @brief Service the server: accept, reassemble ADUs, answer, retry sends.
 *
 * Call next to w5500_poll() (which dispatches the socket events). A client
 * that sends a malformed frame is disconnected; a socket that the driver
 * released is reopened and listening again.
 */
w5500_result_t w5500_modbus_server_poll(w5500_modbus_server_t *srv);

/** Close all server sockets. */
void w5500_modbus_server_stop(w5500_modbus_server_t *srv);

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

typedef struct w5500_modbus_client w5500_modbus_client_t;

/**
 * @brief Result callback, exactly once per request.
 * @param status  W5500_OK, W5500_ERR_TIMEOUT (no answer) or
 *                W5500_ERR_STATE (Modbus exception or broken frame).
 * @param regs    Read result (internal storage) or NULL for writes.
 */
typedef void (*w5500_mb_client_cb)(w5500_modbus_client_t *cli,
                                   w5500_result_t status,
                                   const uint16_t *regs,
                                   uint16_t count,
                                   void *user_arg);

/** Bytes required for one client instance. */
size_t w5500_modbus_client_size(void);

w5500_result_t w5500_modbus_client_init(w5500_modbus_client_t **out_cli,
                                        void *mem,
                                        size_t mem_size,
                                        w5500_handle_t chip,
                                        const uint8_t server_ip[4],
                                        uint16_t port,
                                        w5500_mb_client_cb cb,
                                        void *user_arg);

/** Start FC3/FC4 read of @p count (1..125) registers at @p addr. */
w5500_result_t w5500_modbus_client_read(w5500_modbus_client_t *cli,
                                        uint32_t now_ms,
                                        uint8_t unit,
                                        uint8_t fc,
                                        uint16_t addr,
                                        uint16_t count);

/** Start FC6 write of one register. */
w5500_result_t w5500_modbus_client_write_single(w5500_modbus_client_t *cli,
                                                uint32_t now_ms,
                                                uint8_t unit,
                                                uint16_t addr,
                                                uint16_t value);

/** Start FC16 write of @p count (1..123) registers. */
w5500_result_t w5500_modbus_client_write_multiple(
    w5500_modbus_client_t *cli,
    uint32_t now_ms,
    uint8_t unit,
    uint16_t addr,
    const uint16_t *values,
    uint16_t count);

/**
 * @brief Drive the request: connect, send, await the answer, time out.
 *
 * One resend happens if the answer is late (2 s), the request fails with
 * W5500_ERR_TIMEOUT after 4 s.
 */
w5500_result_t w5500_modbus_client_poll(w5500_modbus_client_t *cli,
                                        uint32_t now_ms);

/** Close the client socket and cancel a pending request (no callback). */
void w5500_modbus_client_stop(w5500_modbus_client_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* W5500_MODBUS_H */
