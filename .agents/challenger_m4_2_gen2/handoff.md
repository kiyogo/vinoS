# Challenger M4-2 Handoff Report: Milestone M4 Process & Thread Lifecycle

**Agent**: `challenger_m4_2_gen2` (Empirical Challenger)  
**Parent Agent**: `c843668f-19e3-4d81-abc8-ea4359b7dde0` (`parent`)  
**Target**: Milestone K14-M4 (Requirement R3)  
**Date**: 2026-09-18  
**Verdict**: **APPROVE**

---

## 1. Observation

### 1.1 Resource Reclamation Integrity (`kernel/process.c`, `mm/vmm.c`)
- In `kernel/process.c` lines 242–247:
  ```c
  /* 6. Destroy private user address space if non-zero and not kernel PML4 */
  uint64_t user_pml4 = proc->pml4_physical;
  proc->pml4_physical = 0;
  if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical) {
      vmm_destroy_address_space(user_pml4);
  }
  ```
- In `mm/vmm.c` lines 248–264:
  ```c
  void vmm_destroy_address_space(uint64_t pml4_phys) {
      if (pml4_phys == 0 || (pml4_phys % PAGE_SIZE) != 0) {
          return;
      }
      uint64_t interrupt_flags = spinlock_lock_irqsave(&vmm_lock);
      if (!vmm.initialized || pml4_phys == vmm.kernel_pml4_phys) {
          spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);
          return;
      }
      if ((cpu_read_cr3() & PAGE_ADDRESS_MASK) == pml4_phys) {
          cpu_write_cr3(vmm.kernel_pml4_phys);
      }
      spinlock_unlock_irqrestore(&vmm_lock, interrupt_flags);

      vmm_clear_user_address_space(pml4_phys);
      pmm_free_frame(pml4_phys);
  }
  ```
- In `mm/vmm.c` lines 185–239 (`vmm_clear_user_address_space`):
  - Traversal is strictly constrained to `for (uint64_t pml4_idx = 0; pml4_idx < 256; ++pml4_idx)`. Entries 256..511 (kernel higher-half mappings) are completely bypassed and preserved.
  - Recursively frees each present user leaf frame (`pmm_free_frame(leaf_phys)`), then PT table frame (`pmm_free_frame(pt_phys)`), then PD table frame (`pmm_free_frame(pd_phys)`), and then PDPT table frame (`pmm_free_frame(pdpt_phys)`).
  - Finally, `pmm_free_frame(pml4_phys)` reclaims the root PML4 frame.

### 1.2 Zombie Semantics & Hierarchy Linkage (`kernel/process.c`)
- In `kernel/process.c` lines 161–170 (`process_create`):
  New processes link their `sibling_node` to the parent's `children` list with `spinlock_lock_irqsave(&cur->lock)`.
- In `kernel/process.c` lines 257–306 (`process_exit`):
  Sets `proc->state = PROCESS_STATE_ZOMBIE`, preserves `proc->exit_code`, destroys handles, broadcasts on `proc->exit_wq`, and skips destruction if parent process is active:
  ```c
  if (has_no_threads) {
      struct process *parent = process_find_by_pid(ppid);
      if (parent == NULL || parent->state == PROCESS_STATE_DEAD) {
          process_destroy(proc);
      }
  }
  ```
- In `kernel/process.c` lines 180–192 (`process_destroy`):
  Reparents orphaned children to `kernel_process` (PID 1):
  ```c
  /* 1. Reparent children to kernel_process */
  uint64_t pflags = spinlock_lock_irqsave(&proc->lock);
  struct list_head *cpos = proc->children.next;
  while (cpos != &proc->children) {
      struct process *child = list_entry(cpos, struct process, sibling_node);
      cpos = cpos->next;
      list_del_init(&child->sibling_node);
      child->ppid = 1;
      uint64_t kflags = spinlock_lock_irqsave(&kernel_process.lock);
      list_add_tail(&child->sibling_node, &kernel_process.children);
      spinlock_unlock_irqrestore(&kernel_process.lock, kflags);
  }
  ```

