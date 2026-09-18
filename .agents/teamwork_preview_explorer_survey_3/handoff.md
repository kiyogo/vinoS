# Handoff Report: Explorer Survey 3 (Process/Thread Subsystem, Lifecycle, Reclamation, Scheduler & Test Harness)

**Agent**: `teamwork_preview_explorer_survey_3`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_3`  
**Milestone**: K14 (Process and Ring 3 Userspace Subsystem)  
**Date**: 2026-09-18  

---

## 1. Observation

### 1.1 Process and Thread Subsystem Layout
- **Files Checked**: `kernel/task.c`, `kernel/process.c`, `include/kos/task.h`, `include/kos/process.h`.
- **Finding**: Files `kernel/sched.c`, `kernel/thread.c`, `include/kos/sched.h`, and `include/kos/thread.h` do not exist. Scheduler and thread operations reside in `kernel/task.c` and `include/kos/task.h`.
- `include/kos/task.h` lines 85-87:
  ```c
  #define thread task
  typedef struct task thread_t;
  typedef struct task task_t;
  ```
- `struct task` (`include/kos/task.h:64-83`):
  Fields present: `struct task_context context;`, `task_entry entry;`, `void *argument;`, `uint8_t *stack;`, `size_t stack_size;`, `const char *name;`, `volatile enum thread_state state;`, `uint64_t tid;`, `uint64_t id;`, `struct process *process;`, `struct list_head process_node;`, `int exit_code;`, `uint64_t sleep_target_tick;`, `struct list_head sleep_node;`, `uint32_t preempt_count;`, `bool reschedule_pending;`, `uint32_t quantum_remaining;`, `struct list_head reaper_node;`.
  *Absence*: No user stack pointer (`user_stack_top`), no `is_user_thread` flag.
- `struct process` (`include/kos/process.h:27-40`):
  Fields present: `uint64_t pid;`, `char name[PROCESS_NAME_MAX];`, `uint64_t ppid;`, `enum process_state state;`, `int exit_code;`, `struct list_head threads;`, `uint32_t thread_count;`, `uint64_t pml4_physical;`, `kos_spinlock_t lock;`, `struct list_head process_node;`, `struct handle_table *handles;`, `struct wait_queue exit_wq;`.
  *Absence*: No `children` list, no `sibling_node`, no user memory limits, no user heap break.

### 1.2 Hierarchy and Lifecycle Implementations
- `process_create(name)` (`kernel/process.c:115-157`): Allocates from static pool `processes[32]`. Sets `proc->ppid = cur ? cur->pid : 1;`. Sets `proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;` (line 139).
- `process_exec`: **Not implemented anywhere in the codebase**. (Search for `process_exec` returns 0 hits).
- `process_exit(proc, exit_code)` (`kernel/process.c:183-235`): Destroys handle table (`handle_table_destroy(ht)`), enqueues remaining non-current threads to `reaper_queue`, wakes `exit_wq`.
  Lines 218, 228-230:
  ```c
  bool has_no_threads = (proc->thread_count == 0);
  ...
  if (has_no_threads) {
      process_destroy(proc);
  }
  ```
  If all threads have exited, `process_destroy` is called immediately.
- `reaper_thread_entry` (`kernel/process.c:367-373`):
  ```c
  bool should_destroy = (proc->state == PROCESS_STATE_ZOMBIE && proc->thread_count == 0);
  spinlock_unlock_irqrestore(&proc->lock, pflags);
  if (should_destroy) {
      process_destroy(proc);
  }
  ```
  In `process_destroy(proc)` (line 166), `proc->state = PROCESS_STATE_DEAD; list_del_init(&proc->process_node);`.
  In `process_create()` (line 122):
  ```c
  if (processes[i].state == PROCESS_STATE_UNUSED || processes[i].state == PROCESS_STATE_DEAD)
  ```
  The process slot can be immediately recycled before a parent reads `exit_code`.
- `process_wait(proc, exit_code, timeout_ms)` (`kernel/process.c:245-268`): Waits on `proc->exit_wq` with condition `proc->state == PROCESS_STATE_ZOMBIE || proc->state == PROCESS_STATE_DEAD`. Does not free the process slot. Only waits on a specific pointer `proc`, not arbitrary child or PID.

### 1.3 Scheduler and Context Switching
- `schedule()` (`kernel/task.c:261-303`): Selects `next` from `task_find_next_ready()`, calls `task_switch(&previous->context, &next->context)`.
  *Absence*: Never checks `next->process != previous->process`, never switches CR3, never calls `tss_set_rsp0()`.
- `arch/x86_64/task_switch.asm:10-35`: Only preserves `rbx, rbp, r12, r13, r14, r15, rsp, rip, rflags`.
- `arch/x86_64/descriptors.c:81`: `tss_set_rsp0((uint64_t)kos_boot_stack_top)` is called only once at boot in `gdt_initialize()`. It is never updated on context switches.

### 1.4 Virtual Memory and Paging
- `include/kos/vmm.h:7-10`:
  ```c
  enum {
      VMM_PAGE_WRITABLE = 1ull << 1,
      VMM_PAGE_NO_EXECUTE = 1ull << 63,
  };
  ```
  *Absence*: `VMM_PAGE_USER` (`1ull << 2`) is completely absent.
- `mm/vmm.c:116-118`:
  ```c
  if (virtual_address % PAGE_SIZE != 0 || physical_address % PAGE_SIZE != 0
      || (flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) != 0) {
      return false;
  }
  ```
  Any flag other than WRITABLE or NO_EXECUTE causes `vmm_map_page()` to fail.
- `mm/vmm.c:97`: Intermediate page table entries are set to `frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE`, omitting the user bit (`PAGE_USER`).

### 1.5 Page Fault Handling in User Mode
- `arch/x86_64/interrupts.c:258-269`:
  ```c
  } else {
      /* User mode page fault (Ring 3) */
      log_enter_emergency_mode();
      log_emergency_puts("\r\n==================== USER PROCESS PAGE FAULT ====================\r\n");
      log_error("PAGE FAULT: Access violation in User Mode (Ring 3)");
      log_info_hex("Faulting Address (CR2): ", cr2);
      ...
      log_error("User space process termination not yet available in K12 -> System halted.");
      cpu_halt();
  }
  ```
  User mode faults trigger a kernel halt (`cpu_halt()`) instead of process termination.

### 1.6 Boot Failure Observation during QEMU Test Run
- Running QEMU via PowerShell:
  `C:\Program Files\qemu\qemu-system-x86_64.exe -machine q35 -cpu max -m 256 ... -serial file:build/qemu_serial.log`
- Serial log output:
  ```
  Fatal Page Fault: read data at address 0x0000000000000000 (kernel mode, non-present page)
  #00 wait_queue_wake_all+0x38
  #01 net_udp_sendto+0x59d
  #02 net_udp_loopback_self_test+0x2ea
  #03 kernel_main+0x621
  ```
- Traced to `net/socket.c:186`:
  `sockets[index] = (struct udp_socket){ .open = true };`
  Overwrites socket structure without calling `wait_queue_init(&sockets[index].wait_queue)`, leaving `head.next == NULL`. `wait_queue_wake_all()` checks `!list_empty(&wq->head)` and dereferences NULL.

### 1.7 Build Toolchain Verification
- Running `tools\kos-tool.exe build` succeeds with exit code 0.
- Clang, NASM, LLD execute two-pass symbol generation and link `build/kos.elf` (651 embedded symbols).
- Generates `build/initramfs.cpio` containing `README.TXT` and `KOS.ELF`.
- Stages UEFI ESP in `build/esp/`.

---

## 2. Logic Chain

1. **Premise 1**: In x86_64 architecture, Ring 3 user programs execute with CPL=3. When an interrupt or exception occurs while CPL=3, the CPU hardware automatically loads `SS:RSP` from `TSS.rsp0`.
   - *Observation*: `tss_set_rsp0()` is only called once at boot with `kos_boot_stack_top` and never updated on context switch (`descriptors.c:81`).
   - *Inference*: Any user thread taking a timer interrupt or page fault would write its interrupt context to the single global boot stack, causing stack corruption if multiple threads exist or if the boot stack was reused.
   - *Required Action*: Every thread context switch must invoke `tss_set_rsp0((uint64_t)next->stack + next->stack_size)`.

2. **Premise 2**: User processes require memory isolation. Address translation depends on CR3 pointing to a per-process PML4 table where user-space mappings are distinct.
   - *Observation*: `process_create()` assigns `proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;` (`process.c:139`), and `schedule()` never modifies CR3 (`task.c:261-303`). Furthermore, `mm/vmm.c` does not define `VMM_PAGE_USER` and rejects it (`vmm.c:117`).
   - *Inference*: All current processes share the kernel's address space. No true user-space isolation is possible without adding `VMM_PAGE_USER`, per-process PML4 allocation (`vmm_create_address_space`), and CR3 reloading on process switch.

3. **Premise 3**: Milestone K14 Acceptance Criteria require:
   "Parent processes can fork/create children, retrieve child exit status via `process_wait`, and zombie processes are properly reaped upon cleanup."
   - *Observation*: In `kernel/process.c:367`, `reaper_thread` marks `proc->state = PROCESS_STATE_DEAD` as soon as the last thread exits, allowing `process_create()` to immediately overwrite the process slot in `processes[32]`.
   - *Inference*: A fast-running child process will be destroyed before the parent thread has an opportunity to call `process_wait()`, losing the exit code and corrupting wait queues.
   - *Required Action*: A process must remain in `PROCESS_STATE_ZOMBIE` until its parent retrieves the exit status via `process_wait()`. Only `process_wait()` should transition the process to `PROCESS_STATE_DEAD` and free the slot.

4. **Premise 4**: Acceptance Criteria require:
   "Hitting the user stack guard page cleanly faults and terminates the faulting process without crashing the kernel."
   - *Observation*: `page_fault_handler()` in `arch/x86_64/interrupts.c:267-268` prints an emergency log and executes `cpu_halt()`.
   - *Inference*: Any user-mode fault currently halts the entire operating system.
   - *Required Action*: `page_fault_handler()` must check `if (context->error_code & PF_ERROR_USER)` and invoke `process_exit(cur, -11 /* SIGSEGV */)` instead of `cpu_halt()`.

5. **Premise 5**: Acceptance Criteria require:
   "Repeated process creation and destruction loops demonstrate zero kernel heap or page frame leakage."
   - *Observation*: Current KOS has no mechanism to tear down user page tables.
   - *Inference*: Creating and destroying user processes without a recursive page table teardown (`vmm_destroy_address_space`) will continuously leak physical frames allocated by PMM.
   - *Required Action*: Implement `vmm_destroy_address_space(uint64_t pml4_phys)` which walks PML4 entries 0..255 and frees all mapped user frames and page table hierarchy frames (`pmm_free_frame`).

---

## 3. Caveats

1. **Scope of Investigation**: This investigation is strictly read-only analysis. No source files were modified during this survey.
2. **Pre-existing Boot Fault**: QEMU live boot currently crashes in `net_udp_loopback_self_test()` due to the uninitialized wait queue in `net/socket.c:186`. This must be patched during the implementation phase before automated boot tests will reach the shell prompt or subsequent tests.
3. **No ELF Loader in Codebase**: While `fs/elf.c` contains `elf_inspect_image()`, it only inspects headers; an actual `PT_LOAD` mapper and executable loader does not yet exist.

---

## 4. Conclusion

The KOS kernel possesses a robust foundation in its preemptive scheduler (`kernel/task.c`), dedicated reaper thread (`reaper_thread`), object handle table (`kernel/handle.c`), wait queue synchronization (`kernel/wait.c`), and two-pass symbol-embedded build toolchain (`tools/kos-tool.exe`).

However, to fulfill Milestone K14 requirements (R1, R2, R3), five core architectural mechanisms must be introduced:
1. **Paging & Isolation**: Add `VMM_PAGE_USER (1ull << 2)` to `mm/vmm.c`, implement per-process PML4 creation (`vmm_create_address_space`), and recursive user address space teardown (`vmm_destroy_address_space`).
2. **Context Switching & TSS**: Update `schedule()` to switch CR3 when processes differ and call `tss_set_rsp0()` on every thread switch. Implement `user_mode_enter` trampoline with `iretq`.
3. **Process Hierarchy & Zombie Semantics**: Add `children` and `sibling_node` list links to `struct process`. Change zombie reaping so processes remain in `PROCESS_STATE_ZOMBIE` until reaped by parent via `process_wait()`.
4. **Fault Recovery**: Modify `page_fault_handler()` to terminate faulting user-mode processes with `process_exit(cur, -11)` rather than calling `cpu_halt()`.
5. **ELF64 Loader & User Stack**: Implement `process_exec` to load `PT_LOAD` segments, allocate a user stack with an unmapped bottom guard page, format the System V ABI stack (`argc, argv, envp`), and jump to user space.

---

## 5. Verification Method

### 5.1 Automated Build Verification
Verify that the host tool compiles the kernel cleanly without warnings or errors:
```powershell
.\tools\kos-tool.exe build
```
*Expected Result*: Returns exit code 0, prints `Kernel ELF built with ... embedded symbols: build/kos.elf` and `UEFI ESP staged: build/esp`.

### 5.2 QEMU Live Test Execution
Execute QEMU headless and verify serial log output:
```powershell
$proc = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" `
  -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru
