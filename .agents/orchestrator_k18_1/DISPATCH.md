# Dispatch Log

## 2026-09-18T08:14:00Z
From: parent (722a75aa-3ca9-4c7e-a0aa-97e2481a29f9)
Role: Project Orchestrator for Milestone K18 & K19 (Complete Network Stack and Independent Socket Subsystem) in KOS (x86_64)

### Mission Objectives
1. R1. Chuẩn hóa Network Interface & Hoàn thiện Driver E1000:
   - Tách rời network stack khỏi driver e1000 bằng trừu tượng hóa network interface (`netif` / `net_device`).
   - Hoàn thiện driver e1000: hỗ trợ cấu hình RX/TX descriptor rings, ngắt (interrupts) với IMS/ICR, quản lý vòng đời buffer/packet, xử lý trạng thái lỗi và khôi phục khi ring đầy hoặc lỗi truyền nhận.
2. R2. Hoàn thiện Tầng Network Core (Ethernet, ARP, IPv4 Routing, Fragmentation & ICMP):
   - Ethernet layer: đóng gói, phân tích frame và dispatch theo ethertype.
   - ARP: quản lý ARP cache động với thời gian hết hạn (expiration/timeout), xử lý ARP request/reply đầy đủ.
   - IPv4 core: kiểm tra header validation và checksum, định tuyến IPv4 cơ bản (local, subnet, default gateway, loopback), xử lý fragmentation và reassembly ở tầng IPv4.
   - ICMP: echo request/reply (ping) và xử lý/phát sinh thông điệp báo lỗi mạng (Destination Unreachable, Time Exceeded).
3. R3. Hoàn thiện Giao thức UDP & Củng cố Tầng Giao vận TCP:
   - UDP: truyền nhận datagram tin cậy, kiểm tra checksum, port demux.
   - TCP State Machine: hỗ trợ đầy đủ các trạng thái chuẩn (SYN, ACK, FIN, RST, v.v.), bắt tay 3 bước, đóng kết nối đúng chuẩn (TIME_WAIT, CLOSE_WAIT, LAST_ACK).
   - TCP Reliability & Flow Control: quản lý sequence/acknowledgement numbers, receive/send sliding window, retransmission queue với RTT/RTO động (Jacobson/Karn algorithm), timer quản lý timeout.
   - Congestion Control cơ bản: slow start, congestion avoidance (như Reno/Tahoe cơ bản) để TCP hoạt động ổn định khi truyền lượng dữ liệu lớn.
4. R4. Thiết kế Socket Subsystem Độc lập & Hỗ trợ Concurrency/Poll:
   - Socket Object Lifecycle: struct socket với reference counting (`refcount`), atomic cleanup, tách rời khỏi các tầng ứng dụng.
   - Standard Socket Operations: hỗ trợ đầy đủ `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `shutdown()`, `getsockopt()`, `setsockopt()`.
   - Blocking & Non-blocking: hỗ trợ cờ non-blocking (O_NONBLOCK), timeout trên send/recv.
   - Event Notification / Poll: tích hợp wait queue / poll mechanism (`poll` / `select` readiness notification) để luồng người dùng hoặc kernel thread không phải busy wait.
   - Socket Concurrency: hỗ trợ nhiều socket hoạt động đồng thời an toàn (đồng bộ hóa bằng spinlock/mutex).
   - Cung cấp API nội bộ sạch cho syscall layer (không trực tiếp sửa syscall dispatcher của Người 5).