### 1.3 `process_wait()` Semantics & Timeout Handling (`kernel/process.c:320-356`)
```c
bool process_wait(struct process *proc, int *exit_code, uint64_t timeout_ms) {
    if (proc == NULL) {
        return false;
    }

    bool exited = false;
    if (timeout_ms == 0) {
        exited = process_exit_condition(proc);
    } else if (timeout_ms == UINT64_MAX) {
        wait_event(&proc->exit_wq, process_exit_condition, proc);
        exited = process_exit_condition(proc);
    } else {
        exited = wait_event_timeout(&proc->exit_wq, process_exit_condition, proc, timeout_ms);
    }

    if (exited) {
        if (exit_code != NULL) {
            *exit_code = proc->exit_code;
        }

        /* Remove proc from parent's children list */
        struct process *parent = process_find_by_pid(proc->ppid);
        if (parent != NULL) {
            uint64_t parent_flags = spinlock_lock_irqsave(&parent->lock);
            list_del_init(&proc->sibling_node);
            spinlock_unlock_irqrestore(&parent->lock, parent_flags);
        } else {
            list_del_init(&proc->sibling_node);
        }

        /* Reap child: transition to DEAD, reclaim resources, free table slot */
        process_destroy(proc);
        return true;
    }

    return false;
}
```

### 1.4 Empirical Verification Results (QEMU Serial Execution)
1. **Pristine Boot Execution Output**:
   - `[process] Running Process & Thread Lifecycle Self-Test...`
   - `[PASS] Process hierarchy (children & sibling_node) verified.`
   - `[PASS] Zombie process retention with active parent verified.`
   - `[PASS] process_wait zombie reaping and hierarchy unlinking verified.`
   - `[PASS] Address space reclamation verified with zero frame leaks.`
   - `[PASS] process_exec error rollback and unwinding verified with zero frame leaks.`
   - `>>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<`
   - `[waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...`
   - `Child process PID 8 created, handle 0x00010000 created, object refcount=2`
   - `process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=DEAD`
   - `[PASS] Process termination resource cleanup verified!`
   - Terminal prompt reached: `KOS debug terminal ready. KOS> `
2. **Stress & Boundary Test Execution**:
   - Immediate check (`timeout=0`) on active process returned `false` without blocking.
   - Timed wait (`timeout=20ms`) on active process expired after 20 ms and returned `false`.
   - Immediate check (`timeout=0`) on zombie process reaped cleanly with exit code 88, transitioning state to `DEAD`.
   - Multi-level mapping test across PML4, PDPT, PD levels (0x400000, 0x8000000000, 0x7FFFFFFFE000): 0 leaked physical frames (`pmm_free_frame_count()` matched baseline before allocation).
   - Higher-half kernel PML4 entries 256..511 were byte-for-byte identical before and after destruction.
   - Orphaned child (parent terminates before child) reparented to `kernel_process` (PID 1) upon parent destruction, exited to `PROCESS_STATE_ZOMBIE`, and reaped by `kernel_process` via `process_wait()`.
   - Pre-existing zombie child (child terminates before parent) reparented to `kernel_process` with exit code 73 intact, cleanly reaped by `kernel_process` via `process_wait()`.

---

## 2. Logic Chain

1. **Resource Reclamation Guarantee**:
   - Observation 1.1 demonstrates that `process_destroy()` inspects `proc->pml4_physical` and conditionally invokes `vmm_destroy_address_space(user_pml4)` if and only if `user_pml4 != 0` and `user_pml4 != kernel_process.pml4_physical`.
   - In `vmm_clear_user_address_space()`, iteration bounds `pml4_idx < 256` structurally prevent traversal into kernel higher-half mappings (256..511).
   - Every mapped leaf frame and every intermediate table page allocated by `next_table()` is freed using `pmm_free_frame()`.
   - Our empirical test confirmed that creating multi-level mappings across discrete PDPT/PD branches and destroying the address space results in exactly 0 leaked frames, and all 256 kernel higher-half PML4 entries remain unaltered.

2. **Zombie Retention & Race Freedom**:
   - Observation 1.2 demonstrates that `process_exit()` checks whether the parent process is active (`parent != NULL && parent->state != PROCESS_STATE_DEAD`). If active, `process_destroy(proc)` is skipped, preserving `proc->state = PROCESS_STATE_ZOMBIE` and keeping `proc->exit_code` intact.
   - When the parent invokes `process_wait(proc, &exit_code, timeout_ms)`, `wait_event_timeout` unblocks, retrieves `exit_code`, unlinks `sibling_node` under `parent->lock`, and calls `process_destroy(proc)`.
   - This prevents premature slot recycling while guaranteeing atomic hierarchy cleanup upon wait completion.

