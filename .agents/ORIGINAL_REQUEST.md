# Original User Request

## Initial Request — 2026-09-17T20:01:36Z

# Milestone K12: Ổn định lõi kernel KOS (x86_64)

Bạn đang làm việc trực tiếp trên mã nguồn dự án KOS tại thư mục `d:\DATA\source\OS\KOS\OS2`.
Kiến trúc mục tiêu: x86_64, freestanding C17 + NASM x86_64, UEFI Limine, QEMU q35 cpu max, e1000.
Công cụ build chính: `.\tools\kos-tool.exe build` (điều phối Clang, LLD, NASM).

## Hiện trạng Codebase Đã Hoàn Thành:
1. Báo cáo kiểm toán kiến trúc đã lập tại `docs/k12_audit.md`.
2. K12.0 (Logging Foundation): Ring buffer 64 KiB trong RAM, kvsnprintf/ksnprintf không malloc, emergency polling bypass serial COM1, lệnh dmesg đã tích hợp và hoạt động.
3. K12.1 (Exception Handling & IDT): 256 vector IDT, struct interrupt_context 176 byte chuẩn AMD64 ABI, IST1 cho Double Fault (Vector 8) với stack 16 KiB riêng, dynamic exception handler registry và exception_self_test đã vượt qua.

## Nhiệm vụ Tiếp Tục Thực Hiện:
Triển khai tuần tự các phase còn lại của K12 theo quy trình nghiêm ngặt (mỗi phase phải build thành công bằng `.\tools\kos-tool.exe build` và smoke test QEMU):

### Phase 1: Diagnostics & Fault Handling (K12.2, K12.3, K12.4)
- K12.2: Page Fault Handler (`kernel/fault.c`, `include/kos/fault.h`): Đọc CR2 ngay khi vào handler, giải mã toàn bộ bit error code (P, W/R, U/S, RSVD, I/D, PK, SS), panic khi lỗi kernel mode, unmap page 0, lệnh shell `pftest`.
- K12.3: Kernel Panic Subsystem (`kernel/panic.c`, `include/kos/panic.h`): API `panic()`, `panicf()`, `panic_with_frame()`, recursion guard `panic_in_progress`, banner, uptime, CPU id, TID/PID, RIP/RSP/RBP/CR2/CR3, register dump, stack trace. Polling khẩn cấp ra serial COM1 (không malloc, không mutex, không VFS, không network), lệnh shell `panic-test`.
- K12.4: Stack Trace (`kernel/stacktrace.c`, `include/kos/stacktrace.h`): Cấu hình clang `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`, duyệt chuỗi RBP (alignment, canonical address, stack range, frame limit), symbol table resolution, lệnh shell `backtrace`.

### Phase 2: Synchronization Primitives (K12.5, K12.6, K12.7, K12.16)
- K12.5: Atomic primitives (`include/kos/atomic.h`): Clang builtins 32-bit & 64-bit, kiểu `refcount_t` với underflow detection, lệnh `atomictest`.
- K12.6: Spinlock (`include/kos/spinlock.h`, `kernel/spinlock.c`): `spinlock_t`, `spin_lock_irqsave()` và `spin_unlock_irqrestore()` bảo tồn chính xác cờ IF của RFLAGS, vòng lặp `cpu_pause()`, debug owner tracking, lệnh `locktest`.
- K12.16: Wait Queue (`include/kos/wait.h`, `kernel/wait.c`): `wait_queue_head_t`, hàng chờ không lost wakeup.
- K12.7: Mutex (`include/kos/mutex.h`, `kernel/mutex.c`): `mutex_t`, sleep-blocking đưa luồng vào `THREAD_STATE_BLOCKED`, FIFO wake, `assert_can_sleep()` assert trong ISR, lệnh `mutextest`.

### Phase 3: Standardized Timer Subsystem (K12.8)
- PIT 1000 Hz (1 tick = 1 ms). Tách biệt hardware source, monotonic 64-bit clock, nhịp scheduler tick, software timer queue.
- Hàm `thread_sleep(uint64_t ms)` đưa luồng vào sleep không đốt CPU.

