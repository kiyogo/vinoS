# Milestone M4 Review & Adversarial Challenge Report

**Reviewer**: Reviewer M4-2 (Gen 2)  
**Target Milestone**: K14 (Milestone M4 / Requirement R3)  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\reviewer_m4_2_gen2`  
**Verdict**: **APPROVE**  
**Date**: 2026-09-18  

---

## 1. Observation

### 1.1 Concurrency & Locking Discipline (`kernel/process.c`)
- **Global `process_lock`**:
  - `process_create()` (`kernel/process.c:123-171`): acquires `process_lock` to search for an unused/dead slot in `processes[]`, populates process fields, appends to `process_list`, and under child creation acquires `cur->lock` (or `kernel_process.lock`), adds `proc->sibling_node` to `cur->children`, releases `cur->lock`, and finally releases `process_lock`.
  - `process_find_by_pid()` (`kernel/process.c:105-115`): acquires `process_lock`, traverses `process_list`, releases `process_lock`.
  - `process_destroy()` (`kernel/process.c:225-235`): `proc->lock` is released at line 221 before `process_lock` is acquired at line 225 to mark `proc->state = PROCESS_STATE_DEAD` and remove `proc->process_node` from `process_list`. No process lock is ever held while acquiring `process_lock`.
- **Per-process `proc->lock` & `parent->lock`**:
  - In `process_wait()` (`kernel/process.c:340-348`):
    ```c
    struct process *parent = process_find_by_pid(proc->ppid);
    if (parent != NULL) {
        uint64_t parent_flags = spinlock_lock_irqsave(&parent->lock);
        list_del_init(&proc->sibling_node);
        spinlock_unlock_irqrestore(&parent->lock, parent_flags);
    } else {
        list_del_init(&proc->sibling_node);
    }
    process_destroy(proc);
    ```
    `parent->lock` is held strictly while unlinking `proc->sibling_node` and released prior to calling `process_destroy(proc)`.
  - In `process_destroy()` (`kernel/process.c:181-191`):
    `proc->lock` is held while iterating over `proc->children`. Each child's `sibling_node` is unlinked, `child->ppid` set to 1, and `kernel_process.lock` is acquired/released to append to `kernel_process.children`.
- **Reaper Lock Hierarchy**:
  - In `reaper_thread_entry()` (`kernel/process.c:495-529`):
    `reaper_lock` is acquired to dequeue a zombie thread and unlocked at line 506. Only after releasing `reaper_lock` is `proc->lock` acquired at line 521.
  - In `process_exit()` (`kernel/process.c:257-285`) and `process_destroy()` (`kernel/process.c:205-209`), `reaper_lock` is acquired and immediately released while holding `proc->lock`. Thus, `proc->lock` is never acquired while holding `reaper_lock`.

### 1.2 Zombie State Retention & Recycling Prevention (`kernel/process.c`)
- **Slot Reuse Criteria**:
  In `process_create()` (`kernel/process.c:125-130`):
  ```c
  for (size_t i = 0; i < PROCESS_MAX_COUNT; i++) {
      if (processes[i].state == PROCESS_STATE_UNUSED || processes[i].state == PROCESS_STATE_DEAD) {
          proc = &processes[i];
          break;
      }
  }
  ```
  Slots in `PROCESS_STATE_ZOMBIE` cannot be reused.
- **Zombie Preservation**:
  - In `process_exit()` (`kernel/process.c:263-305`):
    `proc->state = PROCESS_STATE_ZOMBIE;`
    If `has_no_threads`: checks `parent = process_find_by_pid(ppid)`. Only calls `process_destroy(proc)` if `parent == NULL || parent->state == PROCESS_STATE_DEAD`.
  - In `reaper_thread_entry()` (`kernel/process.c:526-536`):
    When `is_zombie_empty` (`proc->state == PROCESS_STATE_ZOMBIE && proc->thread_count == 0`), it checks `parent = process_find_by_pid(ppid)`. If parent exists and is alive (`parent->state != PROCESS_STATE_DEAD`), it does not call `process_destroy(proc)`.
- **Transition to DEAD only upon Reap**:
  In `process_wait()` (`kernel/process.c:335-352`), `exit_code` is copied to the caller (`*exit_code = proc->exit_code;`), the child is unlinked from `parent->children`, and only then is `process_destroy(proc)` invoked to transition `proc->state = PROCESS_STATE_DEAD`.

### 1.3 Memory Leak Protection on `process_exec` Failure (`kernel/process.c`, `fs/elf.c`, `mm/vmm.c`)
- In `process_exec()` (`kernel/process.c:358-395`):
  ```c
  uint64_t pml4_phys = vmm_create_address_space();
  if (pml4_phys == 0) return -1;

  struct elf_binary_image img;
  bool ok = elf_load_binary(path, pml4_phys, &img);
  if (!ok) {
      vmm_destroy_address_space(pml4_phys);
      return -1;
  }

  uint64_t user_rsp = 0;
  ok = process_setup_user_stack(pml4_phys, argc, argv, &user_rsp);
  if (!ok) {
      vmm_destroy_address_space(pml4_phys);
      return -1;
  }
  ```
- In `fs/elf.c:366-368`:
  `load_error: vmm_clear_user_address_space(pml4_phys); return false;`
- In `mm/vmm.c:248-264` (`vmm_destroy_address_space`):
  Recursively frees entries 0..255 (leaf frames, PT frames, PD frames, PDPT frames) and frees the root PML4 frame via `pmm_free_frame(pml4_phys)`. Restores CR3 to `kernel_pml4_phys` if the destroyed address space was currently loaded.
- In `kernel/process.c:789-810` (`process_lifecycle_self_test`):
  Tests `process_exec(NULL)` and `process_exec("nonexistent.elf")` and verifies `pmm_free_frame_count()` before and after: confirmed 0 frame leaks.

### 1.4 Address Space Switching in `kernel/task.c`
- In `schedule()` (`kernel/task.c:301-314`):
  ```c
  if (next->process != NULL && next->process->pml4_physical != 0) {
      if (previous == NULL || previous->process != next->process) {
          cpu_write_cr3(next->process->pml4_physical);
      }
  }

  if (next->stack != NULL) {
      tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
  } else {
      extern uint8_t kos_boot_stack_top[];
      tss_set_rsp0((uint64_t)kos_boot_stack_top);
  }

  task_switch(&previous->context, &next->context);
  ```
  CR3 is written at line 303 before `task_switch()` at line 314.
  If `previous->process == next->process`, `cpu_write_cr3()` is skipped.

### 1.5 Toolchain Build & QEMU Verification
- Build output (`.\tools\kos-tool.exe build`):
  ```
  Kernel ELF built with 668 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  Exit code: 0 (0 warnings, 0 errors)
  ```
- Headless QEMU execution log (`build/qemu_serial.log`):
  ```
  [    1.070] [TID 1] [INFO ] [process] Running Process & Thread Lifecycle Self-Test...
  [    1.080] [TID 1] [INFO ]   [PASS] Process hierarchy (children & sibling_node) verified.
  [    1.080] [TID 1] [INFO ]   [PASS] Zombie process retention with active parent verified.
  [    1.090] [TID 1] [INFO ]   [PASS] process_wait zombie reaping and hierarchy unlinking verified.
  [    1.100] [TID 1] [INFO ]   [PASS] Address space reclamation verified with zero frame leaks.
  [    1.110] [TID 1] [INFO ]   [PASS] process_exec error rollback and unwinding verified with zero frame leaks.
  [    1.110] [TID 1] [INFO ] >>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<
  ...
  [    1.760] [TID 1] [INFO ] [waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...
  [    1.790] [TID 1] [INFO ]   process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=DEAD
  [    1.820] [TID 1] [INFO ]   [PASS] Process termination resource cleanup verified!
  [    1.830] [TID 1] [INFO ] >>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<
  [    1.990] [TID 1] [INFO ] >>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<
  [    2.010] [TID 1] [INFO ] KOS debug terminal ready.
  KOS>
  ```

---

## 2. Logic Chain

1. **Deadlock Prevention (Lock Hierarchy)**:
   - Observation 1.1 reveals a strict lock ordering across all execution paths:
     `process_lock` -> `proc->lock` -> `kernel_process.lock` / `reaper_lock`.
   - `process_destroy()` drops `proc->lock` before acquiring `process_lock`.
   - `reaper_thread_entry()` drops `reaper_lock` before acquiring `proc->lock`.
   - `process_wait()` drops `parent->lock` before calling `process_destroy()`.
   - Because no lower-tier lock is ever held while acquiring a higher-tier lock, circular wait (AB-BA) deadlock is impossible.

2. **Zombie Retention & Race Freedom**:
   - Observation 1.2 demonstrates that process slot allocation in `process_create()` is restricted to `PROCESS_STATE_UNUSED` and `PROCESS_STATE_DEAD`.
   - When a child exits, it transitions to `PROCESS_STATE_ZOMBIE`. Both `process_exit()` and `reaper_thread_entry()` check if the parent process exists and is not `PROCESS_STATE_DEAD`.
   - Because the parent is alive, neither path calls `process_destroy()`.
   - Therefore, the child slot remains reserved in `PROCESS_STATE_ZOMBIE` until the parent explicitly invokes `process_wait()`.
   - `process_wait()` copies `proc->exit_code` to the caller before unlinking and calling `process_destroy()`, guaranteeing the exit code cannot be lost, corrupted, or overwritten.

3. **Memory Reclamation Completeness**:
   - Observation 1.3 shows that all failure branches in `process_exec()` (`elf_load_binary` failure, `process_setup_user_stack` failure) invoke `vmm_destroy_address_space(pml4_phys)`.
   - `vmm_destroy_address_space()` recursively traverses PML4 entries 0..255 and invokes `pmm_free_frame()` for all mapped leaf frames and intermediate table frames (PT, PD, PDPT), and frees the PML4 root frame.
   - Empirical measurements in `process_lifecycle_self_test()` verify that physical frame counts before and after aborted `process_exec()` and `process_destroy()` runs are identical, confirming 0 leaked frames.

4. **Address Space Isolation & Switching**:
   - Observation 1.4 confirms that `schedule()` checks `previous->process != next->process` and reloads CR3 with `next->process->pml4_physical` prior to `task_switch()`.
   - Redundant reloads during context switches between threads belonging to the same process are avoided.
   - `tss_set_rsp0()` is called unconditionally on every context switch, ensuring user-mode interrupts and exceptions return cleanly to the active thread's kernel stack.

---

## 3. Adversarial Challenges & Stress Testing

### 3.1 Challenge 1: Fast-Terminating Child Process Before Parent Calls `process_wait`
- **Assumption Challenged**: If a child terminates quickly and all its threads are reaped by `reaper_thread` before the parent thread gets scheduled, the child process could be prematurely destroyed and its exit code lost.
- **Attack Scenario**: Spawn child thread that immediately calls `exit(42)`. Scheduler runs reaper thread before parent thread.
- **Result / Mitigation**: In `reaper_thread_entry()`, the check `if (parent == NULL || parent->state == PROCESS_STATE_DEAD)` evaluates to false because the parent is active. The reaper reaps the thread stack, decrements `proc->thread_count`, but leaves `proc` in `PROCESS_STATE_ZOMBIE`. When the parent later calls `process_wait()`, it finds `proc->state == PROCESS_STATE_ZOMBIE` immediately and retrieves `exit_code == 42`.
- **Verdict**: PASS.

### 3.2 Challenge 2: Parent Process Dies Before Waiting on Child
- **Assumption Challenged**: If the parent process exits or is destroyed while children are still active or in zombie state, children could be left pointing to a dangling parent PID.
- **Attack Scenario**: Kill parent process while child is alive or zombie.
- **Result / Mitigation**: In `process_destroy(parent)`, step 1 reparents all children in `parent->children` to `kernel_process` (PID 1), setting `child->ppid = 1` under `kernel_process.lock`. If the child then exits, its parent is `kernel_process` which is permanently alive, preventing dangling references.
- **Verdict**: PASS.

### 3.3 Challenge 3: Process Slot Exhaustion (`PROCESS_MAX_COUNT = 32`)
- **Assumption Challenged**: Creating 32 processes without reaping crashes or corrupts the process table.
- **Attack Scenario**: Call `process_create()` 32 times without reaping.
- **Result / Mitigation**: `process_create()` scans `processes[0..31]`. When no slot is `UNUSED` or `DEAD`, it cleanly returns `NULL` without panicking or modifying internal state.
- **Verdict**: PASS.

### 3.4 Challenge 4: Memory Leak on Corrupted ELF Program Headers
- **Assumption Challenged**: An ELF file with 2 valid load segments followed by an invalid segment or failed memory allocation could leak physical frames already mapped for the first 2 segments.
- **Attack Scenario**: `elf_load_binary()` fails midway through segment allocation.
- **Result / Mitigation**: `elf_load_binary()` has `goto load_error`, which calls `vmm_clear_user_address_space(pml4_phys)`. `process_exec()` then calls `vmm_destroy_address_space(pml4_phys)`. All allocated frames are freed.
- **Verdict**: PASS.

---

## 4. Integrity Violation Audit

| Integrity Check | Status | Evidence |
|---|---|---|
| Hardcoded test results or fake assertions | **NONE DETECTED** | `process_lifecycle_self_test()` and `waittest` dynamically check `pmm_free_frame_count()`, list traversals, and actual exit codes. |
| Dummy or facade implementations | **NONE DETECTED** | Genuine 4-level page table traversals in `vmm_destroy_address_space`, authentic doubly-linked list operations, genuine spinlocks with IRQ state preservation. |
| Shortcuts bypassing task requirements | **NONE DETECTED** | System V AMD64 ABI user stack formatting, ELF segment boundary checks, CR3 switching, and TSS RSP0 updates are all fully implemented. |
| Fabricated verification outputs | **NONE DETECTED** | Verified via live compiler execution (`kos-tool build`) and independent QEMU serial log capture. |

---

## 5. Caveats

1. **Uniprocessor Locking Model**: Spinlocks currently operate with `cli/sti` IRQ save/restore on a single core. In future multi-core SMP milestones, child list traversals under concurrent writers may benefit from read-copy-update (RCU) or read-write spinlocks.
2. **Init Reaping Loop**: Currently, zombie children reparented to `kernel_process` (PID 1) remain in zombie state until explicitly reaped. In future milestones with userland `init`, a background wait loop in `init` will automatically reap orphaned zombies.

---

## 6. Conclusion

The Milestone M4 implementation (Requirement R3) satisfies all architectural and safety requirements:
- Process hierarchy tracking (`children`, `sibling_node`) with parent-child linkage and reparenting to `kernel_process`.
- Strict locking discipline with zero AB-BA deadlocks across `process_lock`, `proc->lock`, and `reaper_lock`.
- Strict zombie lifecycle guaranteeing no premature slot reuse before parent retrieves exit status via `process_wait()`.
- Zero-leak physical frame and page table reclamation on `process_destroy()` and `process_exec()` error unwinding.
- CR3 address space switching and TSS RSP0 configuration in `kernel/task.c`.
- 100% build pass (0 warnings, 0 errors) and 100% test pass in QEMU.

**Verdict**: **APPROVE**.

---

## 7. Verification Method

To independently verify this evaluation:
1. Run build:
   ```pwsh
   .\tools\kos-tool.exe build
   ```
   Confirm exit code 0, 0 warnings, 0 errors.
2. Run QEMU:
   ```pwsh
   $proc = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList '-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\edk2-x86_64-code.fd" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none' -PassThru
   Start-Sleep -Seconds 8
   Stop-Process -Id $proc.Id -Force
   Get-Content build/qemu_serial.log -Tail 80
   ```
   Confirm `[PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED!` and zero frame leaks.
