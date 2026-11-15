/**
 * @file w5500_sockets.h
 * @brief Event-driven, non-blocking socket API (Layer 3).
 *
 * All socket structures are opaque; the user works with @ref w5500_socket_t
 * handles. The model is reactive: connect/listen/send return immediately and
 * progress is reported through @ref w5500_event_cb_t callbacks dispatched
 * from w5500_poll(), which must be called periodically from the application
 * network task.
 *
 * Callbacks run in the context of the w5500_poll() caller, with the driver
 * lock released, so it is safe to call back into the driver from them.
 */
#ifndef W5500_SOCKETS_H
#define W5500_SOCKETS_H

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque socket handle, owned by a driver instance. */
typedef struct w5500_socket *w5500_socket_t;

/** Socket protocol selected at open time. */
typedef enum {
    W5500_PROTO_TCP = 0,
    W5500_PROTO_UDP = 1
} w5500_proto_t;

/** Asynchronous socket events reported to the user callback. */
typedef enum {
    W5500_EVENT_CONNECTED,     /**< TCP handshake completed */
    W5500_EVENT_DATA_RECEIVED, /**< New data waiting; call w5500_socket_recv() */
    W5500_EVENT_DISCONNECTED,  /**< Peer closed / connection lost; handle freed */
    W5500_EVENT_SEND_DONE,     /**< SEND command completed (Sn_IR.SEND_OK) */
    W5500_EVENT_ERROR          /**< Timeout or link failure; see status */
} w5500_event_t;

/**
 * @brief User event callback.
 * @param sock      Socket the event belongs to.
 * @param event     Event identifier.
 * @param status    Extra status (W5500_OK, or error code for ERROR).
 * @param user_arg  User context pointer passed at open time.
 *
 * @note After W5500_EVENT_DISCONNECTED or W5500_EVENT_ERROR with a fatal
 *       status the handle is released back to the driver; do not reuse it.
 */
typedef void (*w5500_event_cb_t)(w5500_socket_t sock,
                                 w5500_event_t event,
                                 w5500_result_t status,
                                 void *user_arg);

/** Socket open flags, mapped onto Sn_MR option bits. */
#define W5500_SOCK_F_NO_DELAY        0x01u /**< TCP: no delayed ACK (Sn_MR.ND) */
#define W5500_SOCK_F_MULTI           0x02u /**< UDP: multicast (Sn_MR.MULTI) */
#define W5500_SOCK_F_BROADCAST_BLOCK 0x04u /**< UDP: drop broadcasts (Sn_MR.BCASTB) */
#define W5500_SOCK_F_IGMP_V2         0x08u /**< UDP+multicast: IGMPv2 (Sn_MR.MC) */

/**
 * @brief Open a hardware socket with option flags.
 * @param flags  Any combination of @ref W5500_SOCK_F_NO_DELAY (TCP only),
 *               @ref W5500_SOCK_F_MULTI and @ref W5500_SOCK_F_BROADCAST_BLOCK
 *               (UDP only); unknown bits or flags invalid for the protocol
 *               return W5500_ERR_PARAM.
 * @return Same semantics as w5500_socket_open().
 */
w5500_result_t w5500_socket_open_ex(w5500_handle_t handle,
                                    w5500_proto_t proto,
                                    uint16_t local_port,
                                    uint32_t flags,
                                    w5500_event_cb_t cb,
                                    void *user_arg,
                                    w5500_socket_t *out_sock);

/**
 * @brief Open a hardware socket and arm its event callback (blocking only
 *        for the OPEN command completion, bounded by the chip timeout).
 * @param local_port  Source port (valid for TCP/UDP); 0 lets the chip decide.
 * @param cb          Event callback; required.
 * @param user_arg    Opaque pointer handed back in every callback.
 * @param out_sock    Receives the socket handle.
 */
w5500_result_t w5500_socket_open(w5500_handle_t handle,
                                 w5500_proto_t proto,
                                 uint16_t local_port,
                                 w5500_event_cb_t cb,
                                 void *user_arg,
                                 w5500_socket_t *out_sock);

/**
 * @brief Close the socket immediately (Sn_CR.CLOSE) and free its slot.
 *
 * If the socket was connected the peer is not notified with FIN; use
 * w5500_socket_disconnect() for a graceful TCP close.
 */
w5500_result_t w5500_socket_close(w5500_socket_t sock);

/** Graceful TCP disconnect (Sn_CR.DISCON). Completion is asynchronous. */
w5500_result_t w5500_socket_disconnect(w5500_socket_t sock);

/** Enter TCP server mode (Sn_CR.LISTEN); waits for a connection event. */
w5500_result_t w5500_socket_listen(w5500_socket_t sock);

/** Start a non-blocking TCP client connect; result arrives via CONNECTED/ERROR. */
w5500_result_t w5500_socket_connect(w5500_socket_t sock,
                                    const uint8_t ip[4],
                                    uint16_t port);