### Phase 4: Preemptive Multitasking & Scheduler (K12.9, K12.28, K12.29)
- Preemptive Round-Robin Scheduler (quantum time slice 10 ms).
- Timer IRQ giảm quantum, đánh dấu `need_resched`, chuyển ngữ cảnh khi preemption enabled.
- `preempt_disable()` / `preempt_enable()` có nesting counter.
- Idle thread chuyên dụng (`idle_thread`) thực thi `sti; hlt`.
- Reaper thread chuyên dụng: dọn dẹp stack của các luồng ZOMBIE/DEAD, không giải phóng stack trên chính luồng đang chạy.
- Cập nhật `task_switch.asm` bảo toàn toàn vẹn ngữ cảnh.
- Thống kê scheduler và lệnh `taskstress`.

### Phase 5: Process, Thread, Object Model & Handle Table (K12.10 - K12.15)
- Struct `thread` với TID 64-bit đơn điệu, trạng thái (NEW, RUNNABLE, RUNNING, BLOCKED, SLEEPING, ZOMBIE, DEAD), stack riêng, time slice.
- Struct `process` chứa PID 64-bit, danh sách thread, handle table, refcount, exit status.
- Kernel Object model: `kobject`, type (THREAD, PROCESS, FILE, SOCKET, EVENT), refcount, destructor tự động.
- Handle Table: Số nguyên 32-bit kết hợp index và generation chống stale handle, quản lý rights (READ, WRITE, WAIT, SIGNAL, ADMIN), lookup an toàn tăng refcount. Lệnh `handletest`.

### Phase 6: Hardening Bộ nhớ, VFS & Network (K12.13, K12.19, K12.20)
- Nâng cấp `heap_lock`, `pmm_lock`, `vmm_lock` sang spinlock `irqsave`.
- Thống kê heap metadata, kiểm tra poison pattern phát hiện double free.
- Bảo vệ `vfs_lock`, `ramfs_lock`, `arp_lock`, socket tables. Driver e1000 an toàn khi preemption bật.

### Phase 7: Stress Testing & Verification (K12.21 - K12.25)
- Bộ lệnh shell: `atomictest`, `locktest`, `mutextest`, `taskstress`, `handletest`, `backtrace`, `pftest`, `panic-test`, `kstress`.
- Lệnh `kstress`: kiểm thử tải tổng hợp nhiều chu kỳ (CPU, memory, VFS, network, mutex contention) với snapshot bộ nhớ trước và sau kiểm thử.
- Hoàn thiện tài liệu tổng kết K12: `docs/k12_report.md`.

Mỗi phase phải được kiểm tra biên dịch bằng `.\tools\kos-tool.exe build` và smoke test QEMU trước khi bàn giao.
Working directory: `d:\DATA\source\OS\KOS\OS2`
Integrity mode: development

## Follow-up — 2026-09-18T00:22:16Z

Implement Milestone K14 (Process and Ring 3 Userspace) in the KOS x86_64 operating system kernel, delivering an ELF64 loader, Ring 3 privilege switching with TSS/RSP0, user stack management, and a complete process/thread lifecycle subsystem.

Working directory: d:/DATA/source/OS/KOS/OS2
Integrity mode: development

## Requirements

### R1. ELF64 Binary Loader & Address Space Mapping
- Parse and strictly validate ELF64 headers and Program Headers: check magic bytes, 64-bit architecture, endianness, entry point, alignment, and segment boundary limits prior to mapping.
- Map all `PT_LOAD` segments into the process-specific virtual address space with accurate page permissions (Read / Write / Execute / User-accessible).
- Allocate a dedicated user stack for each new user process/thread, protected by a non-present guard page at the stack bottom to catch stack overflow conditions.
- Format the initial user stack with standard System V AMD64 ABI layout containing `argc`, `argv` pointers, `envp` pointers, and string data prior to entering userspace.

### R2. Ring 3 Privilege Transition & Hardware Context Management
- Implement safe Ring 0 to Ring 3 transition using `iretq` (or architecture-standard context switch mechanism) loading appropriate User Code/Data segments (`GDT_USER_CS`, `GDT_USER_DS`).
- Configure the Task State Segment (TSS) and update `RSP0` on every thread/process context switch so that interrupts, exceptions, and syscalls originating in Ring 3 switch safely to a valid kernel-mode stack.

