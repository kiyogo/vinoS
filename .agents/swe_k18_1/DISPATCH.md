# Dispatch History

## 2026-09-18T08:44:31Z

You are teamwork_preview_swe, the SWE Light orchestrator for Milestone K18 & K19.
Your working directory is: d:/DATA/source/OS/KOS/OS2/.agents/swe_k18_1
Workspace root: d:/DATA/source/OS/KOS/OS2
Parent Sentinel conversation ID: ff798f53-5136-4b59-a56e-c8e3330e9c2f

The authoritative user request is recorded in: d:/DATA/source/OS/KOS/OS2/.agents/ORIGINAL_REQUEST.md under header ## Follow-up — 2026-09-18T08:38:37Z.

Task:
Nhiệm vụ Người 2 (K18 & K19): Hoàn thiện toàn diện Network Stack và Socket Subsystem độc lập cho KOS (x86_64), chuẩn hóa trừu tượng hóa NIC (driver e1000 hoàn chỉnh với RX/TX ring, interrupts, buffer lifecycle), IPv4/ARP/ICMP/UDP/TCP đầy đủ và cung cấp Socket Subsystem (refcount, poll/wait queue, blocking/non-blocking) độc lập sẵn sàng cho syscall layer.

Requirements:
- R1: Chuẩn hóa Network Interface (`netif` / `net_device`) & Hoàn thiện Driver E1000 (RX/TX descriptor rings, ngắt IMS/ICR, buffer lifecycle, error recovery).
- R2: Hoàn thiện Tầng Network Core (Ethernet framing/dispatch, ARP cache expiration & request/reply, IPv4 routing/validation/checksum/fragmentation/reassembly, ICMP echo/error).
- R3: Hoàn thiện Giao thức UDP (checksum, datagram, port demux) & Củng cố Tầng Giao vận TCP (state machine, 3-way handshake, teardown, sliding window, dynamic RTO retransmission Jacobson/Karn, basic congestion control).
- R4: Thiết kế Socket Subsystem Độc lập & Hỗ trợ Concurrency/Poll (socket struct refcounting, blocking/non-blocking, poll/wait queue readiness notification, spinlock/mutex concurrency, clean internal API for syscall layer).

Verification:
- Build: tools/kos-tool.exe build
- Test: QEMU with e1000 NIC, internal tests (netinfo, ping, udptest, TCP echo test client/server).

Maintain your progress ledger in d:/DATA/source/OS/KOS/OS2/.agents/swe_k18_1/progress.md.
Follow the SWE Light protocol: dispatch one implementer on the whole task, then repeated reviewer rounds with cumulative open-issues ledger, establishing correctness via running tests.
When complete, notify parent sentinel with your victory report.
