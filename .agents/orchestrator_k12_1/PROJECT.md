# Project: KOS Kernel Stabilization (Milestone K12)

## Architecture
- Target: x86_64, freestanding C17 + NASM x86_64, UEFI Limine bootloader, QEMU q35 cpu max, e1000 network card.
- Build system: `.\tools\kos-tool.exe build` (Clang, LLD, NASM).
- Existing modules:
  - Logging (K12.0): 64 KiB ring buffer in RAM, serial COM1 emergency polling, `dmesg` shell command.
  - Exception & IDT (K12.1): 256 vector IDT, 176-byte `struct interrupt_context` AMD64 ABI, IST1 double fault stack (16 KiB), dynamic exception handler registry.
  - PMM, VMM, Heap: Physical memory allocator (`mm/pmm.c`), paging/page tables (`mm/vmm.c`), dynamic heap allocator (`mm/heap.c`).
  - VFS & RAMFS: File system abstraction (`fs/vfs.c`) and in-memory filesystem (`fs/ramfs.c`).
  - Network: e1000 driver (`drivers/e1000.c`), ARP (`net/arp.c`), DHCP, DNS, UDP, TCP (`net/tcp.c`, `net/socket.c`).
  - Shell / Terminal: Built-in kernel shell commands (`kernel/terminal.c` / `kernel/shell.c`).

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | K12.2 Page Fault Handler | CR2 read on entry, full error code decode (P, W/R, U/S, RSVD, I/D, PK, SS), panic on kernel mode fault, unmap page 0, shell command `pftest` | Phase 1 | ORIGINAL_REQUEST §Phase 1 |
| 2 | K12.3 Kernel Panic Subsystem | APIs `panic()`, `panicf()`, `panic_with_frame()`, recursion guard `panic_in_progress`, banner, uptime, CPU id, TID/PID, RIP/RSP/RBP/CR2/CR3, reg dump, serial COM1 emergency polling, shell command `panic-test` | Phase 1 | ORIGINAL_REQUEST §Phase 1 |
| 3 | K12.4 Stack Trace | Clang `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`, RBP chain traversal (alignment, canonical address, stack bounds, frame limit), symbol table resolution, shell command `backtrace` | Phase 1 | ORIGINAL_REQUEST §Phase 1 |
| 4 | K12.5 Atomic Primitives | Clang 32/64-bit builtins, `refcount_t` with underflow detection, shell command `atomictest` | Phase 2 | ORIGINAL_REQUEST §Phase 2 |
| 5 | K12.6 Spinlock irqsave | `spinlock_t`, `spin_lock_irqsave()` and `spin_unlock_irqrestore()` preserving RFLAGS IF, `cpu_pause()`, owner tracking, shell command `locktest` | Phase 2 | ORIGINAL_REQUEST §Phase 2 |
| 6 | K12.16 Wait Queue | `wait_queue_head_t`, non-lost wakeup queue | Phase 2 | ORIGINAL_REQUEST §Phase 2 |
| 7 | K12.7 Mutex | `mutex_t`, sleep-blocking `THREAD_STATE_BLOCKED`, FIFO wake, `assert_can_sleep()` assert in ISR, shell command `mutextest` | Phase 2 | ORIGINAL_REQUEST §Phase 2 |
| 8 | K12.8 Standardized Timer Subsystem | PIT 1000 Hz (1ms tick), monotonic 64-bit clock, software timer queue, `thread_sleep(uint64_t ms)` | Phase 3 | ORIGINAL_REQUEST §Phase 3 |
| 9 | K12.9 Preemptive Multitasking & Scheduler | Preemptive Round-Robin (10ms quantum), `need_resched`, `preempt_disable()`/`preempt_enable()` with nesting counter, idle thread (`sti; hlt`), reaper thread, `task_switch.asm` full context save/restore, scheduler stats, shell command `taskstress` | Phase 4 | ORIGINAL_REQUEST §Phase 4 |
| 10 | K12.10-12 struct thread & struct process | `struct thread` (TID 64-bit, state enum, own stack, slice), `struct process` (PID 64-bit, thread list, handle table, refcount, exit status) | Phase 5 | ORIGINAL_REQUEST §Phase 5 |
| 11 | K12.14-15 Kernel Object Model & Handle Table | `kobject` model (THREAD, PROCESS, FILE, SOCKET, EVENT), refcount, auto destructor; Handle table 32-bit (index+generation), rights check, shell command `handletest` | Phase 5 | ORIGINAL_REQUEST §Phase 5 |
| 12 | K12.13 Memory Hardening | Upgrade `heap_lock`, `pmm_lock`, `vmm_lock` to spinlock irqsave, heap poison/metadata/canary for double free detection | Phase 6 | ORIGINAL_REQUEST §Phase 6 |
| 13 | K12.19-20 VFS & Network Hardening | `vfs_lock`, `ramfs_lock`, `arp_lock`, socket tables locks, e1000 preemption safe | Phase 6 | ORIGINAL_REQUEST §Phase 6 |
| 14 | K12.21-25 Stress Testing & Verification | Shell commands `atomictest`, `locktest`, `mutextest`, `taskstress`, `handletest`, `backtrace`, `pftest`, `panic-test`, `kstress`; multi-cycle workload stress; `docs/k12_report.md` | Phase 7 | ORIGINAL_REQUEST §Phase 7 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Phase 1: Diagnostics & Fault Handling | K12.2 Page Fault Handler, K12.3 Kernel Panic Subsystem, K12.4 Stack Trace | none | IN_PROGRESS |
| 2 | Phase 2: Synchronization Primitives | K12.5 Atomic primitives, K12.6 Spinlock irqsave, K12.16 Wait Queue, K12.7 Mutex | Phase 1 | PLANNED |
| 3 | Phase 3: Standardized Timer Subsystem | K12.8 PIT 1000 Hz, thread_sleep | Phase 2 | PLANNED |
| 4 | Phase 4: Preemptive Multitasking & Scheduler | K12.9 Preemptive RR, quantum 10ms, preempt counter, idle thread, reaper thread, task_switch.asm, taskstress | Phase 3 | PLANNED |
| 5 | Phase 5: Process, Thread, Object Model & Handle Table | K12.10 - K12.15 struct thread, struct process, kobject model, handle table, handletest | Phase 4 | PLANNED |
| 6 | Phase 6: Hardening Memory, VFS & Network | K12.13, K12.19, K12.20 memory locks irqsave, heap poison/metadata, vfs/ramfs/arp/socket locks, e1000 preemption safe | Phase 5 | PLANNED |
| 7 | Phase 7: Stress Testing & Verification | K12.21 - K12.25 shell commands atomictest, locktest, mutextest, taskstress, handletest, backtrace, pftest, panic-test, kstress; docs/k12_report.md | Phase 6 | PLANNED |

## Interface Contracts
### Phase 1 Diagnostics ↔ Kernel
- `kernel/fault.c`, `include/kos/fault.h`: `void fault_init(void);`, `void page_fault_handler(struct interrupt_context *ctx);`
- `kernel/panic.c`, `include/kos/panic.h`: `_Noreturn void panic(const char *message);`, `_Noreturn void panicf(const char *fmt, ...);`, `_Noreturn void panic_with_frame(const char *message, const struct interrupt_context *frame);`
- `kernel/stacktrace.c`, `include/kos/stacktrace.h`: `void stacktrace_dump(uint64_t max_frames);`, `void stacktrace_dump_frame(uint64_t rbp, uint64_t rip, uint64_t max_frames);`

## Code Layout
- Kernel headers: `include/kos/`
- Kernel core source: `kernel/`
- Architecture x86_64: `arch/x86_64/`
- Memory management: `mm/`
- Filesystem: `fs/`
- Networking: `net/`
- Device drivers: `drivers/`
- Shell: `kernel/terminal.c`
- Build coordinator: `tools/kos-tool.exe`
- Documentation: `docs/`
