# Protocol layers

DHCP, DNS, SNTP, Modbus TCP and MQTT are separate modules on top of the socket
API. The HTTP codec is a message parser/builder with no socket dependency.
The socket-based modules follow the same contract:

- **Caller-owned storage**: `<module>_size()` tells how many bytes an instance
  needs; `_init(&ptr, mem, size, ...)` places all state and packet buffers
  there. Nothing is allocated and nothing is global.
- **Tick driven**: `_poll(instance, now_ms)` never blocks. `now_ms` is a
  free-running millisecond counter; unsigned wraparound is handled.
- **One task**: every `_poll` and `w5500_poll()` must run in the same task
  (or under the same mutex).
- **Socket budget**: each instance consumes hardware sockets from the pool of
  four (table below). Plan the budget before opening application sockets.
  A DHCP client keeps its socket for the whole lease lifetime, and DNS/SNTP
  resolvers keep theirs open between queries for reuse. If all four slots are
  needed for applications, stop the protocol instances you do not need:
  `w5500_dhcp_stop()`, `w5500_dns_stop()`, `w5500_sntp_stop()` and
  `w5500_mqtt_stop()` release their socket. A Modbus server with four
  listeners occupies the entire pool. The HTTP codec uses no socket.

| Module | Sockets used | Ports | Timebase |
|--------|--------------|-------|----------|
| DHCP client | 1 (local 68) | 68 -> 67 | 4 s retries |
| DNS resolver | 1 (local 53535) | -> 53 | 2 s retries |
| SNTP client | 1 (local 53536) | -> 123 | 2 s retries |
| MQTT client | 1 (ephemeral) | 1883 by convention | 5 s connect, keepalive PINGREQ |
| HTTP codec | 0 | none | no timebase |
| Modbus server | 1..4 listeners | 502 (configurable) | event driven |
| Modbus client | 1 | configurable | 2 s resend, 4 s timeout |

## Recommended loop

```c
for (;;) {
    w5500_poll(h);                    /* events + SEND_DONE first */
    w5500_dhcp_poll(dhcp, now_ms);    /* renew, retransmit */
    if (dns_active) {
        w5500_dns_poll(dns, now_ms);  /* only while a query is in flight */
    }
    if (sntp_active) {
        w5500_sntp_poll(sntp, now_ms);
    }
    if (mqtt_active) {
        w5500_mqtt_poll(mqtt, now_ms);
    }
    w5500_modbus_server_poll(server); /* or: modbus_client_poll(cli, now_ms) */
    now_ms += 5;                      /* or read a hardware tick */
    delay_ms(5);
}
```

Polling the driver first matters: a protocol that receives a reply while a
previous `SEND` is still in flight will see `W5500_ERR_BUSY`; the pending
`SEND_DONE` is consumed by `w5500_poll()`, and the protocol retries on its
own schedule.

## DHCP client

- `w5500_dhcp_start()` opens UDP port 68, generates a transaction id and
  sends DISCOVER (broadcast flag set, client-id option = MAC, parameter
  request list = subnet mask, router, DNS).
- OFFER -> REQUEST (requested IP + server id) -> ACK. A NAK returns to
  DISCOVER.
- **Retransmits**: the current request is re-sent every 4 s; after 4
  transmissions the client closes its socket and reports
  `W5500_DHCP_EVENT_FAILED`. Call `w5500_dhcp_start()` again to retry (the
  example does this from the callback).
- **Lease**: renew is attempted at 50% of the lease time (REQUEST ->
  `W5500_DHCP_EVENT_RENEWED` on ACK); at 100% the client discards the lease
  and starts DISCOVER again.
- **Before the callback** the driver chip is reconfigured (SIPR, SUBR, GAR),
  so opening a socket inside `BOUND` is safe. DNS server, mask and gateway
  are available through `w5500_dhcp_get_config()`.
- Server port 67, client port 68; the client-id uses the MAC passed to
  `_init`.

## DNS resolver

- `w5500_dns_start()` validates and copies the name (labels 1..63 bytes,
  total <= 253, no empty labels), opens UDP (local 53535) and sends a single
  A query with a generated id.
- A reply is accepted only when the id matches and `QR`/`RCODE` are sane; a
  non-zero RCODE completes immediately with `W5500_ERR_STATE`.
- Answer parsing follows CNAME chains: a CNAME whose owner matches the name
  being resolved replaces the expected owner, so a later A record for the
  canonical name completes the query. Up to four A records are collected.