3. **Orphan Reparenting Integrity**:
   - When a parent process terminates before its child, step 1 of `process_destroy(parent)` iterates over `proc->children`, resets `child->ppid = 1`, and moves `child->sibling_node` into `kernel_process.children`.
   - When the orphaned child terminates, its parent PID is 1. It enters `PROCESS_STATE_ZOMBIE` attached to `kernel_process`.
   - Similarly, if a child exited earlier (becoming a zombie) and the parent terminates without waiting, `process_destroy(parent)` reparents the zombie child to `kernel_process`, preserving its exit code for retrieval.

4. **Timeout Handling Robustness**:
   - Observation 1.3 shows distinct handling for `timeout_ms == 0` (immediate non-blocking poll of `process_exit_condition`), `timeout_ms == UINT64_MAX` (unconditional block), and `0 < timeout_ms < UINT64_MAX` (deadline sleep via `wait_event_timeout`).
   - If the timeout expires before the child exits, `wait_event_timeout` removes the entry from `proc->exit_wq` and returns `false`. `process_wait` leaves the child in its active state without reaping or corrupting the process list.
   - If the child has already exited prior to `process_wait`, `process_exit_condition` evaluates to `true` immediately on entry (lines 131–133 of `wait.c`), reaping the child immediately with zero unnecessary context switches.

---

## 3. Caveats

1. **Orphan Automatic Harvesting**:
   - In standard POSIX systems, PID 1 runs an explicit wait loop to harvest terminated orphaned children. In KOS, orphaned processes reparented to `kernel_process` remain in `PROCESS_STATE_ZOMBIE` until explicitly reaped via `process_wait` or until system shutdown. Future user-mode init daemon milestones should implement a background `wait()` reaping loop for PID 1.
2. **Process Slot Upper Limit**:
   - Process slots are statically bounded by `PROCESS_MAX_COUNT = 32`. Creating more than 30 concurrent processes without reaping will exhaust process descriptors.

---

## 4. Conclusion

Milestone M4 (Requirement R3) satisfies all lifecycle, hierarchy, synchronization, and resource reclamation criteria:
- `vmm_destroy_address_space` cleanly reclaims lower-half user leaf and table frames while preserving kernel higher-half mappings with zero leaks.
- Zombie processes persist until reaped by their parent via `process_wait()`.
- Orphaned children (both active and pre-existing zombies) are cleanly reparented to `kernel_process` (PID 1).
- `process_wait()` accurately supports non-blocking polling (`timeout=0`), timed expiration, and immediate zombie reaping.
- Live QEMU execution passes 100% of self-tests and boots to the interactive shell.
- Final Verdict: **APPROVE**.

---

## 5. Verification Method

### 5.1 Build Command
```pwsh
.\tools\kos-tool.exe build
```
**Expected**:
- Exit code 0, 0 compiler warnings, 0 compiler errors.
- `Kernel ELF built with 668 embedded symbols: build/kos.elf`
- `UEFI ESP staged: build/esp`

### 5.2 QEMU Live Execution Command
```pwsh
$proc = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList '-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\edk2-x86_64-code.fd" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none' -PassThru
Start-Sleep -Seconds 8
Stop-Process -Id $proc.Id -Force
Get-Content build/qemu_serial.log -Tail 60
```
**Expected Verifications in `build/qemu_serial.log`**:
- `[PASS] Process hierarchy (children & sibling_node) verified.`
- `[PASS] Zombie process retention with active parent verified.`
- `[PASS] process_wait zombie reaping and hierarchy unlinking verified.`
- `[PASS] Address space reclamation verified with zero frame leaks.`
- `[PASS] process_exec error rollback and unwinding verified with zero frame leaks.`
- `>>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<`
- `process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=DEAD`
- `KOS debug terminal ready. KOS> `

### 5.3 Invalidation Conditions
1. Calling `process_destroy()` leaks PMM physical frames on private user address spaces.
2. `vmm_destroy_address_space()` modifies or frees PML4 entries in index range 256..511.
3. `process_wait(proc, exit, 0)` returns `true` for a running (`ACTIVE`) child.
4. An orphaned child is not re-linked to `kernel_process` upon parent destruction.
