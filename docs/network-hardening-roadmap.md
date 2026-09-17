# KOS N12 -- Network Core Hardening

N12 makes the QEMU e1000 network path observable and recoverable before more
network features are built on top of it.  It deliberately keeps the stack
poll-driven: packet processing remains deterministic in the early kernel and
does not do unbounded work in a hardware interrupt.

## Delivered in this milestone

- e1000 link-state sampling, TX watchdog, RX descriptor error recycling and
  safe DMA-ring reinitialisation after a stuck TX operation.
- Ethernet destination filtering and counters for malformed or unsupported
  Ethernet frames.
- Separate IPv4 malformed-packet and checksum-error counters.
- An ARP cache with reachable/stale state, a bounded size, TTL expiry and
  resolution timeout accounting.
- A small IPv4 route table with connected and default-gateway routes.  Outbound
  IPv4 traffic chooses its ARP next hop through this table.
- DHCP lease timestamps, renewal/rebind attempts and lease-expiry handling.
  Renewal has a finite timeout so it cannot wait forever in the terminal loop.
- Diagnostics: `ifconfig`, `arp`, `route`, `netinfo`, `netstats` and
  `e1000info`.

## Expected QEMU diagnostics

```text
KOS> ifconfig
eth0: UP  inet 10.0.2.x  ...

KOS> route
Destination       Netmask           Gateway
10.0.2.0          255.255.255.0     on-link
0.0.0.0           0.0.0.0           10.0.2.2

KOS> e1000info
e1000: initialized, link UP
```

## Deliberate limits

This is a single-core, polling early-kernel stack.  It does not yet provide
real hardware hot-plug, MSI/MSI-X, interrupt-driven e1000 RX, route editing,
or a production DHCP unicast-renewal state machine.  Those belong after the
kernel's process and user-mode isolation layers are stable.
