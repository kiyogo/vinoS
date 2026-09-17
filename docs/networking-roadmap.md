# Networking roadmap

## Current status (N6 foundation)

The QEMU e1000 path now provides Ethernet, ARP, IPv4, ICMP, DHCP, UDP, DNS,
and a bounded TCP client/server core. TCP tracks sequence and acknowledgement
numbers, advertises receive windows, segments output at MSS, keeps one small
out-of-order segment for reassembly, retries unacknowledged data with an
exponential RTO, and expires closed connections through TIME-WAIT. All blocking
connect/send/receive operations have finite timeouts.

The DNS client sends A-record queries over UDP, validates the transaction ID,
server address and port, retries up to three times within the caller's total
deadline, and expires cached answers using their TTL. Terminal diagnostics are:

```text
KOS> dns example.com
example.com -> 93.184.216.34
KOS> tcpinfo
KOS> netstat
```

HTTP/1.1 is available through the bounded `curl` client. N6 now provides
freestanding SHA-256/HMAC/HKDF/AES-GCM primitives, a hardware-entropy gate,
and an explicit TLS session/error API. TLS 1.3 record exchange and X.509
validation are still pending; the kernel must not claim HTTPS success without
certificate validation.

`curl` accepts a bare host name, for example `curl example.com`; bare names use
HTTP by default so the command remains usable while TLS is unfinished. Use
`curl --https example.com` or an explicit `https://` URL when TLS is required.
Raw response body is the default (matching Windows `curl`); `--text` filters
HTML into readable text, and `--raw` is accepted as an explicit raw alias.
HTTP responses are buffered up to 128 KiB; larger responses are returned with a
clear partial-output warning instead of being reported as a connection error.

## Required path to `curl`

1. Implement a TLS 1.3 client in a protected user-space library (see
   `docs/tls-roadmap.md`).
2. Add certificate store and hostname validation.
3. Add HTTP redirect and HTTPS integration tests.
4. Move DNS/TCP/HTTP clients out of the kernel behind socket syscalls.
5. Add virtio-net and physical NIC drivers after the deterministic e1000 path.

The next visible networking milestone is TLS; each new protocol must have an
observable QEMU test and must not expose fake success output.
