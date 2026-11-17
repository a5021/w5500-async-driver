# API reference

All public functions return `w5500_result_t` unless stated otherwise.
Handles are opaque; a `w5500_socket_t` belongs to a `w5500_handle_t`.

## Types

| Type | Meaning |
|------|---------|
| `w5500_handle_t` | Driver instance (`struct w5500_context *`) |
| `w5500_socket_t` | One claimed hardware socket (`struct w5500_socket *`) |
| `w5500_result_t` | `OK`, `ERR_PARAM`, `ERR_TIMEOUT`, `ERR_SPI`, `ERR_STATE`, `ERR_NOMEM`, `ERR_CHIP`, `ERR_BUSY` |
| `w5500_proto_t` | `W5500_PROTO_TCP`, `W5500_PROTO_UDP` |
| `w5500_event_t` | `CONNECTED`, `DATA_RECEIVED`, `DISCONNECTED`, `SEND_DONE`, `ERROR` |
| `w5500_plat_if_t` | Platform hooks: select, deselect, byte, burst, delay_ms, lock, unlock, delay_us |
| `w5500_link_info_t` | `up`, `speed_100`, `full_duplex` |
| `w5500_link_cb_t` | `(handle, const w5500_link_info_t *, user_arg)` |

Version macros: `W5500_VERSION_MAJOR/MINOR/PATCH`, `W5500_VERSION_STRING`.
Chip identity: `W5500_HW_VERSION` (0x04), `W5500_SOCKET_COUNT` (4).

## Core (`include/w5500.h`)

### Lifecycle

| Function | Purpose |
|----------|---------|
| `w5500_init(&h, &plat)` | Allocate and bind a context |
| `w5500_init_static(&h, mem, size, &plat)` | Same with caller storage |
| `w5500_context_size()` | Bytes needed by `w5500_init_static` |
| `w5500_deinit(h)` | Release (frees only heap contexts) |

### Chip

| Function | Purpose | Notes |
|----------|---------|-------|
| `w5500_sw_reset(h)` | MR.RST and wait for self-clear | Bounded by the timeout |
| `w5500_check(h)` | Probe VERSIONR, factory RTR and PHYCFGR | `ERR_CHIP` on mismatch |
| `w5500_get_hw_version(h, &ver)` | Raw VERSIONR byte | |
| `w5500_set_timeout(h, ms)` | Chip-wait timeout | 0 or > 0xFFFFF rejected; default 1000 ms |
| `w5500_get_link_status(h, &up)` | PHYCFGR LNK bit | |
| `w5500_get_link_info(h, &info)` | LNK/SPD/DPX decoded | |
| `w5500_set_link_cb(h, cb, user)` | Link-change callback | Fired from poll; NULL disables |
| `w5500_set_ping_block(h, enable)` | MR.PB | Off by default |
| `w5500_set_retransmit(h, rtr_100us, rcr)` | RTR/RCR | rtr_100us 0 rejected |
| `w5500_get_retransmit(h, &rtr, &rcr)` | Read-back | |

### Network configuration

| Function | Purpose |
|----------|---------|
| `w5500_set_mac` / `w5500_get_mac` | SHAR (6 bytes) |
| `w5500_set_ip` / `w5500_get_ip` | SIPR (4 bytes) |
| `w5500_set_subnet_mask` / `w5500_get_subnet_mask` | SUBR |
| `w5500_set_gateway` / `w5500_get_gateway` | GAR |
| `w5500_set_socket_buffer_size(h, kb)` | Same size for all sockets (1/2/4 KB with four sockets) |
| `w5500_set_socket_buffers(h, kb[4])` | Per-socket sizes, sum <= 16 KB per direction |
| `w5500_set_socket_buffers_ex(h, tx[4], rx[4])` | Independent TX/RX layouts |

Changing the buffer layout invalidates every open socket handle.

### Raw access

