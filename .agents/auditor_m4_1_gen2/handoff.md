# Forensic Audit Report: Milestone M4 — Process Execution & Lifecycle (Gen 2)

**Work Product**: Milestone M4 (`include/kos/process.h`, `kernel/process.c`, `kernel/task.c`)  
**Profile**: General Project  
**Integrity Mode**: Development  
**Auditor**: Forensic Auditor M4 Gen 2 (`auditor_m4_1_gen2`)  
**Verdict**: **CLEAN**

---

## 1. Observation

### 1.1 Toolchain Build Authenticity
- **Command**: `.\tools\kos-tool.exe build`
- **Exit Code**: 0 (0 warnings, 0 errors)
- **Output**:
  ```text
  Kernel ELF built with 668 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  ```

### 1.2 Binary Symbol Verification
- **Command**: `& "C:/Program Files/LLVM/bin/llvm-nm.exe" build/kos.elf | Select-String "process_exec|process_lifecycle_self_test|process_destroy|process_wait"`
- **Output**:
  ```text
  ffffffff8000ed20 T process_destroy
  ffffffff8000f410 T process_exec
  ffffffff8000feb0 T process_lifecycle_self_test
  ffffffff8000f280 T process_wait
  ```

### 1.3 Static Analysis of Implementation Code
- **`include/kos/process.h`**:
  - Added hierarchy list pointers to `struct process` (lines 38-39):
    ```c
    struct list_head children;
    struct list_head sibling_node;
    ```
  - Added function signatures for `process_exec` (line 49) and `process_lifecycle_self_test` (line 57).