### R3. Process & Thread Lifecycle Subsystem
- Implement full `process_t` and `thread_t` data structures with parent-child tracking, process state (Ready, Running, Blocked, Zombie), and exit status.
- Implement process management primitives: `process_create`, `process_exec`, `process_exit`, and `process_wait`.
- Implement thread management primitives: `thread_create`, `thread_exit`, `thread_sleep`, and `yield`.
- Support parent process waiting on child termination, retrieving exit status codes, and reaping zombie processes.
- Ensure complete reclamation of all process resources (page tables, user memory mappings, allocated stacks, handles/descriptors) upon process termination without leaking memory.
- Implement foundational signal/event notification primitives adequate for inter-process synchronization in subsequent milestones.
- Strict modularity: do not directly couple filesystems, sockets, or TLS into the process core subsystem; interact strictly through abstract kernel interfaces.

## Acceptance Criteria

### Automated Build & Static Verification
- [ ] Build succeeds without compiler errors or warnings: `tools\kos-tool.exe build`.
- [ ] All code strictly follows KOS coding conventions and maintains existing architectural boundaries.

### Functionality & Execution in QEMU
- [ ] Valid ELF64 binaries load and execute in Ring 3 userspace successfully.
- [ ] Invalid/corrupted ELF binaries (wrong magic, bad program headers, out-of-bounds addresses) fail gracefully with error codes and do not panic the kernel.
- [ ] Memory protections are strictly enforced: Ring 3 cannot access kernel-only pages without triggering a page fault handled by the kernel.
- [ ] Hitting the user stack guard page cleanly faults and terminates the faulting process without crashing the kernel.
- [ ] TSS/RSP0 switching handles hardware interrupts (e.g., timer/PIT) and exceptions occurring in Ring 3 seamlessly, returning to userspace cleanly.
- [ ] Parent processes can fork/create children, retrieve child exit status via `process_wait`, and zombie processes are properly reaped upon cleanup.
- [ ] Repeated process creation and destruction loops demonstrate zero kernel heap or page frame leakage.

## Follow-up — 2026-09-18T07:27:39Z

# Teamwork Project Prompt — Milestone K16 & K17 (Storage, Block Device, Partition, VFS & FAT32)

Working directory: D:\DATA\source\OS\KOS\OS2
Integrity mode: development
Requested team: Full team

Xây dựng và hoàn thiện toàn diện tầng lưu trữ (Storage Stack), thiết bị khối (Block Device), phân vùng MBR/GPT, bộ nhớ đệm khối (Block Cache), hệ thống tệp ảo mô-đun (Modular VFS), trình điều khiển tệp tin FAT32 đầy đủ và trình quản lý ổ đĩa (Volume Manager) cho hệ điều hành KOS x86_64.

## Requirements

### R1. Universal Block Device Abstraction & Hardware Drivers
- Xây dựng abstraction chung cho block device với `read_blocks`, `write_blocks`, `flush`, `block_size`, `capacity` và quản lý trạng thái thiết bị độc lập bằng ID/đối tượng (không phụ thuộc tên ổ đĩa).
- Triển khai RAM Block Device (`ramdisk`) để kiểm thử độc lập toàn bộ storage stack.
- Triển khai VirtIO Block Driver cho QEMU (PCI discovery `0x1AF4:0x1001`, Virtqueue vring, DMA bounce buffer, timeout polling).
- Triển khai AHCI SATA Driver hỗ trợ thiết bị SATA thực và QEMU (PCI SATA controller `0x01:0x06`, ABAR mapping, Port discovery, Command List, Received FIS, Command Tables, PRDT, DMA Read/Write LBA48, Identify Device).
- Xử lý timeout, lỗi controller, lỗi đọc ghi và kiểm tra trạng thái I/O chặt chẽ.

### R2. Partition Table Parsers (MBR & GPT)
- Viết parser MBR kiểm tra signature `0x55AA`, phạm vi LBA và 4 partition entries.
- Viết parser GPT kiểm tra `EFI PART`, CRC32 header, CRC32 partition table array, backup LBA và trích xuất Partition GUID.
- Tự động phát hiện và đăng ký toàn bộ partition hợp lệ thành các child block device (ví dụ: `blk0p1`, `blk0p2`...).