| Function | Purpose |
|----------|---------|
| `w5500_read_regs(h, addr, bsb, buf, len)` | Read from any block |
| `w5500_write_regs(h, addr, bsb, buf, len)` | Write to any block |

`len == 0` returns `ERR_PARAM`; a read on a valid handle otherwise only fails
on a platform error.

## Sockets (`include/w5500_sockets.h`)

### Open flags

| Flag | Sn_MR bit | Restriction |
|------|-----------|-------------|
| `W5500_SOCK_F_NO_DELAY` | ND | TCP only |
| `W5500_SOCK_F_MULTI` | MULTI | UDP only |
| `W5500_SOCK_F_BROADCAST_BLOCK` | BCASTB | UDP only |
| `W5500_SOCK_F_IGMP_V2` | MC | UDP, requires `MULTI` |

Unknown bits or protocol/flag mismatches return `ERR_PARAM`.

### Lifecycle

| Function | Purpose | Notes |
|----------|---------|-------|
| `w5500_socket_open(h, proto, port, cb, user, &sock)` | Open a socket | Blocks only for OPEN completion |
| `w5500_socket_open_ex(... , flags, ...)` | Same with Sn_MR flags | |
| `w5500_socket_close(sock)` | Immediate close (no FIN) | Releases the slot |
| `w5500_socket_disconnect(sock)` | Graceful DISCON | Completes via `DISCONNECTED` |
| `w5500_socket_listen(sock)` | TCP server mode | Bounded wait for LISTEN |
| `w5500_socket_connect(sock, ip, port)` | TCP client, non-blocking | Result via `CONNECTED`/`ERROR` |

### Data path

| Function | Purpose | Notes |
|----------|---------|-------|
| `w5500_socket_send(sock, data, len, &sent)` | TCP send | Partial; needs an established socket and non-empty data; `ERR_BUSY` while a SEND is in flight |
| `w5500_socket_sendto(sock, ip, port, data, len, &sent)` | UDP datagram | Same rules |
| `w5500_socket_recv(sock, buf, len, &recvd)` | Receive | Non-blocking; `recvd == 0` when empty |
| `w5500_socket_recvfrom(sock, buf, len, &recvd, ip, port)` | Receive + peer latch | Peer filled before data is consumed |
| `w5500_socket_rx_available(sock, &bytes)` | Sn_RX_RSR, double-sampled | |
| `w5500_socket_tx_free(sock, &bytes)` | Sn_TX_FSR, double-sampled | |
| `w5500_socket_get_peer(sock, ip, &port)` | Sn_DIPR/DPORT | |

### Options and status

| Function | Purpose |
|----------|---------|
| `w5500_socket_set_keepalive(sock, interval_5s)` | Sn_KPALVTR auto keep-alive (0 disables) |
| `w5500_socket_keepalive_probe(sock)` | Immediate SEND_KEEP (TCP ESTABLISHED only) |
| `w5500_socket_set_ttl(sock, ttl)` | Sn_TTL (default 128 at open) |
| `w5500_socket_set_tos(sock, tos)` | Sn_TOS |
| `w5500_socket_get_mss(sock, &mss)` / `w5500_socket_set_mss(sock, mss)` | Sn_MSSR (0 = chip default) |
| `w5500_socket_get_status(sock, &sr)` | Sn_SR |

### Events

| Function | Purpose |
|----------|---------|
| `w5500_poll(h)` | Pump events; SIR-based; callbacks run with the lock released |
| `w5500_irq_pending(h, &ir, &sir)` | Snapshot IR/SIR for INTn designs |

Callback signature: `void (*)(w5500_socket_t, w5500_event_t,
w5500_result_t status, void *user_arg)`. `status` carries `ERR_TIMEOUT` for a
chip TIMEOUT and `OK` otherwise.

Notes:

- If the peer sends data and then closes, `DATA_RECEIVED` arrives first with
  the slot still usable; `DISCONNECTED` follows after the RX buffer is
  drained (next poll).
