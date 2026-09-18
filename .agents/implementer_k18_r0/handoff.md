# Milestone K18 & K19 Handoff Report: Network Stack & Socket Subsystem

## Observation
Prior to this milestone, KOS had a monolithic network implementation tightly coupled to the Intel e1000 driver. `net/ethernet.c` and `net/socket.c` directly invoked `e1000_*` driver functions, preventing multi-NIC or virtual interface expansion. The e1000 driver lacked hardware interrupt-driven processing and proper descriptor ring error recovery. The network core lacked dynamic ARP cache expiration (entries were static/infinite), IPv4 fragmentation on transmit, and an IP datagram reassembly engine with timeout/ICMP handling. TCP lacked dynamic RTO estimation (Jacobson/Karn algorithm), congestion window management (slow start/congestion avoidance), and a formal TIME_WAIT timer. Finally, there was no independent, refcounted socket subsystem providing standard POSIX socket abstractions (`kos_socket`, `kos_bind`, `kos_listen`, `kos_accept`, `kos_connect`, `kos_send`, `kos_recv`, `kos_poll`) backed by kernel object headers.

## Logic Chain & Architectural Implementation
To resolve all architectural and functional deficits across Milestones K18 & K19, the following changes were systematically implemented:

1. **R1: Standardize Network Interface & Complete E1000 Driver**
   - **`include/kos/net_device.h` & `net/net_device.c`**: Created a standardized network interface abstraction (`struct net_device`). Defined operation function pointers in `struct net_device_ops` (`open`, `stop`, `transmit`, `receive`, `wait_transmit`, `poll`, `get_stats`, `reset`) and device statistics (`struct net_device_stats`). Implemented device registration (`net_device_register`, `net_device_unregister`), enumeration, and default device lookup (`net_device_get_default`).
   - **`drivers/e1000.c`**: Upgraded the Intel e1000 driver. Registered `eth0` via `net_device_register()`. Connected hardware interrupts (IMS/ICR registers) with `irq_register_handler(e1000.irq_line, e1000_irq_handler)` and unmasked the corresponding PIC line. Implemented RX descriptor error checking (recycling faulty descriptors back to the ring) and TX ring full backpressure. Implemented all `net_device_ops` callbacks.
   - **`net/ethernet.c`**: Decoupled completely from `e1000.h`. Now dispatches transmissions and polls through `net_device_get_default()`.

2. **R2: Complete Network Core**
   - **`net/arp.c`**: Implemented dynamic ARP cache expiration. Added `expires_at_tick` with `ARP_ENTRY_TTL_TICKS` (60s). Created `net_arp_cache_prune()` and updated `net_arp_resolve()` and `arp_lookup()` to invalidate expired entries automatically.
   - **`net/ipv4.c`**: Implemented IPv4 fragmentation on transmit when `payload_size > 1480`. Fragments carry identical identification, appropriate `MF` flags, and 8-byte aligned offsets. Implemented an 8-slot reassembly engine tracking received 8-byte bitmap blocks. On reassembly timeout (10s), discarded fragments trigger ICMP Time Exceeded (RFC 792).
   - **`net/icmp.c`**: Implemented `net_icmp_send_dest_unreachable` (Type 3) and `net_icmp_send_time_exceeded` (Type 11), generating RFC 792 frames including the original IPv4 header and payload bytes with valid 1's complement checksums.

3. **R3: Complete UDP & Solidify TCP**
   - **`net/socket.c`**: Updated UDP port demultiplexer to generate ICMP Destination Unreachable (Code 3: Port Unreachable) when a received datagram does not match any bound socket (ignoring broadcast/loopback). Decoupled initialization and detection from `e1000.h` to `net_device`.
   - **`net/tcp.c`**: Implemented dynamic RTO calculation using the Jacobson/Karn algorithm (`srtt`, `rttvar`, `rto_ticks`), Karn's rule (ignoring RTT on retransmitted segments), slow start and congestion avoidance window tracking (`cwnd`, `ssthresh`), effective transmit window throttling (`min(snd_wnd, cwnd)`), and a formal `TIME_WAIT` state transition with automated timer-based reaping in `net_tcp_poll()`.

4. **R4: Independent Socket Subsystem & Concurrency/Poll**
   - **`include/kos/socket.h`**: Defined standard POSIX socket constants (`AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_NONBLOCK`, `SOL_SOCKET`, `SO_REUSEADDR`, `SO_BROADCAST`, `SO_RCVTIMEO`, `SO_SNDTIMEO`, `SO_ERROR`, `SO_TYPE`, `POLLIN`, `POLLOUT`, error numbers, `struct sockaddr_in`, byte-order inline functions `htons`, `ntohs`, `htonl`, `ntohl`). Defined `struct socket` with embedded `struct kos_object_header` (refcounted, spinlock, wait queue, destructor callback).
   - **`net/socket_subsystem.c`**: Implemented the kernel socket subsystem. Managed a pool of refcounted socket objects. Implemented `kos_socket`, `kos_bind`, `kos_listen`, `kos_accept`, `kos_connect`, `kos_send`, `kos_recv`, `kos_sendto`, `kos_recvfrom`, `kos_shutdown`, `kos_close`, `kos_getsockopt`, `kos_setsockopt`, `kos_poll`, `socket_ref`, `socket_unref`. Implemented comprehensive `socket_self_test()`.
   - **`kernel/command.c` & `kernel/main.c`**: Added `sockettest` shell command, integrated `socket_subsystem_init()` and `socket_self_test()` into early kernel boot, and enhanced `netinfo` to report `net_device` information and interface statistics.
   - **`tools/kos.c`**: Added `net/net_device.c` and `net/socket_subsystem.c` to `c_sources` and rebuilt `tools/kos-tool.exe`.