### R3. Block Cache Layer
- Xây dựng block cache (512 sector / 256 KiB), bảng băm 128 bucket, chính sách thay thế LRU.
- Theo dõi dirty block, writeback flush và hàm `bcache_sync()` đồng bộ toàn cục dữ liệu xuống thiết bị phần cứng.

### R4. Modular VFS & FAT32 File System Engine
- Chuẩn hóa VFS với vnode, mount entry, file object, file descriptor table và path resolver.
- Tách biệt hoàn toàn VFS và FAT32 để hỗ trợ mở rộng thêm filesystem khác (như ext2, ramfs).
- Hoàn thiện FAT32 đọc và ghi file, quản lý FAT chain và cluster allocation, hỗ trợ file vượt nhiều cluster.
- Hỗ trợ đầy đủ CRUD: `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `readdir`, `seek`, `truncate`.
- Hỗ trợ Long File Names (LFN lên tới 255 ký tự).
- Đồng bộ FAT table, directory entry và data cluster đúng thứ tự để chống filesystem corruption.

### R5. Dynamic Volume Manager & CLI Tools
- Xây dựng Volume Manager tự động quét volume discovery trên các block device/partition, gán ký tự ổ đĩa động (`E:`, `F:`...) theo chính sách hệ thống.
- Nhận diện ổ đĩa ổn định bằng Partition GUID hoặc Volume ID.
- Tích hợp các lệnh shell chẩn đoán và thao tác: `fdisk`, `blkinfo`, `mount`, `umount`, `sync`, `touch`, `mkdir`, `rm`, `mv`.
- Bộ kiểm thử tự động toàn diện: `blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`.

## Acceptance Criteria

### Automated Verification
- [ ] `tools\kos-tool.exe build` biên dịch sạch 100% với `-Wall -Wextra -Werror` (0 warnings, 0 errors).
- [ ] Lệnh `blocktest` kiểm tra đọc/ghi/flush trên RAM disk và Block Device thành công 100%.
- [ ] Lệnh `mbrtest` tạo MBR, ghi partition table, parse và phát hiện partition thành công.
- [ ] Lệnh `gpttest` tạo GPT, tính toán CRC32 header/table, parse GUID và phát hiện partition thành công.
- [ ] Lệnh `cachetest` kiểm tra hit/miss/LRU eviction/dirty writeback và sync thành công.
- [ ] Lệnh `fattest` kiểm tra format/mount FAT32, tạo thư mục, ghi file đa cluster, đọc file, rename, xóa và kiểm tra tính toàn vẹn dữ liệu thành công 100%.
- [ ] Không rò rỉ bộ nhớ (0 leaks) và không gây panic hay lỗi kernel trong QEMU.

## Follow-up — 2026-09-18T08:10:49Z

Nhiệm vụ Người 2 (K18 & K19): Hoàn thiện toàn diện Network Stack và Socket Subsystem độc lập cho KOS (x86_64), chuẩn hóa trừu tượng hóa NIC (driver e1000 hoàn chỉnh với RX/TX ring, interrupts, buffer lifecycle), IPv4/ARP/ICMP/UDP/TCP đầy đủ và cung cấp Socket Subsystem (refcount, poll/wait queue, blocking/non-blocking) độc lập sẵn sàng cho syscall layer.

Working directory: d:/DATA/source/OS/KOS/OS2

## Requirements

### R1. Chuẩn hóa Network Interface & Hoàn thiện Driver E1000
- Tách rời network stack khỏi driver e1000 bằng trừu tượng hóa network interface (`netif` / `net_device`).
- Hoàn thiện driver e1000: hỗ trợ cấu hình RX/TX descriptor rings, ngắt (interrupts) với IMS/ICR, quản lý vòng đời buffer/packet, xử lý trạng thái lỗi và khôi phục khi ring đầy hoặc lỗi truyền nhận.

### R2. Hoàn thiện Tầng Network Core (Ethernet, ARP, IPv4 Routing, Fragmentation & ICMP)
- Ethernet layer: đóng gói, phân tích frame và dispatch theo ethertype.
- ARP: quản lý ARP cache động với thời gian hết hạn (expiration/timeout), xử lý ARP request/reply đầy đủ.
- IPv4 core: kiểm tra header validation và checksum, định tuyến IPv4 cơ bản (local, subnet, default gateway, loopback), xử lý fragmentation và reassembly ở tầng IPv4.
- ICMP: echo request/reply (ping) và xử lý/phát sinh thông điệp báo lỗi mạng (Destination Unreachable, Time Exceeded).

### R3. Hoàn thiện Giao thức UDP & Củng cố Tầng Giao vận TCP
- UDP: truyền nhận datagram tin cậy, kiểm tra checksum, port demux.
- TCP State Machine: hỗ trợ đầy đủ các trạng thái chuẩn (SYN, ACK, FIN, RST, v.v.), bắt tay 3 bước, đóng kết nối đúng chuẩn (TIME_WAIT, CLOSE_WAIT, LAST_ACK).
- TCP Reliability & Flow Control: quản lý sequence/acknowledgement numbers, receive/send sliding window, retransmission queue với RTT/RTO động (Jacobson/Karn algorithm), timer quản lý timeout.
- Congestion Control cơ bản: slow start, congestion avoidance (như Reno/Tahoe cơ bản) để TCP hoạt động ổn định khi truyền lượng dữ liệu lớn.

### R4. Thiết kế Socket Subsystem Độc lập & Hỗ trợ Concurrency/Poll
- Socket Object Lifecycle: struct socket với reference counting (`refcount`), atomic cleanup, tách rời khỏi các tầng ứng dụng (HTTP, TLS, curl, shell).
- Standard Socket Operations: hỗ trợ đầy đủ `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `shutdown()`, `getsockopt()`, `setsockopt()`.
- Blocking & Non-blocking: hỗ trợ cờ non-blocking (O_NONBLOCK), timeout trên send/recv.
- Event Notification / Poll: tích hợp wait queue / poll mechanism (`poll` / `select` readiness notification) để luồng người dùng hoặc kernel thread không phải busy wait.
- Socket Concurrency: hỗ trợ nhiều socket hoạt động đồng thời một cách an toàn (đồng bộ hóa bằng spinlock/mutex).
- Cung cấp API nội bộ sạch cho syscall layer (không trực tiếp sửa syscall dispatcher của Người 5).