- If `SEND_DONE` is lost (chip reset, link flap during a send), `poll()`
  recovers the in-flight guard from `Sn_TX_RD`/`Sn_TX_WR` and emits
  `SEND_DONE` on its own.

## Diagnostics (`include/w5500.h`)

| Function | Purpose |
|----------|---------|
| `w5500_stats_reset(&stats)` | Zero a counter block |
| `w5500_set_stats(handle, &stats)` | Attach counters (NULL disables) |
| `w5500_get_stats(handle)` | Return the attached block or NULL |

`w5500_stats_t` counts `spi_transfers`, `spi_bytes`, `spi_errors`, `timeouts`,
`link_changes`, `socket_opens`, `socket_closes`, `bytes_sent`,
`bytes_received`, `datagrams_sent` and `datagrams_received`. The caller owns
the storage; counters are plain `uint32_t` increments and need no locking
beyond the driver's own hooks.

## DHCP (`include/w5500_dhcp.h`)

| Function | Purpose |
|----------|---------|
| `w5500_dhcp_size()` | Storage bytes |
| `w5500_dhcp_init(&d, mem, size, chip, mac, cb, user)` | Initialize in caller storage |
| `w5500_dhcp_start(d, now_ms)` | Open UDP/68 and send DISCOVER |
| `w5500_dhcp_poll(d, now_ms)` | Receive, retransmit, renew, expire |
| `w5500_dhcp_stop(d)` | Close the socket, idle state |
| `w5500_dhcp_get_config(d, ip, mask, gw, dns)` | Current lease (any output may be NULL) |

Events: `W5500_DHCP_EVENT_BOUND`, `_RENEWED`, `_FAILED`. The lease is applied
to the chip before the callback.

## DNS (`include/w5500_dns.h`)

| Function | Purpose |
|----------|---------|
| `w5500_dns_size()` | Storage bytes |
| `w5500_dns_init(&r, mem, size, chip, server_ip, cb, user)` | Initialize (first A only) |
| `w5500_dns_init_multi(&r, mem, size, chip, server_ip, cb, user)` | Initialize (all A records + TTL) |
| `w5500_dns_set_server(r, ip)` | Change server (clears the cache) |
| `w5500_dns_set_cache(r, enable)` | Enable/disable the answer cache (off by default) |
| `w5500_dns_cache_clear(r)` | Drop cached answers |
| `w5500_dns_start(r, name, now_ms)` | A query (name is copied) |
| `w5500_dns_poll(r, now_ms)` | Receive, retry, time out |
| `w5500_dns_stop(r)` | Close the socket |

Callback status: `OK`, `ERR_TIMEOUT` (no answer), `ERR_STATE`
(NXDOMAIN/SERVFAIL or a broken reply). The multi callback receives
`const w5500_dns_addr_t *addrs` with up to `W5500_DNS_MAX_ADDRS` entries, each
holding `ip[4]` and `ttl_s`; `count` is 0 on error. CNAME chains are followed.
A cached answer is delivered from the next `poll()` after `start()`, without
sending a query.

## SNTP (`include/w5500_sntp.h`)

| Function | Purpose |
|----------|---------|
| `w5500_sntp_size()` | Storage bytes |
| `w5500_sntp_init(&c, mem, size, chip, server_ip, cb, user)` | Initialize |
| `w5500_sntp_set_server(c, ip)` | Change server |
| `w5500_sntp_start(c, now_ms)` | Mode-3 request |
| `w5500_sntp_poll(c, now_ms)` | Receive, retry, time out |
| `w5500_sntp_stop(c)` | Close the socket |

Callback delivers Unix time (`OK`) or `ERR_TIMEOUT`/`ERR_STATE`.

## Modbus TCP (`include/w5500_modbus.h`)

### Server