- The default callback reports the first A address. Use
  `w5500_dns_init_multi()` to receive every address with its TTL; the array
  points into the resolver and stays valid only for the call.
- The answer cache is off by default. `w5500_dns_set_cache(r, 1)` enables four
  TTL-bound entries; a cache hit is reported from the next `poll()` after
  `start()`, with no packet on the wire. `w5500_dns_cache_clear()` and a
  server change drop all entries. Entries with TTL 0 are not stored.
- Retries: every 2 s, three transmissions; `ERR_TIMEOUT` afterwards.
- Starting a new query cancels the previous one silently (no callback).

## SNTP client

- 48-byte mode-3 request from local port 53536 to server port 123.
- A reply is accepted only when the mode is 4 (server), the stratum is not 0
  (LI is not 3) and the transmit timestamp converts to a positive Unix time.
- The callback receives Unix time: `ntp_seconds - 2208988800`.
- Retries: every 2 s, three transmissions; `ERR_TIMEOUT` otherwise.

## Modbus TCP server

- `w5500_modbus_server_init(..., port, max_clients, read_cb, write_cb, user)`
  with `max_clients` in 1..4 opens that many listeners on the same port (each
  can carry one client). TCP no-delay is enabled for every listener.
- Requests are reassembled from the TCP stream using the MBAP length field.
  A frame with an illegal MBAP length or an inconsistent PDU drops that
  client; `poll()` reopens a listener for the next connection.
- Supported function codes: 3 and 4 (1..125 registers), 6 (single register),
  16 (1..123 registers). Everything else gets exception 01 (illegal
  function).
- Register access goes through the callbacks:
  - `read_cb(unit, fc, addr, count, regs_out, user)` - fill host-endian
    values.
  - `write_cb(unit, addr, count, regs_in, user)` - apply the values.
  - Returning `W5500_ERR_PARAM` answers exception 02 (illegal address); any
    other error answers exception 04 (server failure).
- One reply is outstanding per socket. If the driver reports `ERR_BUSY`, the
  reply stays queued and is flushed on a later `poll()`.
- Call `w5500_modbus_server_start()` from the link-up callback to reopen the
  listeners after a link flap.

## Modbus TCP client

- One request at a time per instance; starting another while one is active
  returns `W5500_ERR_BUSY`.
- `read()` (FC 3/4) stores up to 125 registers internally; the callback
  receives a pointer that stays valid until the next request.
- `write_single()` (FC 6) and `write_multiple()` (FC 16, up to 123 values)
  report `OK` with a NULL register pointer on success.
- Phases: open+connect (event), send (poll), await reply (poll). A late
  reply triggers one resend at 2 s; no reply by 4 s reports `ERR_TIMEOUT`.
- A Modbus exception, a mismatched transaction id or a malformed reply
  reports `W5500_ERR_STATE`. The socket is closed after every request.

## MQTT 3.1.1 client

`w5500_mqtt.h` is a minimal publisher/subscriber over one TCP socket. It is
tick driven, keeps all state in caller-owned memory and uses no heap.

- Allocate `w5500_mqtt_size()` bytes (static or long-lived) and call
  `w5500_mqtt_init()` with the chip handle, broker IP/port, client id and a
  callback. The client id must be shorter than 48 bytes.
- `w5500_mqtt_connect()` opens the socket and queues CONNECT (clean session
  flag, keepalive in seconds, optional user/password). Call it again only
  after a stop or a disconnect event; otherwise it returns `ERR_BUSY`.
- `w5500_mqtt_publish()` (QoS 0, optional retain) and
  `w5500_mqtt_subscribe()` (one topic filter, QoS 0/1) queue one packet at a
  time. `ERR_BUSY` means the previous packet has not been sent yet.
- Call `w5500_mqtt_poll()` alongside `w5500_poll()` on every tick. It sends
  queued packets, drains and parses inbound frames, emits callbacks and
  sends PINGREQ after half the keepalive interval without broker traffic.
- The callback receives CONNECTED, PUBLISH (topic and payload point into the
  client's RX buffer and stay valid only for the call), SUBACK,
  DISCONNECTED and ERROR. Copy any data the application needs to keep.
- A refused CONNACK, a connect timeout (5 s) or a link error closes the
  socket and emits DISCONNECTED/ERROR. `w5500_mqtt_stop()` closes without a
  callback; after it, `poll()` returns `ERR_STATE` until reconnecting.

Limitations: QoS 1/2 and PUBACK handling, multiple pending subscriptions,
TLS, websockets and retained publishing on the client side are not
implemented. Packets are capped at 512 bytes.