## Verification Resources & Strategy
- Build tool: `tools/kos-tool.exe build` (hoặc compiler clang/gcc theo cấu hình KOS).
- Test environment: QEMU với e1000 NIC và user-mode / tap network.
- Kernel terminal / self-tests: các lệnh kiểm tra mạng nội bộ (`netinfo`, `ping`, `udptest`, TCP echo test client/server).

## Acceptance Criteria

### Build & Driver Cleanliness
- [ ] Kernel biên dịch thành công không có warning/error (`tools\kos-tool.exe build` sạch).
- [ ] Network stack không còn gọi trực tiếp hàm nội bộ của e1000; e1000 đăng ký qua cấu trúc `net_device` / `netif`.
- [ ] Driver e1000 xử lý RX/TX qua descriptor ring an toàn, không rò rỉ memory buffer hay treo khi traffic tăng.

### Network Protocol Correctness
- [ ] ARP cache tự động cập nhật khi nhận packet, tự loại bỏ entry hết hạn sau timeout cấu hình.
- [ ] Gửi và nhận ping ICMP thành công giữa KOS và QEMU gateway (hoặc host), RTT đo đạc chính xác.
- [ ] IPv4 packet lớn hơn MTU được phân mảnh (fragmented) và ghép lại (reassembled) thành công ở đích.
- [ ] TCP state machine chuyển đổi mượt mà và xử lý đúng mọi cờ SYN/ACK/FIN/RST; đóng kết nối dọn sạch socket và chuyển qua TIME_WAIT đúng thời lượng.
- [ ] Retransmission kích hoạt chính xác khi bị mất gói hoặc không nhận được ACK trong khoảng RTO tính toán động.