/**
 * @brief Non-blocking send.
 *
 * Transfers up to the free TX-buffer space (Sn_TX_FSR) and issues SEND.
 * @param sent  Receives how many bytes were accepted (may be 0 when the
 *              TX buffer is full; retry later or wait for SEND_DONE).
 * @note  Only one SEND may be in flight per socket: a call made before the
 *        previous SEND_DONE (or ERROR/DISCONNECTED) returns W5500_ERR_BUSY
 *        with *sent == 0. Retry from the SEND_DONE callback.
 */
w5500_result_t w5500_socket_send(w5500_socket_t sock,
                                 const uint8_t *data,
                                 uint16_t len,
                                 uint16_t *sent);

/** UDP send to an explicit destination (sets Sn_DIPR/Sn_DPORT, then SEND). */
w5500_result_t w5500_socket_sendto(w5500_socket_t sock,
                                   const uint8_t ip[4],
                                   uint16_t port,
                                   const uint8_t *data,
                                   uint16_t len,
                                   uint16_t *sent);

/**
 * @brief Zero-copy receive: data is read from the chip RX buffer directly
 *        into @c data (no intermediate driver copies), then Sn_RX_RD is
 *        advanced and RECV is issued.
 * @param recvd  Receives the number of bytes actually read (0 if none).
 *
 * @note A zero-length UDP datagram raises W5500_EVENT_DATA_RECEIVED but
 *       leaves Sn_RX_RSR == 0, so w5500_socket_recv() returns 0 bytes.
 *       Treat "event with recvd == 0" on a UDP socket as an empty datagram,
 *       not as an error.
 */
w5500_result_t w5500_socket_recv(w5500_socket_t sock,
                                 uint8_t *data,
                                 uint16_t len,
                                 uint16_t *recvd);

/** Like w5500_socket_recv(), but also reports the UDP peer address. */
w5500_result_t w5500_socket_recvfrom(w5500_socket_t sock,
                                     uint8_t *data,
                                     uint16_t len,
                                     uint16_t *recvd,
                                     uint8_t ip[4],
                                     uint16_t *port);

/** Query bytes waiting in the socket RX buffer (Sn_RX_RSR). */
w5500_result_t w5500_socket_rx_available(w5500_socket_t sock, uint16_t *avail);

/** Query free bytes in the socket TX buffer (Sn_TX_FSR). */
w5500_result_t w5500_socket_tx_free(w5500_socket_t sock, uint16_t *free_bytes);

/** Read the peer address of an established TCP connection (Sn_DIPR/DPORT). */
w5500_result_t w5500_socket_get_peer(w5500_socket_t sock,
                                     uint8_t ip[4],
                                     uint16_t *port);

/**
 * @brief Enable the TCP keep-alive timer (Sn_KPALVTR, unit = 5 s).
 * @param interval_5s  0 disables auto keep-alive.
 */
w5500_result_t w5500_socket_set_keepalive(w5500_socket_t sock,
                                          uint16_t interval_5s);

/** Set the IPv4 TTL for outgoing packets (Sn_TTL); default at open is 128. */
w5500_result_t w5500_socket_set_ttl(w5500_socket_t sock, uint8_t ttl);

/** Set the IPv4 TOS/DSCP byte for outgoing packets (Sn_TOS). */
w5500_result_t w5500_socket_set_tos(w5500_socket_t sock, uint8_t tos);

/** Read the TCP MSS currently in effect (Sn_MSSR; 0 = chip default). */
w5500_result_t w5500_socket_get_mss(w5500_socket_t sock, uint16_t *mss);

/** Override the TCP MSS before connect/listen (Sn_MSSR; 0 = default). */
w5500_result_t w5500_socket_set_mss(w5500_socket_t sock, uint16_t mss);

/** Read the hardware socket status register (Sn_SR). */
w5500_result_t w5500_socket_get_status(w5500_socket_t sock, uint8_t *status);

/**
 * @brief Send an immediate TCP keep-alive probe (Sn_CR.SEND_KEEP).
 *
 * Only valid on an established TCP socket. Waits for the command to be
 * accepted, bounded by the chip timeout (it does not wait for the peer's
 * answer).
 */
w5500_result_t w5500_socket_keepalive_probe(w5500_socket_t sock);

/**
 * @brief Snapshot the chip interrupt summary without servicing it.
 *
 * Intended for INTn-pin driven designs: call from the ISR or before
 * deciding to run w5500_poll(), then do the actual work in task context.
 *
 * @param ir   Common IR (CONFLICT/UNREACH/PPPoE/MP); 0 when none pending.
 * @param sir  SIR: one bit per socket, set while that socket has Sn_IR bits.
 */
w5500_result_t w5500_irq_pending(w5500_handle_t handle,
                                 uint8_t *ir,
                                 uint8_t *sir);

/**
 * @brief Service all sockets: dispatch completed events to callbacks.
 *
 * Call periodically (e.g. every 10-100 ms or on the INTn pin). Reads SIR
 * once, then only the Sn_IR of sockets with a pending summary bit, clears
 * handled bits, releases disconnected sockets and invokes user callbacks
 * after the driver lock is released. When no interrupt is pending the PHY
 * is sampled for link changes (see w5500_set_link_cb()).
 */
w5500_result_t w5500_poll(w5500_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* W5500_SOCKETS_H */
