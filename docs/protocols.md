# Protocol layers

DHCP, DNS, SNTP and Modbus TCP are separate modules on top of the socket API.
They follow the same contract:

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
  `w5500_dhcp_stop()`, `w5500_dns_stop()`, `w5500_sntp_stop()` release their
  socket. A Modbus server with four listeners occupies the entire pool.

| Module | Sockets used | Ports | Timebase |
|--------|--------------|-------|----------|
| DHCP client | 1 (local 68) | 68 -> 67 | 4 s retries |
| DNS resolver | 1 (local 53535) | -> 53 | 2 s retries |
| SNTP client | 1 (local 53536) | -> 123 | 2 s retries |
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
- Only an A record in the reply completes the query. A CNAME-only reply is
  ignored and the request eventually times out (send an A query directly to
  a resolver that includes the A record).
- Retries: every 2 s, three transmissions; `ERR_TIMEOUT` afterwards.
- Starting a new query cancels the previous one silently (no callback).

## SNTP client

- 48-byte mode-3 request from local port 53536 to server port 123.
- A reply is accepted only when the mode is 4 (server), the stratum is not 0
  and the transmit timestamp is non-zero.
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