### Socket Subsystem & Concurrency
- [ ] Socket subsystem hoạt động độc lập; nhiều socket (UDP & TCP) mở và trao đổi dữ liệu đồng thời không xung đột.
- [ ] Socket blocking đưa thread vào trạng thái ngủ qua wait queue/event; socket non-blocking trả về `EWOULDBLOCK`/`EAGAIN` đúng chuẩn.
- [ ] Cơ chế poll/event readiness phản hồi chính xác trạng thái readable/writable/error mà không cần busy polling.
- [ ] Socket lifecycle: đóng socket giảm refcount, tài nguyên chỉ giải phóng khi refcount = 0.
- [ ] Cung cấp header/API nội bộ rõ ràng cho Người 5 tích hợp syscall mà không xâm lấn dispatcher.

## Follow-up — 2026-09-18T08:38:37Z

This is a single self-contained fix; keep it small and focused.

# Teamwork Project Prompt — Draft

> Status: Launched
> Goal: Multi-agent execution in progress
> Requested team: Small focused team

Nhiệm vụ Người 2 (K18 & K19): Hoàn thiện toàn diện Network Stack và Socket Subsystem độc lập cho KOS (x86_64), chuẩn hóa trừu tượng hóa NIC (driver e1000 hoàn chỉnh với RX/TX ring, interrupts, buffer lifecycle), IPv4/ARP/ICMP/UDP/TCP đầy đủ và cung cấp Socket Subsystem (refcount, poll/wait queue, blocking/non-blocking) độc lập sẵn sàng cho syscall layer.

Working directory: d:/DATA/source/OS/KOS/OS2

## Requirements

### R1. Chuẩn hóa Network Interface & Hoàn thiện Driver E1000
- Tách rời network stack khỏi driver e1000 bằng trừu tượng hóa network interface (`netif` / `net_device`).
- Hoàn thiện driver e1000: hỗ trợ cấu hình RX/TX descriptor rings, ngắt (interrupts) với IMS/ICR, quản lý vòng đời buffer/packet, xử lý trạng thái lỗi và khôi phục khi ring đầy hoặc lỗi truyền nhận.

### R2. Hoàn thiện Tầng Network Core (Ethernet, ARP, IPv4 Routing, Fragmentation & ICMP)
- Ethernet layer: đóng gói, phân tích frame và dispatch theo ethertype.
- ARP: quản lý ARP cache động với thời gian hết hạn (expiration/timeout), xử lý ARP request/reply đầy đủ.
- IPv4 core: kiểm tra header validation và checksum, định tuyến IPv4 cơ bản (local, subnet, default gateway, loopback), xử lý fragmentation và reassembly ở tầng IPv4.
- ICMP: echo request/reply (ping) và xử lý/phát sinh thông điệp báo lỗi mạng (Destination Unreachable, Time Exceeded).

### R3. Hoàn thiện Giao thức UDP & Củng cố Tầng Giao vận TCP
- UDP: truyền nhận datagram tin cậy, kiểm tra checksum, port demux.
- TCP State Machine: hỗ trợ đầy đủ các trạng thái chuẩn (SYN, ACK, FIN, RST, v.v.), bắt tay 3 bước, đóng kết nối đúng chuẩn (TIME_WAIT, CLOSE_WAIT, LAST_ACK).
- TCP Reliability & Flow Control: quản lý sequence/acknowledgement numbers, receive/send sliding window, retransmission queue với RTT/RTO động (Jacobson/Karn algorithm), timer quản lý timeout.
- Congestion Control cơ bản: slow start, congestion avoidance (như Reno/Tahoe cơ bản) để TCP hoạt động ổn định khi truyền lượng dữ liệu lớn.