| Function | Purpose |
|----------|---------|
| `w5500_modbus_server_size()` | Storage bytes |
| `w5500_modbus_server_init(&s, mem, size, chip, port, max_clients, read_cb, write_cb, user)` | Initialize (1..4 listeners) |
| `w5500_modbus_server_start(s)` | Open and listen |
| `w5500_modbus_server_poll(s)` | Accept, reassemble ADUs, answer |
| `w5500_modbus_server_stop(s)` | Close all sockets |

Callbacks: `w5500_mb_read_cb(unit, fc, addr, count, regs_out, user)` and
`w5500_mb_write_cb(unit, addr, count, regs_in, user)`. `ERR_PARAM` maps to
exception 02, any other error to exception 04. Function codes: 3, 4, 6, 16;
anything else gets exception 01.

### Client

| Function | Purpose |
|----------|---------|
| `w5500_modbus_client_size()` | Storage bytes |
| `w5500_modbus_client_init(&c, mem, size, chip, server_ip, port, cb, user)` | Initialize |
| `w5500_modbus_client_read(c, now_ms, unit, fc, addr, count)` | FC3/FC4 |
| `w5500_modbus_client_write_single(c, now_ms, unit, addr, value)` | FC6 |
| `w5500_modbus_client_write_multiple(c, now_ms, unit, addr, values, count)` | FC16 |
| `w5500_modbus_client_poll(c, now_ms)` | Drive connect/send/await |
| `w5500_modbus_client_stop(c)` | Close the socket |

The callback receives `OK` with the register array for reads, `OK` with NULL
for writes, `ERR_STATE` for a Modbus exception and `ERR_TIMEOUT` when no
answer arrives.

## MQTT (`include/w5500_mqtt.h`)

| Function | Purpose |
|----------|---------|
| `w5500_mqtt_size()` | Storage bytes for one client |
| `w5500_mqtt_init(&c, mem, size, chip, broker_ip, port, client_id, cb, user)` | Initialize (client id < 48 bytes) |
| `w5500_mqtt_connect(c, now_ms, keepalive_s, clean_session, user, pass)` | Open TCP and queue CONNECT |
| `w5500_mqtt_publish(c, now_ms, topic, payload, len, retain)` | Queue QoS 0 PUBLISH |
| `w5500_mqtt_subscribe(c, now_ms, topic, qos)` | Queue SUBSCRIBE (QoS 0/1) |
| `w5500_mqtt_poll(c, now_ms)` | Send, receive, keepalive, timeout |
| `w5500_mqtt_stop(c)` | Close without a callback |

The callback is `void cb(w5500_mqtt_t *c, w5500_mqtt_event_t ev,
const uint8_t *topic, uint16_t topic_len, const uint8_t *payload,
uint16_t payload_len, void *user)`. Topic and payload are non-NULL only for
`W5500_MQTT_EVENT_PUBLISH` and are valid for the call only. Other events are
`CONNECTED`, `SUBACK`, `DISCONNECTED` and `ERROR`.

## HTTP (`include/w5500_http.h`)

Pure codec: no socket or driver dependency, no heap. All decoded pointers
alias the input buffer and stay valid only while that buffer is alive.

| Function | Purpose |
|----------|---------|
| `w5500_http_parse_response(buf, len, &out)` | Parse status line, headers and body |
| `w5500_http_parse_request_line(buf, len, &out)` | Parse `GET <path> HTTP/1.x` |
| `w5500_http_header_value(headers, len, name, &value_len)` | Find a header value |
| `w5500_http_build_get(buf, cap, path, host)` | Build an HTTP/1.0 GET request |
| `w5500_http_build_response(buf, cap, status, content_type, body, len)` | Build an HTTP/1.0 response |

`w5500_http_response_t` carries `status`, `reason`/`reason_len`,
`headers`/`headers_len`, `body`/`body_len` and a `truncated` flag set when
`Content-Length` exceeds the available bytes. `w5500_http_request_t` carries
`method`/`method_len` and `path`/`path_len`. Scope: no TLS, chunked encoding,
keep-alive or POST.
