# KOS network roadmap: N14 through N20

This document defines the dependency order for turning KOS's QEMU IPv4 stack
into a safer operating-system networking foundation. A feature is only marked
complete after it has real packet-path tests; a command that prints a planned
result is not a completed network feature.

## N14 -- DNS and HTTP hardening

### Delivered baseline

- DNS A lookups use UDP, transaction IDs, finite retry, timeout and an
  eight-entry TTL cache. `dnsinfo` reports cache/hit/miss/timeout state and
  `dns flush` clears only that cache.
- HTTP/1.1 requests emit `Host`, `User-Agent` and `Connection: close`.
- HTTP status, `Content-Length`, chunked transfer decoding and the standard
  301/302/303/307/308 redirect statuses are handled with response-size limits.
- The shell exposes `curl -i`, `-I`, `-v`, `-L` and `-o`.

### Work remaining in N14

- Add CNAME following with bounded loop detection.
- Add AAAA only alongside N18 IPv6; an AAAA result cannot be used by the
  current IPv4-only transport.
- Add HTTP parser fuzz vectors for bad chunk boundaries, overlong headers,
  malformed status lines and redirect loops.

Acceptance: repeated `dns example.com`, `curl http://example.com`, a chunked
HTTP endpoint and a redirect endpoint all finish without growing socket use.

## N15 -- authenticated TLS 1.3 client

TLS is a security boundary, not a formatter around TCP. It must be developed
as a separately testable cryptographic library before it receives internet
traffic.

1. Add audited test vectors for SHA-256, HMAC, HKDF, AES-GCM and a new
   ChaCha20-Poly1305 implementation.
2. Add X25519 and its known-answer tests.
3. Add DER length parser with strict bounds, then X.509 certificate parsing.
4. Implement signature verification needed by the chosen root certificates
   (ECDSA P-256 first; RSA only if required by the selected trust store).
5. Put a small curated root CA store in the immutable initramfs.
6. Implement TLS 1.3 ClientHello, SNI, ALPN `http/1.1`, ServerHello, key
   schedule, encrypted extensions, certificate verification and Finished.
7. Provide TLS record encrypt/decrypt with strict sequence-number accounting.

Acceptance: `curl https://example.com` succeeds with certificate chain,
hostname and validity checks enabled. `--insecure` is debug-only and never the
default. The current KOS TLS code intentionally reports “not implemented”; it
does not pretend HTTPS works.

## N16 -- user-mode network API

Prerequisite: process table, per-process page tables, user-copy validation and
ELF user program loading. Until those exist, dereferencing a user pointer from
a socket syscall would be unsafe.

Kernel interface:

```text
socket, bind, connect, send, recv, listen, accept, close
```

The kernel retains NIC drivers, Ethernet/IP/TCP and validated socket objects.
DNS resolver, HTTP client, TLS library and `curl` move into Ring 3. Each call
validates handle ownership, buffer range, length and blocking timeout. A
crashed `curl` must release only its handles, not panic the kernel.

Acceptance: an ELF Ring-3 test program opens a socket, receives HTTP data, is
terminated, and kernel `netstat` shows no leaked sockets.

## N17 -- additional NIC drivers

1. virtio-net PCI: feature negotiation, virtqueues, RX/TX descriptor chains,
   bounded completion polling, link status and reset recovery.
2. RTL8139: legacy QEMU compatibility only, isolated behind the same driver
   interface.
3. e1000/e1000e: extend the current e1000 driver only after hardware IDs and
   register differences are verified.
4. Physical Ethernet: DMA mapping, IOMMU policy where available, interrupt
   handling and hardware-specific recovery.
5. Wi-Fi last: firmware loading, 802.11 authentication and WPA are a separate
   subsystem, not a small extension of Ethernet.

Acceptance: `ifconfig`, DHCP, ping and HTTP run on both e1000 and virtio-net
in independent QEMU profiles.

## N18 -- IPv6 and interface management

Build a separate IPv6 parser/transmitter rather than overloading IPv4 fields:

- link-local addressing, Neighbor Discovery and router advertisements;
- SLAAC, then DHCPv6;
- IPv6 DNS records and `ping6`;
- per-family route tables;
- interface up/down and manually configured IPv4/IPv6 addresses.

Acceptance: KOS gets an IPv6 address through SLAAC and reaches an IPv6 test
endpoint without affecting IPv4 DHCP.

## N19 -- firewall and network security

Start with a bounded, stateless IPv4/IPv6 packet filter placed after packet
length/checksum validation and before transport dispatch. Rules have direction,
protocol, address/prefix and port range, with default-deny options and counters.
After N16, add socket ownership/permission checks and rate limits. Network
namespaces require process isolation and are not meaningful earlier.

Acceptance: inbound port 80 can be blocked while DNS and outbound HTTP still
work; malformed input increments counters without reaching TCP/UDP parsers.

## N20 -- verification and performance

- host-independent packet fixtures for Ethernet, IPv4, TCP, DNS, HTTP and TLS;
- QEMU loss/reorder scenarios, socket exhaustion and response-size tests;
- DNS timeout/cache-expiry tests;
- malformed certificate and hostname-mismatch TLS tests;
- e1000 and virtio-net matrix;
- bounded parser fuzzing run outside the kernel build, with every crash input
  converted to a regression fixture;
- latency, throughput, memory and socket-leak measurements.

No phase is complete solely because it compiles. Each acceptance test must run
on QEMU and record the relevant `netstats`, `tcpinfo` and driver counters.