Start-Sleep -Seconds 8
Stop-Process -Id $proc.Id -Force
Get-Content build/qemu_serial.log -Tail 60
```

### 5.3 Invalidation Conditions
The conclusions of this report would be invalidated if:
1. `tss_set_rsp0()` is proved to be unnecessary for hardware interrupts arriving in Ring 3 (which contradicts the AMD64 Architecture Programmer's Manual Vol 2 §8.9.3).
2. Existing processes are proved to have isolated page tables (which is disproved by `process.c:139` where `proc->pml4_physical = cpu_read_cr3()`).
3. User mode page faults can recover without terminating the faulting process or returning to userspace.

### 5.4 Proposed Test Cases for Acceptance Criteria Verification
Once implemented, add shell command `usertest` in `kernel/command.c` validating:
1. `usertest valid`: Executes Ring 3 ELF, performs syscall, exits code 42, parent retrieves code via `process_wait`.
2. `usertest corrupted`: Rejects corrupted ELF cleanly without kernel panic.
3. `usertest kernel_mem`: Ring 3 attempts to read `0xFFFFFFFF80000000`, terminated with SIGSEGV without kernel panic.
4. `usertest stack_overflow`: Ring 3 recurses into guard page, terminated with SIGSEGV without kernel panic.
5. `usertest preemption`: Ring 3 compute loop preempted by timer IRQ, returns cleanly.
6. `usertest leak`: 50 create/destroy iterations verify baseline PMM frames and heap allocations are preserved.
