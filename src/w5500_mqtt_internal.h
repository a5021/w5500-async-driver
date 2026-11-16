/**
 * @file w5500_mqtt_internal.h
 * @brief MQTT 3.1.1 packet codec shared by the client and fuzz harnesses.
 */
#ifndef W5500_MQTT_INTERNAL_H
#define W5500_MQTT_INTERNAL_H

#include <stdint.h>

#define W5500_MQTT_PKT_MAX 512u

/* Fixed-header packet types (high nibble). */
#define W5500_MQTT_CONNECT     1u
#define W5500_MQTT_CONNACK     2u
#define W5500_MQTT_PUBLISH     3u
#define W5500_MQTT_PUBACK      4u
#define W5500_MQTT_SUBSCRIBE   8u
#define W5500_MQTT_SUBACK      9u
#define W5500_MQTT_UNSUBSCRIBE 10u
#define W5500_MQTT_UNSUBACK    11u
#define W5500_MQTT_PINGREQ     12u
#define W5500_MQTT_PINGRESP    13u
#define W5500_MQTT_DISCONNECT  14u

/** A decoded incoming packet; payload pointers alias the input buffer. */
typedef struct {
    uint8_t        type;          /**< packet type from fixed header */
    uint8_t        flags;         /**< low nibble of fixed header */
    uint8_t        qos;           /**< PUBLISH QoS */
    uint8_t        retain;        /**< PUBLISH retain flag */
    uint16_t       packet_id;     /**< PUBACK/SUBACK/SUBSCRIBE id */
    const uint8_t *topic;         /**< PUBLISH topic (NULL if absent) */
    uint16_t       topic_len;
    const uint8_t *payload;       /**< PUBLISH payload */
    uint16_t       payload_len;
} w5500_mqtt_packet_t;

/** Remaining-length field value for a stored packet, or -1 on error. */
int w5500_mqtt_remaining_len(const uint8_t *buf, uint16_t len,
                             uint16_t *value, uint8_t *header_len);

/**
 * @brief Decode a full MQTT packet.
 * @return 0 on success, -1 on malformed/incomplete input.
 */
int w5500_mqtt_parse(const uint8_t *buf, uint16_t len,
                     w5500_mqtt_packet_t *out, uint16_t *consumed);

/** Build CONNECT (v3.1.1, clean session, optional user/pass). */
uint16_t w5500_mqtt_build_connect(uint8_t *buf, uint16_t cap,
                                  const char *client_id,
                                  uint16_t keepalive_s,
                                  uint8_t clean_session,
                                  const char *user, const char *pass);

/** Build PUBLISH (QoS 0 only). */
uint16_t w5500_mqtt_build_publish(uint8_t *buf, uint16_t cap,
                                  const char *topic, const uint8_t *payload,
                                  uint16_t len, uint8_t retain);

/** Build SUBSCRIBE with one topic filter (QoS 0/1). */
uint16_t w5500_mqtt_build_subscribe(uint8_t *buf, uint16_t cap,
                                    const char *topic, uint16_t packet_id,
                                    uint8_t qos);

/** Build a no-payload fixed-header packet (PINGREQ / DISCONNECT). */
uint16_t w5500_mqtt_build_simple(uint8_t *buf, uint16_t cap, uint8_t type);

#endif /* W5500_MQTT_INTERNAL_H */