- **`kernel/task.c`**:
  - In `schedule()` (lines 301-312):
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
    ```
- **`kernel/process.c`**:
  - **Hierarchy tracking** (`process_create`, lines 161-169): Newly created processes are enqueued into `cur->children` (or `kernel_process.children`) protected by `spinlock_lock_irqsave`.
  - **Zombie state retention** (`process_exit`, lines 299-305; `reaper_thread_entry`, lines 529-536): If the parent process is alive and active, the child is NOT auto-destroyed upon reaching 0 threads. It remains in `PROCESS_STATE_ZOMBIE`.
  - **Parent wait & reaping** (`process_wait`, lines 335-354): When a child reaches ZOMBIE or DEAD state, `process_wait()` extracts `exit_code`, safely unlinks `proc->sibling_node` under `parent->lock`, and invokes `process_destroy(proc)`.
  - **Recursive resource reclamation** (`process_destroy`, lines 180-248):
    - Reparents remaining children to `kernel_process` (PID 1).
    - Safely dequeues and frees all threads and thread stacks.
    - Destroys handle tables via `handle_table_destroy(ht)`.
    - Recursively frees user page tables and physical frames:
      ```c
      uint64_t user_pml4 = proc->pml4_physical;
      proc->pml4_physical = 0;
      if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical) {
          vmm_destroy_address_space(user_pml4);
      }
      ```
  - **`process_exec` execution primitive** (lines 358-427):
    - Line 369: `uint64_t pml4_phys = vmm_create_address_space();`
    - Line 376: `bool ok = elf_load_binary(path, pml4_phys, &img);` (calls `vmm_destroy_address_space(pml4_phys)` on failure)
    - Line 384: `ok = process_setup_user_stack(pml4_phys, argc, argv, &user_rsp);` (calls `vmm_destroy_address_space(pml4_phys)` on failure)
    - Line 393: Destroys previous private PML4 if replacing an existing user process address space
    - Line 416: Updates `tss_set_rsp0()` with the kernel stack top
    - Line 420: Switches CR3 via `cpu_write_cr3(pml4_phys)`
    - Line 423: Calls `enter_userspace(img.entry_point, user_rsp)`

### 1.4 Disassembly Verification (`llvm-objdump`)
- Disassembly of `process_exec` (`0xffffffff8000f410`):
  - Line `0xffffffff8000f44c`: `callq vmm_create_address_space`
  - Line `0xffffffff8000f463`: `callq elf_load_binary`
  - Line `0xffffffff8000f481`: `callq process_setup_user_stack`
  - Line `0xffffffff8000f48d`: `callq vmm_destroy_address_space` (error branch)
  - Line `0xffffffff8000f4b8`: `callq vmm_destroy_address_space` (old PML4 replacement branch)
  - Line `0xffffffff8000f533`: `callq tss_set_rsp0`
  - Line `0xffffffff8000f53b`: `callq cpu_write_cr3`
  - Line `0xffffffff8000f548`: `callq enter_userspace`
- Disassembly of `process_lifecycle_self_test` (`0xffffffff8000feb0`):
  - Genuinely invokes `process_create`, `process_exit`, `process_wait`, `pmm_free_frame_count`, `vmm_create_address_space`, `pmm_allocate_frame`, `vmm_map_page_in`, `process_destroy`, and `process_exec`.
  - Evaluates assertions with explicit error branches calling `log_error` and returning `0`.

### 1.5 Live QEMU Boot Test Log
- **Serial Log Output** (`build/qemu_serial.log` lines 45-51 and 165-177):
  ```text
  [    1.070] [TID 1] [INFO ] [process] Running Process & Thread Lifecycle Self-Test...
  [    1.080] [TID 1] [INFO ]   [PASS] Process hierarchy (children & sibling_node) verified.
  [    1.080] [TID 1] [INFO ]   [PASS] Zombie process retention with active parent verified.
  [    1.090] [TID 1] [INFO ]   [PASS] process_wait zombie reaping and hierarchy unlinking verified.
  [    1.090] [TID 1] [INFO ]   [PASS] Address space reclamation verified with zero frame leaks.
  [    1.100] [TID 1] [INFO ]   [PASS] process_exec error rollback and unwinding verified with zero frame leaks.
  [    1.110] [TID 1] [INFO ] >>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<
  ...
  [    1.760] [TID 1] [INFO ] [waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...
  [    1.760] [TID 1] [INFO ]   Child process PID 8 created, handle 0x00010000 created, object refcount=2
  [    1.790] [TID 1] [INFO ]   process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=DEAD
  [    1.790] [TID 1] [INFO ]   After process exit: object refcount=1, destructor_called=0
  [    1.800] [TID 1] [INFO ]   Child handle table cleaned up, object refcount decremented.
  [    1.800] [TID 1] [INFO ]   Parent successfully synchronized with child exit via exit_wq.
  [    1.810] [TID 1] [INFO ]   Exit code 77 cleanly delivered to parent.
  [    1.820] [TID 1] [INFO ]   [PASS] Process termination resource cleanup verified!
  [    1.820] [TID 1] [INFO ] --------------------------------------------------
  [    1.830] [TID 1] [INFO ] >>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<
  ...
  [    2.010] [TID 1] [INFO ] KOS debug terminal ready.
  KOS>
  ```

---

## 2. Logic Chain

1. **Absence of Hardcoded Bypasses or Mocks (Observation 1.3 & 1.4)**:
   - Static analysis and disassembly confirm no artificial constant return bypasses or pre-populated flags exist in `process_lifecycle_self_test()` or `process_exec()`.
   - All tests execute dynamic memory operations, examine physical frame counters from the Physical Memory Manager (`pmm_free_frame_count()`), and test state transitions.

2. **Genuine Hierarchy & Zombie Reaping Implementation (Observation 1.3 & 1.5)**:
   - `struct process` maintains bidirectional hierarchy linkages (`children` list head and `sibling_node`).
   - Prior to Milestone M4, dead threads immediately triggered `process_destroy()`. With the M4 changes, child processes verify `parent != NULL && parent->state != PROCESS_STATE_DEAD` before autodestruction, ensuring the zombie process remains until reaped by `process_wait()`.
   - Live QEMU execution verifies both subtest 2 of `process_lifecycle_self_test` (retention and exit code 42 verification) and subtest 5 of `waittest` (PID 8 child termination with exit code 77 cleanly reaped).

3. **Zero-Leak Memory Reclamation & Address Space Teardown (Observation 1.3, 1.4 & 1.5)**:
   - `process_destroy()` inspects `proc->pml4_physical` and explicitly invokes `vmm_destroy_address_space(user_pml4)`.
   - In `vmm_destroy_address_space()`, all lower-half PML4/PDPT/PD/PT tables and mapped physical frames are recursively freed via `pmm_free_frame()`.
   - In `process_lifecycle_self_test()`, subtest 3 maps physical frames to virtual addresses `0x400000` and `0x500000`, destroys the process, and asserts that `pmm_free_frame_count()` before and after are identical (0 leaked frames).
   - In subtest 4, `process_exec` error paths (invoking NULL or nonexistent ELF binaries) allocate and then destroy address spaces, verifying that error rollback returns 100% of allocated frames.

4. **Hardware Context & Scheduling Synchronization (Observation 1.3)**:
   - In `schedule()`, switching between tasks with different `pml4_physical` pointers updates CR3 via `cpu_write_cr3(next->process->pml4_physical)`.
   - TSS RSP0 is reloaded with the kernel stack top on every context switch, ensuring user-space interrupts or exceptions switch cleanly to kernel stack.

5. **Toolchain Compilation & Symbol Authenticity (Observation 1.1 & 1.2)**:
   - `kos-tool.exe build` compiles with zero warnings and zero errors under Clang `-Wall -Wextra -Werror`.
   - Embedded symbol table contains 668 symbols, including all M4 primitives: `process_destroy`, `process_exec`, `process_lifecycle_self_test`, and `process_wait`.

---

## 3. Caveats

- **SMP Synchronization**: Current spinlocks protect against local interrupt preemption (`spinlock_lock_irqsave`). If multicore SMP is introduced in future milestones, child list traversals across multiple concurrent CPUs will require read-write spinlocks or RCU.
- **Process Table Limit**: The static process table `processes[PROCESS_MAX_COUNT]` supports 32 concurrent processes. If future user applications require more than 32 concurrent processes, dynamic heap-backed descriptors may be considered.

---

## 4. Conclusion

### Forensic Audit Verdict
**VERDICT: CLEAN**

Milestone M4 implementation in `include/kos/process.h`, `kernel/process.c`, and `kernel/task.c` is fully authentic, free of mocks, facades, or test bypasses. Memory reclamation is genuine and verified with zero page frame leaks across repeated allocations and error unwinding.

---

## 5. Verification Method

### 5.1 Compilation Verification
Execute from project root:
```pwsh
.\tools\kos-tool.exe build
```
Verify exit code is 0, with 0 warnings and 0 errors, producing `build/kos.elf`.

### 5.2 Symbol Table Inspection
Execute:
```pwsh
& "C:/Program Files/LLVM/bin/llvm-nm.exe" build/kos.elf | Select-String "process_exec|process_lifecycle_self_test|process_destroy|process_wait"
```
Verify that all 4 symbols exist in the `.text` section (`T`).

### 5.3 Live QEMU Boot Execution
Run QEMU headless and verify serial log output:
```pwsh
$proc = Start-Process -FilePath 'C:\Program Files\qemu\qemu-system-x86_64.exe' -ArgumentList '-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\edk2-x86_64-code.fd" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none' -PassThru
Start-Sleep -Seconds 8
Stop-Process -Id $proc.Id -Force
Get-Content build/qemu_serial.log | Select-String "\[PASS\].*process"
```
Expected output:
- `[PASS] Process hierarchy (children & sibling_node) verified.`
- `[PASS] Zombie process retention with active parent verified.`
- `[PASS] process_wait zombie reaping and hierarchy unlinking verified.`
- `[PASS] Address space reclamation verified with zero frame leaks.`
- `[PASS] process_exec error rollback and unwinding verified with zero frame leaks.`
- `>>> [PASS] ALL PROCESS LIFECYCLE & RECLAMATION SELF-TESTS PASSED! <<<`
- `[PASS] Process termination resource cleanup verified!`
