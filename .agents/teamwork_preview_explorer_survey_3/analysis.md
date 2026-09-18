# Comprehensive Survey & Analysis: Process/Thread Lifecycle, Scheduler Integration, Resource Reclamation & Verification Harness for Milestone K14

**Surveyor**: `teamwork_preview_explorer_survey_3`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_3`  
**Target Milestone**: K14 (Process and Ring 3 Userspace Subsystem)  
**Date**: 2026-09-18  

---

## Executive Summary

This survey provides a comprehensive architectural and code-level investigation of the KOS operating system (x86_64) across its process/thread subsystems, multitasking scheduler, virtual memory manager, resource reclamation, synchronization mechanisms, build toolchain, and test harness.

### Core Discoveries
1. **Model Representation**: Scheduler and thread management are implemented in `kernel/task.c` and declared in `include/kos/task.h` (using `#define thread task` and `typedef struct task thread_t`). High-level process structures are implemented in `kernel/process.c` and declared in `include/kos/process.h`. Separate files `sched.c`, `thread.c`, `sched.h`, `thread.h` do not exist.
2. **Missing Primitives**: `process_exec` does not exist in the codebase. Parent-child relationships are tracked only as a flat `ppid` integer; there is no child list, no sibling list, no orphan reparenting, and no `waitpid(-1)` equivalent.
3. **Paging & Isolation Gaps**: Every process currently points its `proc->pml4_physical` to the shared kernel CR3. Virtual memory (`mm/vmm.c`) lacks `VMM_PAGE_USER` (bit 2) support, rejects any flags other than WRITABLE and NO_EXECUTE, and has no per-process address space allocator or recursive teardown function.
4. **Context Switching Gaps**: Neither `schedule()` nor `task_switch.asm` switches CR3 or updates TSS `rsp0` during a thread/process switch. User-to-kernel stack switching relies on TSS `rsp0`, which is currently hardcoded once at boot to `kos_boot_stack_top`.
5. **Premature Zombie Deletion**: In `kernel/process.c`, the dedicated `reaper_thread` automatically calls `process_destroy(proc)` as soon as the last thread exits, moving the process to `PROCESS_STATE_DEAD`. This causes a race condition where a child's slot can be reused before a waiting parent calls `process_wait()`.
6. **Fatal Exception Behavior in User Mode**: The current page fault handler (`arch/x86_64/interrupts.c:267`) halts the CPU (`cpu_halt()`) on any user-mode fault. For K14, this must terminate the faulting process without crashing the kernel.
7. **Boot Regression in Network Wait Queue**: During boot, `net_udp_open()` (`net/socket.c:186`) zeroes out `struct udp_socket`, corrupting `wait_queue.head`. When loopback self-test sends a datagram, `wait_queue_wake_all()` dereferences NULL (CR2 = 0x0) causing a fatal kernel panic.

---

## 1. Existing Process/Thread Model

### 1.1 Structural Organization
The subsystem is partitioned into two layers:
- **Low-level Task/Thread layer** (`include/kos/task.h`, `kernel/task.c`):
  - Manages CPU registers, kernel stack allocation, scheduler runqueues, timer sleep queue, preemption counters, and hardware context switching.
  - Type aliasing:
    ```c
    #define thread task
    typedef struct task thread_t;
    typedef struct task task_t;
    ```
- **High-level Process layer** (`include/kos/process.h`, `kernel/process.c`):
  - Manages PIDs, PPIDs, owning thread list, memory PML4 pointer, handle table (`struct handle_table *`), and process termination wait queue (`struct wait_queue exit_wq`).

