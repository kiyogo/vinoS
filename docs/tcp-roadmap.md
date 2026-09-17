# KOS N13 -- Bounded TCP Foundation

N13 hardens the early-kernel IPv4 TCP implementation used by `curl`,
`tcptest`, and the built-in `httpd`. It remains a polling, single-core kernel
stack: `net_tcp_poll()` performs bounded work over the fixed socket table and
never waits for the network while holding its socket lock.

## TCP states

KOS supports `CLOSED`, `LISTEN`, `SYN_SENT`, `SYN_RECEIVED`, `ESTABLISHED`,
`FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, and
`TIME_WAIT`. Both an active SYN and the server's SYN-ACK are retained for
retransmission. FIN is also retained, simultaneous close enters `CLOSING`, and
a TIME_WAIT socket is retained for two seconds before its tuple can return to
the fixed socket pool.

### T1 transition guard

Live sockets cannot assign a new TCP state directly. A transition guard accepts
only the protocol edges used by KOS: active open (`CLOSED -> SYN_SENT`),
passive open (`CLOSED -> LISTEN -> SYN_RECEIVED -> ESTABLISHED`), normal close,
simultaneous close, and the `TIME_WAIT` expiry path. Any cleanup transition to
`CLOSED` is also permitted. An invalid internal transition is rejected and
counted; an incoming segment that does not fit the receiving socket's state is
dropped and counted. `tcpinfo` exposes both counters so a state-machine issue
can be diagnosed without guessing from a stuck connection.

### T2 retransmission policy

Control packets (SYN, SYN-ACK, and FIN) use one bounded retransmission record;
data uses an eight-entry fixed scoreboard per socket. Its sequence end is
tracked separately from packet bytes, so cumulative ACKs retire every covered
segment while partial ACKs leave the segment timer active. The timer starts at
two seconds, doubles on each timeout, and is capped at sixteen seconds. Five
attempts are allowed; after the retry budget is exhausted the socket is closed
and the failure counter is incremented. A valid cumulative ACK frees scoreboard
entries without heap allocation. The send path fills the peer-advertised
window up to the fixed slot limit, then waits with a bounded timeout.

This is a small reliable send window, not SACK, window scaling, congestion
control, or selective retransmission based on holes; those remain later work.

### T4 flow control

The receiver advertises the free bytes in its fixed 8 KiB ring on every ACK.
Incoming data is clipped to the advertised receive window; fully duplicate,
out-of-window, and zero-window payloads are rejected safely and counted. An
overlapping segment is trimmed to the first new byte so a peer can recover
after a partial ACK. Reading from a socket immediately emits a window-update
ACK, allowing a sender blocked by a zero window to resume. The sender never
queues bytes beyond the peer's current window, and `tcpinfo` exposes blocked,
drop, zero-window, and window-update counters.

### T5 listener backlog and connection lifecycle

Each listener now counts both queued established children and half-open
`SYN_RECEIVED` children against its backlog. A duplicate SYN reuses the
existing half-open child and retransmits its SYN-ACK instead of allocating a
second socket. `accept()` only removes an `ESTABLISHED` child; a half-open
handshake can never be returned to the server as a live connection. Closing,
RST, and retry exhaustion release the listener reservation and remove any
stale queue entry. This keeps the fixed eight-socket pool bounded during
bursts of simultaneous connections and SYN retries.

### T6 TIME-WAIT lifecycle

Connections enter `TIME_WAIT` after an active close or simultaneous close and
remain there for the configured two-second 2*MSL guard. Any duplicate FIN or
payload for that exact tuple receives a fresh cumulative ACK and restarts the
quiet-period timer, so a delayed retransmission cannot corrupt a new socket.
Ephemeral-port allocation skips ports still held by `TIME_WAIT`; expiry then
returns the slot to the fixed pool and is counted for diagnostics.

### T7 duplicate-ACK fast retransmit

For an established socket, three duplicate cumulative ACKs for the same
`snd_una` mark the oldest unacknowledged data segment for immediate retransmit.
The next bounded `net_tcp_poll()` sends it without waiting for the RTO. The
attempt still consumes that segment's retry budget, while its exponential RTO
is not inflated by a duplicate-ACK signal. Invalid ACKs never trigger the fast
path. `tcpinfo` reports duplicate-ACK events and fast retransmissions.

### T8 RTT measurement and adaptive RTO

Each newly transmitted data segment is eligible for one RTT sample. The sample
is taken when a cumulative ACK covers that segment, but only if the segment has
never been retransmitted (Karn's algorithm). KOS maintains a bounded SRTT/RTTVAR
estimator and derives the next retransmission timeout as
`SRTT + max(4*RTTVAR, 10 ticks)`, clamped to 100--1600 timer ticks. The RTO is
used by subsequent data segments while existing in-flight segments retain their
own timers. `tcpinfo` exposes total samples, RTO updates, and per-socket
SRTT/RTTVAR. No heap allocation, unbounded work, or synthetic network success
is introduced.

## Bounds and timeouts

- 8 TCP sockets total; listener backlog defaults to 4.
- 8 KiB receive window per socket.
- MSS is 1460 bytes; outgoing data is segmented one MSS at a time.
- Four fixed out-of-order reassembly slots per socket; no heap allocation
  occurs in packet receive processing.
- Initial RTO is two seconds at the 100 Hz timer rate, with exponential
  backoff capped at sixteen seconds and five retransmission attempts.
- Adaptive RTO remains within one to sixteen seconds; retransmitted segments are
  excluded from RTT samples and timeout backoff remains bounded.
- Each socket keeps the end sequence of every queued data segment. Duplicate
  or partial ACKs therefore do not cancel a segment timer; only a cumulative
  ACK that covers the full segment retires it. A successful ACK resets that
  segment's retry timer and exhausted retry budgets close the socket.
- Connect and receive waits end after five seconds rather than blocking the
  terminal forever.

## Diagnostics

`netstat` lists the TCP states. `tcpinfo` reports segment, retransmission,
checksum, malformed-segment, timeout, reset, reassembly, backlog, socket-pool,
receive-window, invalid-transition, state-rejected-packet, and retransmission-
failure counters, plus RTT sample and adaptive-RTO counters.

## Deliberate limits

N13 does not yet implement SACK, window scaling, congestion control, TCP
options beyond MSS, IPv6, interrupt-driven RX, user-mode socket syscalls, or
production-grade SYN-flood protection. TLS remains separate and is not made
functional by this TCP milestone.
