# Handoff Report: Milestone M4 — Process & Thread Lifecycle Subsystem (Gen 2)

**Agent ID**: `worker_m4_gen2`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\worker_m4_gen2`  
**Target Milestone**: K14 (Milestone M4 / Requirement R3)  
**Parent Agent ID**: `c843668f-19e3-4d81-abc8-ea4359b7dde0` (`parent`)  
**Date**: 2026-09-18  

---

## 1. Observation

### 1.1 Process Hierarchy and Structure Changes (`include/kos/process.h`)
- In `include/kos/process.h` lines 27–42:
  ```c
  struct process {
      uint64_t pid;
      char name[PROCESS_NAME_MAX];
      uint64_t ppid;
      enum process_state state;
      int exit_code;
      struct list_head threads;
      uint32_t thread_count;
      uint64_t pml4_physical;
      kos_spinlock_t lock;
      struct list_head process_node;
      struct list_head children;
      struct list_head sibling_node;
      struct handle_table *handles;
      struct wait_queue exit_wq;
  };
  ```
- Function declarations added at lines 49 and 57:
  ```c
  int process_exec(const char *path, int argc, char *const argv[]);
  bool process_lifecycle_self_test(void);
  ```

### 1.2 Hierarchy Linkage on Creation (`kernel/process.c`)
- In `kernel/process.c:138-167`:
  `list_init(&proc->children);` and `list_init(&proc->sibling_node);` are initialized.
  If parent process exists (`cur != NULL`):
  ```c
  if (cur != NULL) {
      uint64_t pflags = spinlock_lock_irqsave(&cur->lock);
      list_add_tail(&proc->sibling_node, &cur->children);
      spinlock_unlock_irqrestore(&cur->lock, pflags);
  } else {
      uint64_t kflags = spinlock_lock_irqsave(&kernel_process.lock);
      list_add_tail(&proc->sibling_node, &kernel_process.children);
      spinlock_unlock_irqrestore(&kernel_process.lock, kflags);
  }
  ```

### 1.3 Zombie Reaping Preservation (`kernel/process.c`)
- In `process_exit(proc, exit_code)` (`kernel/process.c:266-271`):
  ```c
  /* 4. IMPORTANT: Do NOT automatically destroy if parent is active! */
  if (has_no_threads) {
      struct process *parent = process_find_by_pid(ppid);
      if (parent == NULL || parent->state == PROCESS_STATE_DEAD) {
          process_destroy(proc);
      }
  }
  ```
- In `reaper_thread_entry` (`kernel/process.c:565-578`):
  ```c
  bool is_zombie_empty = (proc->state == PROCESS_STATE_ZOMBIE && proc->thread_count == 0);
  uint64_t ppid = proc->ppid;
  spinlock_unlock_irqrestore(&proc->lock, pflags);

  if (is_zombie_empty) {
      /* Only destroy if parent does not exist or is dead */
      struct process *parent = process_find_by_pid(ppid);
      if (parent == NULL || parent->state == PROCESS_STATE_DEAD) {
          process_destroy(proc);
      }
  }
  ```

### 1.4 Parent Wait Synchronization & Reaping (`kernel/process.c`)
- In `process_wait(proc, exit_code, timeout_ms)` (`kernel/process.c:290-323`):
  ```c
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
  ```

### 1.5 Recursive Resource Reclamation (`kernel/process.c`)
- In `process_destroy(proc)` (`kernel/process.c:174-245`):
  - Reparents remaining children to `kernel_process` (PID 1).
  - Unlinks `proc->sibling_node`.
  - Dequeues all threads from sleep queue and reaper queue; frees thread stacks with `kfree()`; returns slots via `task_free_slot()`.
  - Transitions `proc->state = PROCESS_STATE_DEAD`, removes from `process_list`, decrements `active_process_count`.
  - Destroys handle table via `handle_table_destroy(ht)`.
  - Reclaims user address space:
    ```c
    uint64_t user_pml4 = proc->pml4_physical;
    proc->pml4_physical = 0;
    if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical) {
        vmm_destroy_address_space(user_pml4);
    }
    ```
  - Wakes all waiters on `proc->exit_wq`.

### 1.6 `process_exec` Primitive (`kernel/process.c`)
- In `process_exec(path, argc, argv)` (`kernel/process.c:325-385`):
  - Validates `path` and current process `proc = process_get_current()`.
  - Allocates private address space: `uint64_t pml4_phys = vmm_create_address_space();`.
  - Loads ELF: `elf_load_binary(path, pml4_phys, &img);`. On error: calls `vmm_destroy_address_space(pml4_phys)` and returns -1.
  - Formats user stack: `process_setup_user_stack(pml4_phys, argc, argv, &user_rsp);`. On error: calls `vmm_destroy_address_space(pml4_phys)` and returns -1.
  - Cleans up old PML4 if private user PML4.
  - Assigns `proc->pml4_physical = pml4_phys;`.
  - Updates `proc->name` from basename of binary path.
  - Configures current thread kernel stack in TSS RSP0: `tss_set_rsp0(((uint64_t)cur_thread->stack + cur_thread->stack_size) & ~0xFull);`.
  - Switches CR3: `cpu_write_cr3(pml4_phys);`.
  - Enters Ring 3 userspace: `enter_userspace(img.entry_point, user_rsp);`.

### 1.7 Scheduler Address Space Switching (`kernel/task.c`)
- In `schedule()` (`kernel/task.c:301-305`):
  ```c
  if (next->process != NULL && next->process->pml4_physical != 0) {
      if (previous == NULL || previous->process != next->process) {
          cpu_write_cr3(next->process->pml4_physical);
      }
  }
  ```
  And `tss_set_rsp0()` continues to be invoked on every thread context switch.

### 1.8 Toolchain Build and Verification Output
- Build command: `.\tools\kos-tool.exe build`
  ```
  Kernel ELF built with 668 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  Exit code: 0 (0 warnings, 0 errors)
  ```
- Live QEMU boot test execution output (`build/qemu_serial.log`):
  ```
  [    1.070] [TID 1] [INFO ] [process] Running Process & Thread Lifecycle Self-Test...
  [    1.080] [TID 1] [INFO ]   [PASS] Process hierarchy (children & sibling_node) verified.
  [    1.090] [TID 1] [INFO ]   [PASS] Zombie process retention with active parent verified.
  [    1.090] [TID 1] [INFO ]   [PASS] process_wait zombie reaping and hierarchy unlinking verified.
  [    1.100] [TID 1] [INFO ]   [PASS] Address space reclamation verified with zero frame leaks.
  [    1.110] [TID 1] [INFO ]   [PASS] process_exec error rollback and unwinding verified with zero frame leaks.
  [    1.110] [TID 1] [INFO ] >>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<
  ...
  [    1.780] [TID 1] [INFO ] [waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...
  [    1.780] [TID 1] [INFO ]   Child process PID 8 created, handle 0x00010000 created, object refcount=2
  [    1.810] [TID 1] [INFO ]   process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=DEAD
  [    1.810] [TID 1] [INFO ]   After process exit: object refcount=1, destructor_called=0
  [    1.820] [TID 1] [INFO ]   Child handle table cleaned up, object refcount decremented.
  [    1.820] [TID 1] [INFO ]   Parent successfully synchronized with child exit via exit_wq.
  [    1.830] [TID 1] [INFO ]   Exit code 77 cleanly delivered to parent.
  [    1.840] [TID 1] [INFO ]   [PASS] Process termination resource cleanup verified!
  [    1.840] [TID 1] [INFO ] --------------------------------------------------
  [    1.850] [TID 1] [INFO ] >>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<
  ...
  [    2.010] [TID 1] [INFO ] >>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<
  [    2.030] [TID 1] [INFO ] KOS debug terminal ready.
  KOS> 
  ```

---

## 2. Logic Chain

1. **Hierarchy Tracking**:
   - In UNIX process models, parent processes must track their spawned children to allow synchronized waiting and resource management.
   - Adding `children` and `sibling_node` list heads to `struct process` enables $O(1)$ child insertion and deletion.
   - Locking `cur->lock` during child addition and `parent->lock` during child removal protects against concurrent list mutations without global lock inversion.

2. **Zombie Retention & Race Elimination**:
   - Prior to this milestone, `reaper_thread` automatically invoked `process_destroy(proc)` whenever `thread_count` hit 0, immediately marking the process `DEAD` and permitting slot reuse.
   - A fast-terminating child process would be destroyed before the parent thread executed `process_wait()`, losing the exit code and causing wait failures.
   - Checking `parent != NULL && parent->state != PROCESS_STATE_DEAD` in both `process_exit()` and `reaper_thread_entry()` guarantees that a process remains in `PROCESS_STATE_ZOMBIE` until its parent explicitly reaps it via `process_wait()`.
   - In `process_wait()`, the parent retrieves `exit_code`, unlinks `sibling_node`, and calls `process_destroy(proc)`, transitioning the process to `DEAD` and making the process slot safely eligible for reuse.

3. **Zero-Leak Memory Reclamation**:
   - A user process creates private lower-half PML4 page tables and maps physical frames via `vmm_create_address_space()` and `vmm_map_page_in()`.
   - When a process terminates, failing to tear down its lower-half mappings permanently leaks PMM physical frames.
   - In `process_destroy(proc)`, identifying private PML4 roots (`proc->pml4_physical != 0 && proc->pml4_physical != kernel_process.pml4_physical`) and invoking `vmm_destroy_address_space(user_pml4)` recursively frees all PML4/PDPT/PD/PT tables and user data frames.
   - Verified via `pmm_free_frame_count()` before and after allocation/destruction, demonstrating 0 leaked frames.

4. **Address Space Isolation & Hardware Switching**:
   - AMD64 hardware isolation requires loading CR3 with the active process's PML4 root.
   - In `schedule()`, when switching to a task belonging to a different process with private PML4, `cpu_write_cr3(next->process->pml4_physical)` ensures the target address space is mapped before `task_switch()`.
   - `tss_set_rsp0()` ensures interrupts or exceptions originating in Ring 3 switch safely to that thread's kernel stack.

5. **`process_exec` Integrity and Error Rollback**:
   - Execution transitions a process to a new binary. If binary parsing or stack setup fails halfway, partially allocated page tables must not be orphaned.
   - `process_exec` creates a private PML4, loads segments with `elf_load_binary`, sets up ABI stack with `process_setup_user_stack`, and cleanly calls `vmm_destroy_address_space` on any failure before CR3 reload or Ring 3 jump.

---

## 3. Caveats

1. **Multiprocessor (SMP) Scaling**:
   - The current hierarchy locking uses uniprocessor spinlocks (`spinlock_lock_irqsave`). If SMP kernel scaling is introduced in future milestones, child list traversals should be converted to read-copy-update (RCU) or read-write locks for higher throughput.
2. **Process Table Capacity**:
   - Process slots are allocated from the static table `processes[PROCESS_MAX_COUNT]` (32 processes). Dynamic heap-backed process descriptor allocation can be considered if the process limit needs to exceed 32.

---

## 4. Conclusion

Milestone M4 (Requirement R3) is completely implemented and verified:
- `struct process` contains `children` and `sibling_node` list tracking with parent locking.
- Zombie processes remain in `PROCESS_STATE_ZOMBIE` until reaped by parent via `process_wait()`.
- `process_wait()` retrieves exit status, unlinks from hierarchy, and reaps the child to `PROCESS_STATE_DEAD`.
- `process_destroy()` completely reclaims handle tables, threads, and user page tables (`vmm_destroy_address_space`) with zero leaks.
- `process_exec()` loads ELF binaries into private address spaces, sets up System V AMD64 user stack, switches CR3, and enters Ring 3 userspace with complete error rollback.
- `schedule()` performs CR3 address space switching and maintains TSS RSP0 updates.
- Toolchain build passes cleanly with 0 warnings, 0 errors.
- Live QEMU boot test passes 100% of all self-tests.

---

## 5. Verification Method

### 5.1 Build Verification
Run the toolchain build command from project root:
```pwsh
.\tools\kos-tool.exe build
```
**Expected Output**:
- Exit code: 0
- 0 compiler warnings, 0 compiler errors.
- `Kernel ELF built with 668 embedded symbols: build/kos.elf`
- `UEFI ESP staged: build/esp`

### 5.2 QEMU Live Test Execution
Execute QEMU headless and verify serial log output:
```pwsh
$proc = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList '-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\edk2-x86_64-code.fd" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none' -PassThru
Start-Sleep -Seconds 8
Stop-Process -Id $proc.Id -Force
Get-Content build/qemu_serial.log -Tail 60
```
**Expected Verification Results**:
- `[process] Running Process & Thread Lifecycle Self-Test...`
- `[PASS] Process hierarchy (children & sibling_node) verified.`
- `[PASS] Zombie process retention with active parent verified.`
- `[PASS] process_wait zombie reaping and hierarchy unlinking verified.`
- `[PASS] Address space reclamation verified with zero frame leaks.`
- `[PASS] process_exec error rollback and unwinding verified with zero frame leaks.`
- `>>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<`
- `[PASS] Process termination resource cleanup verified!` in `waittest`
- All subsystem self-tests (PMM, VMM, ELF, handles, wait queue, net, synctest) pass 100%.
- System reaches `KOS debug terminal ready. KOS> `.

### 5.3 Invalidation Conditions
1. A child process with an active parent is deleted or transitioned to `DEAD` before `process_wait()` is called.
2. Repeated execution of `process_create` / `process_destroy` or `process_exec` error paths leaks physical memory frames.
3. Scheduler fails to switch CR3 when alternating between processes with distinct PML4 addresses.