### 1.2 Thread Structure (`struct task` / `thread_t`)
Defined in `include/kos/task.h:64-83`:
```c
struct task {
    struct task_context context;         /* Callee-saved registers + rip/rsp/rflags */
    task_entry entry;                   /* Thread entry function (task_entry = void (*)(void *)) */
    void *argument;                     /* User argument passed to entry */
    uint8_t *stack;                     /* Base pointer to allocated kernel stack */
    size_t stack_size;                  /* Kernel stack size (default 16 KiB = 4 pages) */
    const char *name;                   /* Descriptive thread name */
    volatile enum thread_state state;   /* Current thread lifecycle state */
    uint64_t tid;                       /* Monotonic 64-bit thread ID */
    uint64_t id;                        /* Alias for tid (backward compatibility) */
    struct process *process;            /* Pointer to owning parent process */
    struct list_head process_node;      /* Node linking thread into process->threads */
    int exit_code;                      /* Thread exit code */
    uint64_t sleep_target_tick;         /* Target tick for timer-based sleep */
    struct list_head sleep_node;        /* Node linking thread into sleep_queue */
    uint32_t preempt_count;             /* Preemption disable nesting depth */
    bool reschedule_pending;            /* Reschedule flag when preemption disabled */
    uint32_t quantum_remaining;         /* Time slice remaining (default 20 ticks) */
    struct list_head reaper_node;       /* Node linking thread into reaper_queue */
};
```
*Key Missing Fields for K14*:
- No `uint64_t user_stack_top` or `uint64_t user_stack_base`.
- No flag distinguishing kernel threads vs user-mode threads.
- No `struct interrupt_context *user_ctx` for saved user registers during preemption.