---

## Diff Overview

### Files Created
- `include/kos/net_device.h`: Abstract network device structure (`struct net_device`), operations table, stats, and registration prototypes.
- `net/net_device.c`: Device registry, default device pointer management, statistics retrieval.
- `include/kos/socket.h`: Standard POSIX socket API definitions, sockaddr_in, refcounted `struct socket`.
- `net/socket_subsystem.c`: Implementation of socket subsystem, POSIX operations, poll readiness, refcounting, and self-test.

### Files Modified
- `drivers/e1000.c`: Standardized to register via `net_device_register()`; enabled IMS/ICR interrupts; added descriptor error recovery; implemented `net_device_ops`.
- `net/ethernet.c`: Decoupled from `e1000.h`; dispatches via `net_device_get_default()`.
- `include/kos/net.h`: Added `expires_at_tick` to `struct net_arp_cache_entry`; declared ARP prune, ICMP error sending functions.
- `net/arp.c`: Dynamic ARP entry TTL expiration (`ARP_ENTRY_TTL_TICKS = 6000`), expired entry pruning.
- `net/ipv4.c`: Fragmentation on transmit (`payload_size > 1480`); 8-slot fragment reassembly engine with timeout and ICMP Time Exceeded.
- `net/icmp.c`: Destination Unreachable and Time Exceeded generation with RFC 792 headers and checksums.
- `net/socket.c`: ICMP Port Unreachable dispatch on unmatched UDP datagrams; decoupled from `e1000.h` using `net_device`.
- `net/tcp.c`: Dynamic RTO Jacobson/Karn calculation, cwnd/ssthresh congestion control, effective send window throttling, TIME_WAIT state machine timer.
- `kernel/command.c`: Registered `sockettest` command; updated `command_help`; expanded `netinfo` with `net_device` status and stats.
- `kernel/main.c`: Added `socket_subsystem_init()` and boot verification `socket_self_test()`; executed `sockettest` during bootstrap.
- `tools/kos.c`: Added `net/net_device.c` and `net/socket_subsystem.c` to kernel build sources.

---

## Verification Record

### Build Commands and Output
```powershell
& 'C:/Program Files/LLVM/bin/clang.exe' tools/kos.c -o tools/kos-tool.exe
.\tools\kos-tool.exe build
```
Output:
```
Kernel ELF built with 818 embedded symbols: build/kos.elf
UEFI ESP staged: build/esp
Exit Code: 0
Compiler Warnings: 0
Compiler Errors: 0
Linker Warnings: 0
Linker Errors: 0
```

### Deep Verification (Ran Actual Tests)
1. **Boot Self-Tests (QEMU 25-second run with serial logging to `build/qemu_run25.log`)**:
   - `net_device: registered network interface eth0`: PASS
   - `Intel e1000 built-in driver: RX/TX DMA rings initialized`: PASS
   - `UDP loopback socket self-test passed`: PASS
   - `Socket subsystem self-test passed`: PASS (Verified POSIX socket creation, bind, UDP loopback communication, non-blocking EAGAIN rejection on empty queue, refcounting 1 -> 2 -> 1, and POLLOUT/POLLIN readiness)
   - `ICMP checksum self-test passed`: PASS
   - `ARP self-test passed: QEMU gateway resolved`: PASS
   - `ICMP ping self-test passed: QEMU gateway replied`: PASS
2. **Bootstrap Shell Command Verification**:
   - `sockettest`: PASS ("sockettest passed: Socket subsystem POSIX API, loopback, non-blocking EAGAIN, and refcount verified.")
   - `netinfo`: PASS (Reports registered net devices: 1; net device TX packets: 21; RX packets: 15; TX errors: 0; RX errors: 0)
   - `tcptest 1.1.1.1 80`: PASS (TCP handshake ESTABLISHED, HTTP GET sent, HTTP/1.1 301 Moved Permanently response received, TCP connection closed cleanly)
   - `nslookup example.com`: PASS (DNS query sent to 10.0.2.3, IP 172.66.147.243 resolved)
   - `curl http://example.com/`: PASS (Full HTML document received and rendered)
   - `httpd start 80`: PASS (Web server listening on port 80)
   - `curl http://10.0.2.15/sysinfo`: PASS (Local HTTP request handled and 200 OK returned)

### Shallow Verification (Manual / Eyeball Inspection)
- Eyeballed IPv4 fragmentation logic for packets exceeding 1480 bytes against RFC 791 specification.
- Eyeballed ICMP error packet layout against RFC 792 specification.
- Eyeballed Jacobson/Karn RTT calculation formulas (`srtt = 7/8*srtt + 1/8*m`, `rttvar = 3/4*rttvar + 1/4*|m - srtt|`) against RFC 6298.

### Unverified Aspects
- Heavy multi-fragment IPv4 reassembly under high-packet-loss stress in physical networks (only verified in internal reassembly structure logic).
- Simultaneous TCP connection scaling beyond 8 concurrent connections (limited by `NET_TCP_SOCKET_MAX = 8`).
- IPv6 operations (explicitly out of scope for KOS milestone).

---

## Known Issues
- `Minor Robustness Risk`: DHCP discovery in QEMU occasionally takes longer than the early 2.5s boot timeout (indicated by `DHCP self-test timed out`), but network remains fully functional using fallback static IP `10.0.2.15` and subsequent manual `dhcp` command successfully queries the lease.

---

## Remaining Risk & Next Step
- **Next Step**: Expose `kos_socket` operations to userland via the system call table (`sys_socket`, `sys_bind`, `sys_connect`, `sys_send`, `sys_recv`, etc.) when userland network programs are implemented.