### R4. Thiết kế Socket Subsystem Độc lập & Hỗ trợ Concurrency/Poll
- Socket Object Lifecycle: struct socket với reference counting (`refcount`), atomic cleanup, tách rời khỏi các tầng ứng dụng (HTTP, TLS, curl, shell).
- Standard Socket Operations: hỗ trợ đầy đủ `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `shutdown()`, `getsockopt()`, `setsockopt()`.
- Blocking & Non-blocking: hỗ trợ cờ non-blocking (O_NONBLOCK), timeout trên send/recv.
- Event Notification / Poll: tích hợp wait queue / poll mechanism (`poll` / `select` readiness notification) để luồng người dùng hoặc kernel thread không phải busy wait.
- Socket Concurrency: hỗ trợ nhiều socket hoạt động đồng thời một cách an toàn (đồng bộ hóa bằng spinlock/mutex).
- Cung cấp API nội bộ sạch cho syscall layer (không trực tiếp sửa syscall dispatcher của Người 5).

## Verification Resources & Strategy
- Build tool: `tools/kos-tool.exe build` (hoặc compiler clang/gcc theo cấu hình KOS).
- Test environment: QEMU với e1000 NIC và user-mode / tap network.
- Kernel terminal / self-tests: các lệnh kiểm tra mạng nội bộ (`netinfo`, `ping`, `udptest`, TCP echo test client/server).

## Acceptance Criteria

### Build & Driver Cleanliness
- [ ] Kernel biên dịch thành công không có warning/error (`tools\kos-tool.exe build` sạch).
- [ ] Network stack không còn gọi trực tiếp hàm nội bộ của e1000; e1000 đăng ký qua cấu trúc `net_device` / `netif`.
- [ ] Driver e1000 xử lý RX/TX qua descriptor ring an toàn, không rò rỉ memory buffer hay treo khi traffic tăng.

### Network Protocol Correctness
- [ ] ARP cache tự động cập nhật khi nhận packet, tự loại bỏ entry hết hạn sau timeout cấu hình.
- [ ] Gửi và nhận ping ICMP thành công giữa KOS và QEMU gateway (hoặc host), RTT đo đạc chính xác.
- [ ] IPv4 packet lớn hơn MTU được phân mảnh (fragmented) và ghép lại (reassembled) thành công ở đích.
- [ ] TCP state machine chuyển đổi mượt mà và xử lý đúng mọi cờ SYN/ACK/FIN/RST; đóng kết nối dọn sạch socket và chuyển qua TIME_WAIT đúng thời lượng.
- [ ] Retransmission kích hoạt chính xác khi bị mất gói hoặc không nhận được ACK trong khoảng RTO tính toán động.

### Socket Subsystem & Concurrency
- [ ] Socket subsystem hoạt động độc lập; nhiều socket (UDP & TCP) mở và trao đổi dữ liệu đồng thời không xung đột.
- [ ] Socket blocking đưa thread vào trạng thái ngủ qua wait queue/event; socket non-blocking trả về `EWOULDBLOCK`/`EAGAIN` đúng chuẩn.
- [ ] Cơ chế poll/event readiness phản hồi chính xác trạng thái readable/writable/error mà không cần busy polling.
- [ ] Socket lifecycle: đóng socket giảm refcount, tài nguyên chỉ giải phóng khi refcount = 0.
- [ ] Cung cấp header/API nội bộ rõ ràng cho Người 5 tích hợp syscall mà không xâm lấn dispatcher.

## Follow-up — 2026-09-18T09:30:36Z

# Teamwork Project Prompt — Milestone K13 & K14 (Address Space Isolation, Ring 3 Userspace & ELF Loader)

Working directory: D:\DATA\source\OS\KOS\OS2
Integrity mode: development
Requested team: Full team

Xây dựng và hoàn thiện phân tầng không gian địa chỉ độc lập (Per-process Isolated Address Space), trình nạp tệp thực thi ELF (ELF Binary Loader), cơ chế chuyển đổi mức đặc quyền Ring 3 (User-Mode Execution) và hệ thống lời gọi hệ thống (Syscall ABI & Runtime) cho hệ điều hành KOS x86_64.

## Requirements

### R1. Per-Process Isolated Address Spaces & Memory Safety
- Tạo đối tượng `struct address_space` riêng biệt cho từng `struct process` với bảng trang PML4 độc lập.
- Ánh xạ Higher-Half Direct Map (HHDM) và Kernel Space đồng bộ trên mọi tiến trình, trong khi User Space (`0x00000000_00000000` .. `0x00007FFF_FFFFFFFF`) được cô lập hoàn toàn.
- Thiết lập quyền trang chặt chẽ: `VMM_PAGE_USER`, `VMM_PAGE_WRITABLE`, `VMM_PAGE_NO_EXECUTE` ($W \oplus X$).
- Triển khai các hàm sao chép dữ liệu an toàn có kiểm tra phạm vi vùng nhớ người dùng: `copy_from_user`, `copy_to_user`, `strndup_from_user` (tuyệt đối không dereference trực tiếp con trỏ Ring 3).

### R2. ELF64 Binary Loader & Process Memory Layout
- Nâng cấp `fs/elf.c` thành trình nạp chương trình hoàn chỉnh: phân tích ELF64 Header, nạp các phân đoạn `PT_LOAD` vào không gian địa chỉ của tiến trình đích.
- Cấp phát và ánh xạ ngăn xếp người dùng (User Stack, ví dụ 64 KiB tại `0x00007FFF_FFFF0000`) kèm Guard Page không ánh xạ để chống stack overflow.
- Thiết lập đối số dòng lệnh `argc`, `argv`, `envp` trên user stack theo chuẩn System V AMD64 ABI.

### R3. Ring 3 Privilege Transition & Context Switching
- Cấu hình GDT với User Code Segment (`0x2B`, DPL 3) và User Data Segment (`0x33`, DPL 3).
- Cấu hình TSS với `rsp0` (Kernel Stack) để CPU tự động chuyển sang stack kernel an toàn khi xảy ra ngắt hoặc syscall từ Ring 3.
- Cập nhật Context Switch để chuyển đổi `CR3` (PML4) và cập nhật `TSS.rsp0` tương ứng với từng luồng/tiến trình.
- Triển khai hàm `enter_user_mode(entry_point, user_stack_top, argc, argv)` thông qua lệnh `iretq` với đầy đủ `CS`, `SS`, `RFLAGS` (IF=1), `RIP`, `RSP`.

### R4. Syscall Boundary & Core ABI
- Chuẩn hóa giao tiếp Syscall qua `int 0x80` (DPL 3) và mở rộng hướng `syscall`/`sysret` (MSR LSTAR/STAR/SFMASK).
- Đăng ký và triển khai bảng Syscall cốt lõi:
  - `SYS_EXIT` (1): Kết thúc tiến trình, gửi mã thoát và đánh thức tiến trình cha.
  - `SYS_WRITE` (2): Ghi dữ liệu ra standard output / terminal console an toàn.
  - `SYS_READ` (3): Đọc dữ liệu từ standard input / bàn phím.
  - `SYS_GETTIME` (4): Lấy thời gian kernel tick / monotonic timestamp.
  - `SYS_SYSINFO` (5): Truy vấn thống kê bộ nhớ và hệ thống.
  - `SYS_YIELD` (6): Nhường CPU cho luồng khác.
  - `SYS_SLEEP` (7): Tạm dừng tiến trình theo mili-giây.
- Đảm bảo khi một tiến trình Ring 3 bị lỗi (Page Fault, Divide Error, General Protection), kernel sẽ bắt giữ, chấm dứt tiến trình lỗi mà không làm crash hay panic toàn bộ hệ điều hành.

### R5. User-Space Binary Smoke Test & Regression
- Xây dựng chương trình người dùng mẫu (User-Mode ELF Binary, ví dụ: `hello.elf`, `sh.elf`) chạy ở Ring 3.
- Tích hợp lệnh shell `exec <path>` hoặc `usertest` để chạy chương trình Ring 3 và xác thực:
  - Chạy ở CPL 3 (User Mode).
  - Gọi thành công các syscall `sys_write`, `sys_gettime`, `sys_exit`.
  - Thu hồi 100% bộ nhớ trang người dùng và cấu trúc tiến trình khi kết thúc (0 memory leak).

## Acceptance Criteria

### Automated Verification
- [ ] `tools\kos-tool.exe build` biên dịch sạch 100% với `-Wall -Wextra -Werror` (0 warnings, 0 errors).
- [ ] Lệnh `usertest` hoặc nạp ELF Ring 3 thực thi thành công từ đầu đến cuối trên QEMU q35.
- [ ] Tiến trình Ring 3 gọi `sys_write` in chuỗi ký tự ra màn hình/serial console chính xác.
- [ ] Thử nghiệm cố tình gây lỗi trong Ring 3 (dereference `NULL` hoặc địa chỉ kernel) bị kernel chặn an toàn, tiến trình kết thúc sạch sẽ và kernel tiếp tục hoạt động bình thường.
- [ ] Toàn bộ bộ nhớ của tiến trình được giải phóng hoàn toàn sau khi thoát (PMM frame count và Heap allocation count trở về nguyên vẹn, 0 leaks).