### 1.3 Process Structure (`struct process` / `process_t`)
Defined in `include/kos/process.h:27-40`:
```c
struct process {
    uint64_t pid;                       /* Monotonic process ID (0=idle, 1=kernel, 2+=user) */
    char name[PROCESS_NAME_MAX];        /* Name string (max 32 chars) */
    uint64_t ppid;                      /* Parent PID */
    enum process_state state;           /* Process state */
    int exit_code;                      /* Exit status code returned to parent */
    struct list_head threads;           /* Head of list of threads owned by process */
    uint32_t thread_count;              /* Count of active threads in process */
    uint64_t pml4_physical;             /* Physical address of PML4 page table */
    kos_spinlock_t lock;                /* Spinlock protecting process structure */
    struct list_head process_node;      /* Global list node in process_list */
    struct handle_table *handles;       /* 64-slot object handle table */
    struct wait_queue exit_wq;          /* Wait queue awakened on process exit */
};
```
*Key Missing Fields for K14*:
- No `struct list_head children` (list of child processes).
- No `struct list_head sibling_node` (linking into parent's `children` list).
- No memory region tracking (e.g. user heap break, text/data boundaries, user stack guard page address).
- No signal delivery or event notification mask.

### 1.4 States and Transitions
#### Thread States (`enum thread_state` / `enum task_state`)
- `THREAD_STATE_UNUSED (0)`: Slot in static array `tasks[64]` is unallocated.
- `THREAD_STATE_RUNNABLE (1)`: Thread is ready to run, schedulable.
- `THREAD_STATE_RUNNING (2)`: Thread is actively executing on the CPU.
- `THREAD_STATE_BLOCKED (3)`: Blocked waiting on mutex, wait queue, or event.
- `THREAD_STATE_SLEEPING (4)`: Enqueued in `sleep_queue` waiting for timer tick deadline.
- `THREAD_STATE_ZOMBIE (5)`: Thread has exited (`thread_exit()`), enqueued in `reaper_queue`. Stack and descriptors remain intact until reaper thread processes it.
- `THREAD_STATE_DEAD (6)`: Stack has been freed by reaper thread; slot can be recycled.

#### Process States (`enum process_state`)
- `PROCESS_STATE_UNUSED (0)`: Slot in static array `processes[32]` is free.
- `PROCESS_STATE_ACTIVE (1)`: Process is alive and executing threads.
- `PROCESS_STATE_ZOMBIE (2)`: All non-current threads terminated, handles closed, exit code set, waiting for parent reap.
- `PROCESS_STATE_DEAD (3)`: All threads reaped, unlinked from global list.

---

## 2. Process/Thread Hierarchy & Lifecycle

### 2.1 Hierarchy Tracking
- **Parent-Child Linkage**: `process_create()` records `proc->ppid = cur ? cur->pid : 1`. However, there is no bidirectional linkage:
  - Parent does NOT keep a linked list of children.
  - There is no mechanism for a parent to wait on *any* child process (e.g., `waitpid(-1)`).
  - When a parent process terminates, children are **not reparented** to PID 1 (`kernel_process`). If the parent dies, orphaned children retain a stale PPID.
- **Lookup Mechanisms**:
  - `process_find_by_pid(uint64_t pid)` traverses the global `process_list` under `process_lock`.
  - PIDs 0 (`idle_process`) and 1 (`kernel_process`) are statically allocated singletons.

### 2.2 Lifecycle Primitives Implementation

| Primitive | Location | Existing Implementation Summary | Gaps & Issues |
|---|---|---|---|
| `process_create` | `kernel/process.c:115` | Allocates slot in `processes[32]`, assigns PID, sets `ppid`, inits thread list, allocates handle table (`handle_table_create`), inits `exit_wq`, sets `pml4_physical = cpu_read_cr3() & ~0xfff`. | Does NOT allocate isolated page tables (shares kernel CR3). Does not link into parent's child list. |
| `process_exec` | *None* | **Does not exist**. | Must be implemented to parse ELF64 binaries, map `PT_LOAD` segments, allocate user stack with guard page, setup System V ABI stack, and start execution. |
| `process_exit` | `kernel/process.c:183` | Sets state `PROCESS_STATE_ZOMBIE`, closes handle table (`handle_table_destroy`), enqueues non-current threads to `reaper_queue`, wakes `exit_wq`, and if current thread belongs to proc, calls `thread_exit()`. | If `thread_count == 0`, immediately calls `process_destroy()`, making the process DEAD before parent can read exit code. |
| `process_wait` | `kernel/process.c:245` | Uses `wait_event_timeout` on `proc->exit_wq` waiting for `proc->state == ZOMBIE || DEAD`. Copies `*exit_code = proc->exit_code`. | Does not reap the process (does not mark DEAD or free slot). Only waits on a specific `struct process *`, not arbitrary PID or any child. |
| `thread_create` | `kernel/process.c:270` | Allocates slot in `tasks[64]`, allocates 16 KiB kernel stack via `kmalloc`, sets `rsp` to top, sets `rip = task_start_trampoline`, sets `rflags = 0x202`. Adds to `proc->threads`. | Cannot create user threads. No user stack allocation. |
| `thread_exit` | `kernel/process.c:383` | Disallows PID 0/1 exit. Sets state `THREAD_STATE_ZOMBIE`, adds to `reaper_queue`, unblocks `reaper_thread`, calls `schedule()`. | Clean kernel thread exit, but needs user stack deallocation hook for user threads. |
| `thread_sleep` / `task_sleep` | `kernel/task.c:394` | Converts ms to ticks, inserts into `sleep_queue` sorted by deadline, sets state `THREAD_STATE_SLEEPING`, yields CPU via `schedule()`. | Fully functional. |
| `yield` / `task_yield` | `kernel/task.c:305` | Calls `schedule()`. | Fully functional. |

---

## 3. Resource Reclamation

### 3.1 Existing Reclamation Flow
1. **Thread Stack Reclamation via Dedicated Reaper Thread**:
   - Implemented in `kernel/process.c:332-381` (`reaper_thread_entry`).
   - A dedicated kernel thread (`"reaper"`) runs on its own independent 16 KiB stack.
   - When any thread calls `thread_exit()`, it transitions itself to `THREAD_STATE_ZOMBIE`, pushes itself onto `reaper_queue`, unblocks `reaper_thread`, and calls `schedule()`.
   - The reaper pops each zombie thread:
     - Changes state to `THREAD_STATE_DEAD`.
     - Calls `kfree(zombie->stack)` and sets `zombie->stack = NULL`.
     - Decrements `proc->thread_count`.
     - Increments `reaper_reaped_total`.
   - **Architectural Strength**: Thread stacks are NEVER freed from the context of the running thread. The reaper safely reclaims them outside their execution context.

2. **Handle Table & Object Reclamation**:
   - Implemented in `kernel/handle.c:30-60` (`handle_table_destroy`).
   - When `process_exit()` or `process_destroy()` is called, `handle_table_destroy()` scans all 64 handle entries.
   - Collects all valid `struct kos_object_header *` pointers into a temporary array under `table->lock`.
   - Iterates through the collected objects and calls `kos_object_unref(obj)` **outside** the spinlock to avoid deadlock during destruction.
   - If an object's reference count drops to zero, its registered `destructor` callback is invoked (e.g. `event_destructor` in `kernel/wait.c`, `mutex_destructor`, `file_destructor`), freeing memory.
   - Finally frees the handle table structure via `kfree(table)`.

3. **Page Tables & Virtual Memory Reclamation**:
   - **Current State**: **Zero page table reclamation exists for processes**.
   - `mm/vmm.c` has `vmm_unmap_page()` and `release_owned_empty_table()`, but:
     - Only single pages can be unmapped sequentially.
     - `vmm.pml4` is a global variable pointing solely to kernel CR3.
     - There is no function to recursively free a process's user page table tree (PML4 -> PDPT -> PD -> PT -> user pages).

4. **Zombie Reaping Race Condition**:
   - In `kernel/process.c:367-372`:
     ```c
     bool should_destroy = (proc->state == PROCESS_STATE_ZOMBIE && proc->thread_count == 0);
     if (should_destroy) {
         process_destroy(proc);
     }
     ```
   - When the reaper thread reaps the last thread of a zombie process, it immediately calls `process_destroy(proc)`.
   - In `process_destroy(proc)`:
     ```c
     proc->state = PROCESS_STATE_DEAD;
     list_del_init(&proc->process_node);
     ```
   - Once marked `PROCESS_STATE_DEAD`, the slot in `processes[]` is considered free by `process_create()`:
     ```c
     if (processes[i].state == PROCESS_STATE_UNUSED || processes[i].state == PROCESS_STATE_DEAD)
     ```
   - If a new process is created before the parent calls `process_wait()`, the slot is overwritten, clobbering `exit_code` and corrupting `exit_wq`.
   - **Required Fix**: `proc` must remain in `PROCESS_STATE_ZOMBIE` until the parent reaps it via `process_wait()`. Only `process_wait()` should call `process_destroy()`. If the parent exits while the child is a zombie, the reaper or init process should reap it.

---

## 4. Scheduler Integration

### 4.1 Current Scheduler Mechanics
- **Scheduling Algorithm**: Round-robin over static `tasks[64]` pool (`task_find_next_ready()` in `kernel/task.c:108-118`).
- **Quantum**: Default 20 ticks (20 ms at 1000 Hz or 200 ms at 100 Hz).
- **Preemption**:
  - PIT timer interrupt calls `task_timer_tick()`.
  - If quantum reaches 0 and `current_task->preempt_count == 0`, `reschedule_requested` is set to true.
  - On interrupt return, `task_reschedule_if_needed()` calls `schedule()`.
  - Preemption can be disabled via `preempt_disable()` and enabled via `preempt_enable()`.

### 4.2 Critical Architectural Deficiencies for Ring 3
1. **No CR3 Switching**:
   - In `task_switch.asm`, only callee-saved registers (`rbx, rbp, r12, r13, r14, r15, rsp, rip, rflags`) are swapped.
   - `schedule()` in `kernel/task.c:261-303` never checks if `next->process != previous->process`.
   - CR3 is never reloaded.
   - **Fix**: When `next->process != previous->process` and `next->process->pml4_physical != 0`, execute `mov cr3, rax` to switch address space.
2. **No TSS RSP0 Updating**:
   - In x86_64, when a hardware interrupt, exception, or syscall occurs in Ring 3 (CPL=3), the CPU switches hardware stack to `TSS.rsp0`.
   - Currently, `tss_set_rsp0()` is called exactly ONCE during boot in `gdt_initialize()` (`arch/x86_64/descriptors.c:81`) pointing to `kos_boot_stack_top`.
   - It is never updated when switching tasks.
   - **Impact**: If any interrupt occurs while running in Ring 3, the CPU would push the interrupt frame onto the boot stack rather than the thread's own kernel stack, corrupting kernel state and causing concurrency crashes.
   - **Fix**: On every context switch to a thread, update `tss_set_rsp0((uint64_t)next->stack + next->stack_size)`.
3. **Ring 3 Entry & iretq Mechanism**:
   - To transition to Ring 3:
     - Must set data segments (`DS`, `ES`, `FS`, `GS`) to `KOS_USER_DATA_SELECTOR (0x33)`.
     - Must construct an `iretq` stack frame on the kernel stack:
       ```
       [RSP + 32] User SS (0x33)
       [RSP + 24] User RSP
       [RSP + 16] User RFLAGS (0x202: IF enabled)
       [RSP + 08] User CS (0x2b)
       [RSP + 00] User RIP (entry point)
       ```
     - Execute `iretq`.

---

## 5. Signals / Synchronization Events

### 5.1 Existing IPC & Synchronization Primitives
1. **Wait Queue Subsystem (`kos/wait.h`, `kernel/wait.c`)**:
   - Implements condition-based sleep/wake: `wait_event()`, `wait_event_timeout()`.
   - `wait_queue_wake_one()` and `wait_queue_wake_all()`.
   - Integrated into process exit: `proc->exit_wq`.
2. **Event Objects (`kos_event_object`)**:
   - Supports manual-reset and auto-reset signaling.
   - Wrapped in `kos_object_header` and accessible via process handles (`event_create`, `event_signal`, `event_reset`, `event_wait`).
3. **Mutex & Spinlock**:
   - Sleep-blocking mutex (`mutex_t`) in `kernel/mutex.c`.
   - Spinlocks with irqsave/irqrestore (`kos_spinlock_t`) in `kernel/spinlock.c`.

### 5.2 Gaps for Milestone K14
- There is no asynchronous signal infrastructure (no `sigaction`, `signal()`, or signal frame injection onto user stack).
- For K14, asynchronous signal delivery into user space is NOT strictly required by the prompt, but **foundational signal/event notification primitives** are required:
  - Process exit notification via wait queue (already present via `exit_wq`).
  - Event object signaling between processes via handle duplication or shared handles.
  - Recording standard exit signals / fault reasons in `proc->exit_code` (e.g., status codes representing SIGSEGV, SIGILL, SIGBUS).

---

## 6. Test Harness & Build Toolchain

### 6.1 Build Pipeline (`tools/kos.c` / `tools/kos-tool.exe`)
- **Invocation**: `tools\kos-tool.exe build`
- **Compiler/Linker**:
  - LLVM Clang freestanding target `x86_64-unknown-none-elf` with `-mcmodel=kernel -mno-red-zone -ffreestanding -O2 -g`.
  - NASM 3.02 for assembly sources.
  - LLD `ld.lld.exe` with linker script `boot/linker.ld`.
- **Symbol Table Embedding**:
  - Two-pass build:
    1. Pass 1 compiles all C and ASM sources with stub `kernel_symbols.c` and links intermediate `build/kos.elf`.
    2. `llvm-nm -n build/kos.elf` extracts all text symbols to generate `build/kernel_symbols.c`.
    3. Pass 2 re-compiles `kernel_symbols.c` and re-links final `build/kos.elf` containing 650+ embedded symbols for kernel stack traces.
- **Initramfs Packaging**:
  - `make_initramfs()` packages `assets/initramfs/README.TXT` and `build/kos.elf` into `build/initramfs.cpio` (cpio SVR4 format with crc/padding).
  - Copies Limine bootloader `BOOTX64.EFI`, `limine.conf`, `kos.elf`, and `initramfs.cpio` into `build/esp/` (FAT volume).

### 6.2 QEMU Test Environment
- **Command Line**:
  ```powershell
  qemu-system-x86_64 -machine q35 -cpu max -m 256 \
    -drive if=pflash,format=raw,readonly=on,file="C:/Program Files/qemu/share/edk2-x86_64-code.fd" \
    -drive if=pflash,format=raw,file=build/edk2-vars.fd \
    -drive format=raw,file=fat:rw:build/esp \
    -serial stdio -monitor none -no-reboot -display none
  ```
- **Automated Exit**:
  - QEMU has `-no-reboot`. Calling `cpu_reboot()` causes QEMU to immediately terminate.
  - Serial output can be redirected to a log file (`-serial file:build/qemu_serial.log`) or captured via child process stdout in PowerShell.

### 6.3 Existing Test Commands in Kernel Shell
Located in `kernel/command.c`:
- `taskinfo`: Displays task count and ready count.
- `tasktest`: Two kernel tasks yielding 32 times.
- `taskdemo`: Cooperative task demo with keyboard IRQ concurrency.
- `elfinfo`: Inspects ELF header on disk (e.g. `elfinfo C:\KOS.ELF`).
- `handletest`: Comprehensive test of kernel object refcounting, destructor callbacks, handle creation, rights checking, generation tracking, handle lookup, closing, and duplication.
- `waittest`: Tests `wait_queue_wake_one`, `wait_queue_wake_all`, wait timeouts, event objects, and process child wait/exit cleanup (`process_wait`).
- `synctest`: Stress tests multi-threaded PMM frame allocation/freeing across concurrent tasks.

### 6.4 Boot Failure Reproduction & Discovery
During live QEMU execution of the current codebase, a kernel panic occurred during boot:
```
Fatal Page Fault: read data at address 0x0000000000000000 (kernel mode, non-present page)
#00 wait_queue_wake_all+0x38
#01 net_udp_sendto+0x59d
#02 net_udp_loopback_self_test+0x2ea
#03 kernel_main+0x621
```
**Root Cause**:
In `net/socket.c:186`:
```c
sockets[index] = (struct udp_socket){ .open = true };
```
This zero-initializes the socket structure, overwriting `sockets[index].wait_queue.head.next = NULL`. When `net_udp_sendto` receives a loopback datagram, it calls `wait_queue_wake_all(&receiver->wait_queue)`. Because `head.next` is NULL, `!list_empty(&wq->head)` evaluates to true and `list_entry(0, ...)` dereferences address `0x0`, crashing the kernel.
**Immediate Fix Required**: Call `wait_queue_init(&sockets[index].wait_queue)` in `net_udp_open()`, and harden `wait_queue_wake_all()` against uninitialized heads.

---

## 7. Recommendations and Required Architectural Changes for R3

To satisfy all Acceptance Criteria for Milestone K14, the following architectural additions and modifications must be implemented:

### 7.1 Virtual Memory Manager (VMM) Enhancements (`mm/vmm.c`, `include/kos/vmm.h`)
1. **Add `VMM_PAGE_USER`**:
   - Define `VMM_PAGE_USER = 1ull << 2` in `include/kos/vmm.h`.
   - Update `vmm_map_page()` to accept `VMM_PAGE_USER`.
   - Ensure that whenever intermediate paging tables (PDPT, Directory, Page Table) are created for user mappings, the `PAGE_USER` bit is set in the parent table entries. (In x86_64 paging, user mode access requires the U/S bit to be 1 in PML4E, PDPTE, PDE, and PTE).
2. **Address Space Allocation (`vmm_create_address_space`)**:
   - Allocate 1 physical frame for a new PML4 table.
   - Clear lower half entries (0..255) for user virtual addresses (`0x0000000000000000` to `0x00007FFFFFFFFFFF`).
   - Copy higher half entries (256..511) from the kernel's bootstrap PML4 (preserving kernel code, data, heap, and HHDM).
   - Return physical address of new PML4.
3. **Address Space Teardown (`vmm_destroy_address_space`)**:
   - Traverse user-half PML4 entries (0..255).
   - For each present table, recursively traverse PDPTEs, PDEs, and PTEs.
   - Free all mapped user physical frames via `pmm_free_frame()`.
   - Free all page table structures via `pmm_free_frame()`.
   - Finally free the top-level PML4 frame.
   - Zero kernel frames are touched or freed.

### 7.2 User Stack & Guard Page Management
1. **User Stack Allocation**:
   - Allocate user stack at top of user canonical address space (e.g., base `0x00007FFFFFF00000`, size 64 KiB = 16 pages).
   - Map 15 pages with `VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE`.
2. **Guard Page**:
   - The bottom page (`0x00007FFFFFF00000`) is left **unmapped** (non-present).
   - If user execution exceeds the 60 KiB stack, hitting the bottom page triggers a `#PF` with `P=0, W/R=1, U/S=1`.
3. **System V AMD64 ABI Initialization**:
   - Format initial user stack prior to entering Ring 3:
     - Push string contents (`argv[0]`, `argv[1]`, `envp[0]`, etc.).
     - Push NULL terminator for `envp`.
     - Push `envp` pointers.
     - Push NULL terminator for `argv`.
     - Push `argv` pointers.
     - Push `argc` (as uint64_t).
     - Align RSP to 16-byte boundary (RSP % 16 == 8 before `call`, so RSP % 16 == 0 at entry).

### 7.3 Process/Thread Lifecycle & Hierarchy Updates (`kernel/process.c`, `include/kos/process.h`)
1. **Bidirectional Parent-Child Tracking**:
   - Add `struct list_head children;` and `struct list_head sibling_node;` to `struct process`.
   - `process_create()` attaches child to parent's `children` list.
   - Add `process_reparent_children(struct process *parent, struct process *new_parent)`: when a process terminates, any active children are reparented to `kernel_process` (PID 1).
2. **Zombie Reaping Protocol**:
   - Disallow `reaper_thread` from calling `process_destroy(proc)`.
   - When the last thread of a process exits, the process remains in `PROCESS_STATE_ZOMBIE`.
   - `process_wait(proc, exit_code, timeout_ms)`:
     - Waits for child state `PROCESS_STATE_ZOMBIE`.
     - Copies `exit_code`.
     - Calls `process_destroy(proc)` to reclaim `proc` slot and address space.
     - Removes child from parent's `children` list.
3. **Implement `process_exec` / `process_load_elf`**:
   - `process_exec(const char *path, char *const argv[])`:
     - Reads binary from VFS.
     - Validates ELF64 headers via extended `fs/elf.c`.
     - If current process has existing user mappings, tears them down.
     - Maps `PT_LOAD` segments with appropriate permissions:
       - Code: `VMM_PAGE_USER`
       - Data: `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`
       - Readonly: `VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE`
     - Allocates user stack and guard page.
     - Formats initial user stack with `argc`, `argv`.
     - Prepares user thread context to jump to `elf_entry`.

### 7.4 Scheduler & Ring 3 Transition (`kernel/task.c`, `arch/x86_64/task_switch.asm`)
1. **TSS RSP0 and CR3 in `schedule()`**:
   ```c
   /* In schedule() in kernel/task.c */
   if (next->process != NULL && next->process->pml4_physical != 0) {
       uint64_t current_cr3 = cpu_read_cr3() & ~0xFFFull;
       if (next->process->pml4_physical != current_cr3) {
           cpu_write_cr3(next->process->pml4_physical);
       }
   }
   tss_set_rsp0((uint64_t)next->stack + next->stack_size);
   ```
2. **Ring 3 Enter Trampoline (`arch/x86_64/cpu.asm` or `task_switch.asm`)**:
   - Helper function `user_mode_enter(uint64_t entry, uint64_t user_rsp)`:
     ```nasm
     global user_mode_enter
     user_mode_enter:
         ; rdi = entry point, rsi = user stack pointer
         cli
         mov ax, 0x33            ; KOS_USER_DATA_SELECTOR
         mov ds, ax
         mov es, ax
         mov fs, ax
         mov gs, ax
         push qword 0x33         ; User SS
         push rsi                ; User RSP
         push qword 0x202        ; RFLAGS (IF=1)
         push qword 0x2b         ; User CS (KOS_USER_CODE_SELECTOR)
         push rdi                ; User RIP
         iretq
     ```

### 7.5 Fault Handling Discrimination (`arch/x86_64/interrupts.c`)
- In `page_fault_handler(context)`:
  - If `!(context->error_code & PF_ERROR_USER)`: Kernel-mode fault -> Panic and Halt.
  - If `context->error_code & PF_ERROR_USER`: User-mode fault:
    - Log user fault with CR2, RIP, RSP.
    - Check if CR2 is in guard page range: log `[FAULT] User stack overflow: hit guard page at 0x...`.
    - Check if CR2 is in kernel range (>= 0xFFFF800000000000): log `[FAULT] Unauthorized kernel memory access`.
    - Terminate faulting process cleanly:
      ```c
      struct process *cur = process_get_current();
      log_warnf("Terminating faulting user process '%s' (PID %llu) with SIGSEGV",
                cur->name, (unsigned long long)cur->pid);
      process_exit(cur, -11 /* SIGSEGV */);
      ```
    - The process exits, wakes parent, and yields CPU without panicking or halting the kernel!

### 7.6 Verification Test Suite & Acceptance Matrix
1. **Automated Shell Test Command (`usertest`)**:
   - Add command `usertest` to `kernel/command.c`.
   - Test sub-cases:
     - `usertest elf_valid`: Executes valid Ring 3 ELF binary, performs syscall `GETTIME` / `SYSINFO`, exits with code 42, parent verifies exit code via `process_wait`.
     - `usertest elf_invalid`: Passes corrupted ELF images (bad magic, invalid machine, misaligned segments), verifies graceful return code without kernel panic.
     - `usertest kernel_protect`: Runs user process that attempts to read address `0xFFFFFFFF80000000`. Verifies page fault handler terminates process with SIGSEGV and kernel continues running.
     - `usertest stack_guard`: Runs user process that recurses until touching stack guard page. Verifies clean termination without kernel crash.
     - `usertest preempt`: Runs user process with infinite computation loop. PIT timer preempts process, switches to another thread, returns cleanly.
     - `usertest leak`: Loops 50 times creating and destroying processes/threads; verifies `pmm_free_frame_count()` and `heap_active_allocation_count()` return to baseline values (zero leakage).
2. **Automated CI Integration**:
   - Register `(void)kernel_command_execute("usertest");` in `main.c`.
   - Run QEMU in headless mode (`-display none -serial file:build/qemu_serial.log`).
   - Parse `build/qemu_serial.log` for `>>> [PASS] ALL USERSPACE & LIFECYCLE TESTS PASSED! <<<`.

---

## 8. Summary of File Modifications for Subsequent Phases

| File | Proposed Modifications |
|---|---|
| `include/kos/vmm.h` | Add `VMM_PAGE_USER = (1ull << 2)`, declare `vmm_create_address_space`, `vmm_destroy_address_space`, `vmm_switch_address_space`. |
| `mm/vmm.c` | Implement user-page mapping support in page table hierarchy, per-process PML4 allocation, recursive address space destruction, CR3 switching. |
| `include/kos/task.h` | Add `user_stack_top`, `is_user_thread` to `struct task`. |
| `include/kos/process.h` | Add `children` list, `sibling_node`, user stack tracking, signal/exit fields. Declare `process_exec`. |
| `kernel/process.c` | Implement bidirectional child tracking, fix zombie reaping race condition in `reaper_thread_entry` and `process_wait`, implement `process_exec`. |
| `kernel/task.c` | Update `schedule()` to switch CR3 when process changes and call `tss_set_rsp0()` on every switch. |
| `arch/x86_64/task_switch.asm` / `cpu.asm` | Implement `user_mode_enter` trampoline (`iretq`). Add `cpu_write_cr3()`. |
| `arch/x86_64/interrupts.c` | Update `page_fault_handler` to terminate faulting user processes cleanly instead of `cpu_halt()`. |
| `net/socket.c` & `kernel/wait.c` | Fix socket wait queue initialization bug causing NULL dereference at boot. Harden `wait_queue_wake_all`. |
| `fs/elf.c` & `include/kos/elf.h` | Expand ELF inspector into full segment loader with memory mapping and bounds checking. |
| `kernel/command.c` | Add `usertest` command verifying all K14 acceptance criteria. |
| `tools/kos.c` | Ensure new sources are registered and initramfs includes test ELF binaries. |
