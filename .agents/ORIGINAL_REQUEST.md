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

