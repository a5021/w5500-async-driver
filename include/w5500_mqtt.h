/**
 * @file w5500_mqtt.h
 * @brief Minimal MQTT 3.1.1 client (QoS 0) for the async W5500 driver.
 *
 * One TCP socket, caller-owned state, tick-driven, no heap. The client
 * auto-reconnects with PINGREQ keepalive and reports inbound PUBLISH
 * messages through a callback. No TLS, QoS 1/2 or retained publishing.
 */
#ifndef W5500_MQTT_H
#define W5500_MQTT_H

#include <stddef.h>
#include <stdint.h>

#include "w5500.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct w5500_mqtt w5500_mqtt_t;

/** Events delivered to the client callback. */
typedef enum {
    W5500_MQTT_EVENT_CONNECTED,     /**< CONNACK accepted */
    W5500_MQTT_EVENT_PUBLISH,       /**< topic/payload available */
    W5500_MQTT_EVENT_SUBACK,        /**< subscription acknowledged */
    W5500_MQTT_EVENT_DISCONNECTED,  /**< broker closed or link lost */
    W5500_MQTT_EVENT_ERROR          /**< connect refused / protocol error */
} w5500_mqtt_event_t;

/**
 * @brief Client callback for inbound data and state changes.
 * @param topic       Topic bytes (only for PUBLISH, NULL otherwise)
 * @param payload     Payload bytes (only for PUBLISH, NULL otherwise)
 */
typedef void (*w5500_mqtt_cb_t)(w5500_mqtt_t *cli,
                                w5500_mqtt_event_t event,
                                const uint8_t *topic, uint16_t topic_len,
                                const uint8_t *payload, uint16_t payload_len,
                                void *user_arg);

/** Bytes required for one client instance. */
size_t w5500_mqtt_size(void);

/** Initialize the client in caller-provided storage. */
w5500_result_t w5500_mqtt_init(w5500_mqtt_t **out_cli,
                               void *mem, size_t mem_size,
                               w5500_handle_t chip,
                               const uint8_t broker_ip[4], uint16_t port,
                               const char *client_id,
                               w5500_mqtt_cb_t cb, void *user_arg);

/** Open the socket and send CONNECT (clean session, keepalive in seconds). */
w5500_result_t w5500_mqtt_connect(w5500_mqtt_t *cli, uint32_t now_ms,
                                  uint16_t keepalive_s, uint8_t clean_session,
                                  const char *user, const char *pass);

/** Publish a QoS 0 message (retain flag 0/1). */
w5500_result_t w5500_mqtt_publish(w5500_mqtt_t *cli, uint32_t now_ms,
                                  const char *topic, const uint8_t *payload,
                                  uint16_t len, uint8_t retain);

/** Subscribe to one topic filter (QoS 0/1). */
w5500_result_t w5500_mqtt_subscribe(w5500_mqtt_t *cli, uint32_t now_ms,
                                    const char *topic, uint8_t qos);

/** Service the connection: send pending packets, read, keepalive, timeout. */
w5500_result_t w5500_mqtt_poll(w5500_mqtt_t *cli, uint32_t now_ms);

/** Close the socket and stop (no callback). */
void w5500_mqtt_stop(w5500_mqtt_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* W5500_MQTT_H */
