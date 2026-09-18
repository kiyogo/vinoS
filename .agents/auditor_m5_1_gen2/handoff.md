# Forensic Audit Report: Milestone M5 (Gen 2) — Verification Suite & Process Lifecycle

**Agent**: Forensic Auditor M5 (Gen 2) (`auditor_m5_1_gen2`)  
**Target**: Milestone K14 (Milestone M5 / Acceptance Criteria Verification)  
**Parent Agent**: `c843668f-19e3-4d81-abc8-ea4359b7dde0` (`parent`)  
**Date**: 2026-09-18  
**Integrity Mode**: Development (per `ORIGINAL_REQUEST.md`, lines 61, 68)  
**Final Verdict**: **CLEAN**

---

## 1. Observation

### 1.1 Ground Truth Requirements and Architecture
- **`ORIGINAL_REQUEST.md`** (lines 91–105):
  - Acceptance Criteria 1: Valid ELF64 binaries load and execute in Ring 3 userspace successfully.
  - Acceptance Criteria 2: Invalid/corrupted ELF binaries fail gracefully with error codes and do not panic the kernel.
  - Acceptance Criteria 3: Memory protections strictly enforced: Ring 3 cannot access kernel-only pages without triggering a page fault handled by the kernel.
  - Acceptance Criteria 4: Hitting user stack guard page cleanly faults and terminates faulting process without crashing kernel.
  - Acceptance Criteria 5: TSS/RSP0 switching handles hardware interrupts (timer/PIT) and exceptions occurring in Ring 3 seamlessly, returning to userspace cleanly.
  - Acceptance Criteria 6: Parent processes can fork/create children, retrieve child exit status via `process_wait`, and zombie processes are properly reaped upon cleanup.
  - Acceptance Criteria 7: Repeated process creation and destruction loops demonstrate zero kernel heap or page frame leakage.
- **`PROJECT.md`** (lines 26, 35): Milestone M5 requires automated verification and shell test suite (`usertest`) verifying all acceptance criteria.

### 1.2 Static Source Code Inspection (`kernel/command.c`)
- Line 41: Forward declaration: `static void command_usertest(const char *arguments);`
- Line 50: Shell command help string lists `usertest`.
- Line 1327: Automated boot hook inside `command_synctest`: `command_usertest("all");` ensuring execution on every boot.
- Line 3810: Command registration table entry `{ "usertest", command_usertest }`.
- Lines 2104–2135: Whitespace-tolerant argument parsing helpers:
  - `usertest_is_empty(const char *arg)`: Handles NULL and whitespace-only strings.
  - `usertest_match_arg(const char *arg, const char *target)`: Matches arguments while skipping leading/trailing whitespace.
- Lines 2137–2181: Helper `create_test_elf`:
  - Dynamically synthesizes a conforming ELF64 binary image in memory with valid `Elf64_Ehdr` (magic `\x7fELF`, class 2 (64-bit), data 1 (2's complement little-endian), machine 62 (`EM_X86_64`), type 2 (`ET_EXEC`)) and a `PT_LOAD` program header mapped at `vaddr` (0x00400000) with permissions `PF_R | PF_X | PF_W`.
- Lines 2183–2191: Child thread launcher `usertest_child_launcher`:
  - Invokes `process_exec(path, 0, NULL)`.
  - On failure or exit, calls `process_exit(cur, res)` or `thread_exit(res)`.
- Lines 2196–2278: `test_valid_elf_execution()` (Test 1):
  - Machine payload: `mov rax, 4` (`KOS_SYSCALL_EXIT`), `mov rdi, 42`, `int 0x80`, `jmp $`.
  - Writes to `D:/valid.elf`, spawns `valid_proc`, waits via `process_wait(child, &exit_code, 3000)`.
  - Asserts `wait_ok == true`, `exit_code == 42`, `child->state == PROCESS_STATE_DEAD`, `final_frames == initial_frames`, `final_heap == initial_heap`.
- Lines 2283–2459: `test_corrupted_elf_rejection()` (Test 2):
  - Tests 6 distinct corruption/malformed scenarios:
    - 2a: Corrupted magic bytes (`'X','Y','Z'`) -> passed to `elf_load_binary`. Asserts failure.
    - 2b: Non-x86_64 target (`e_machine = EM_386`) -> passed to `elf_load_binary`. Asserts failure.
    - 2c: Entry point outside loadable segment (`e_entry = 0x00100000ull` < 2 MiB) -> passed to `elf_load_binary`. Asserts failure.
    - 2d: `p_memsz < p_filesz` -> passed to `elf_load_binary`. Asserts failure.
    - 2e: Truncated ELF header (< 64 bytes) -> passed to `elf_load_binary`. Asserts failure.
    - 2f: Full `process_exec` error unwinding: corrupted executable invoked via `process_exec`, returns -1, unwinds PML4, child exits with -1, parent reaps.
  - Asserts `final_frames == initial_frames` and `final_heap == initial_heap` across all 6 subcases.
- Lines 2464–2586: `test_kernel_mem_isolation()` (Test 3):
  - Subcase 3a: Ring 3 payload executes `movabs rax, 0xFFFFFFFF80000000; mov rbx, [rax]`.
  - Subcase 3b: Ring 3 payload executes `movabs rax, 0xFFFF800000000000; mov rbx, [rax]`.
  - In both subcases, AMD64 MMU raises #PF (error code 0x5: `P=1, W=0, U=1`). The kernel page fault handler intercepts the fault, issues SIGSEGV (-11) to `process_exit(cur, -11)`.
  - Parent verifies `wait_ok == true`, `exit_code == -11`, `child->state == PROCESS_STATE_DEAD`, and 0 memory leaks.
- Lines 2591–2713: `test_stack_guard_containment()` (Test 4):
  - Subcase 4a: Ring 3 payload executes `movabs rax, 0x00007FFFFFFEDFF8; mov qword [rax], 0x1234` (top of guard page).
  - Subcase 4b: Ring 3 payload executes `movabs rax, 0x00007FFFFFFED000; mov qword [rax], 0x5678` (base of guard page).
  - Both target the unmapped 4 KiB guard page (`USER_STACK_GUARD_PAGE = 0x00007FFFFFFED000ull`).
  - MMU raises #PF (error code 0x6: `P=0, W=1, U=1`). Kernel intercepts, issues SIGSEGV (-11).
  - Parent verifies `wait_ok == true`, `exit_code == -11`, `child->state == PROCESS_STATE_DEAD`, and 0 memory leaks.
- Lines 2718–2817: `test_preemption_hardware()` (Test 5):
  - Ring 3 payload executes a 30,000,000-iteration compute loop (`dec rcx; jnz`).
  - Hardware PIT timer fires at 100 Hz. CPU transitions from Ring 3 to Ring 0 via TSS RSP0, services IRQ0, decrements quantum, returns to Ring 3 via `iretq`.
  - Measures `elapsed_ticks = ticks_after - ticks_before`.
  - Explicitly asserts `elapsed_ticks > 0`, `wait_ok == true`, `exit_code == 55`, `child->state == PROCESS_STATE_DEAD`, and 0 memory leaks.
- Lines 2822–2898: `test_leak_50_iterations()` (Test 6):
  - Measures `baseline_frames` and `baseline_heap`.
  - Executes 50 consecutive full cycles: `process_create()`, `thread_create()`, `process_exec()`, user mode jump, `int 0x80` exit, `process_wait()`, and reaping.
  - Measures `final_frames` and `final_heap`.
  - Explicitly asserts `final_frames == baseline_frames` and `final_heap == baseline_heap`.
- Lines 2900–2963: `command_usertest(const char *arguments)`:
  - Dispatches subtests based on argument: `valid`, `corrupted`, `kernel_mem`, `stack_overflow`, `preemption`, `leak`, or `all`.
  - Evaluates `all_passed` flag. Prints failure message if ANY test returns false.

### 1.3 Build and Symbol Authenticity Verification
- Executed: `.\tools\kos-tool.exe build`
  - Output: Exit code 0.
  - 0 warnings, 0 errors (compiled with `-Wall -Wextra -Werror`).
  - Output confirmation: `Kernel ELF built with 703 embedded symbols: build/kos.elf` and `UEFI ESP staged: build/esp`.
- Executed: `llvm-nm.exe build/kos.elf | Select-String "command_usertest"`
  - Output: `ffffffff8000b880 t command_usertest`
- Executed: `llvm-nm.exe build/kos.elf | Select-String "test_"`
  - Verified symbols and payloads present in `.rodata` and `.text`:
    - `ffffffff80030700 r test_valid_elf_execution.valid_code`
    - `ffffffff80030720 r test_corrupted_elf_rejection.valid_stub_code`
    - `ffffffff80030740 r test_kernel_mem_isolation.kmem_code_1`
    - `ffffffff80030760 r test_kernel_mem_isolation.kmem_code_2`
    - `ffffffff80030780 r test_stack_guard_containment.guard_code_1`
    - `ffffffff800307b0 r test_stack_guard_containment.guard_code_2`
    - `ffffffff800307e0 r test_preemption_hardware.preempt_code`
    - `ffffffff80030800 r test_leak_50_iterations.iter_code`
    - `ffffffff8000ec00 t usertest_child_launcher`
- Disassembled `command_usertest` via `llvm-objdump.exe`:
  - Verified function size is ~8.3 KiB (`0x209e` bytes) containing genuine instruction sequences, hardware state checks, error branches, and cleanup invocations.

### 1.4 Live QEMU Empirical Execution (`build/qemu_serial.log`)
- Booted fresh QEMU instance:
  `C:\Program Files\qemu\qemu-system-x86_64.exe -machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file="C:\Program Files\qemu\share\edk2-x86_64-code.fd" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none`
- Extracted verbatim log segment from `build/qemu_serial.log` (lines 210–246):
```
[    2.100] [TID 1] [INFO ] ==================================================
[    2.110] [TID 1] [INFO ] === Milestone K14 Ring 3 & Process Test Suite ===
[    2.120] [TID 1] [INFO ] ==================================================
[    2.120] [TID 1] [INFO ] [usertest] 1. Valid ELF64 binary execution...
[    2.140] [TID 1] [INFO ]   [PASS] Test 1: Valid ELF64 binary execution (exit_code=42, 0 leaks)
[    2.140] [TID 1] [INFO ] [usertest] 2. Corrupted / invalid ELF rejection...
[    2.150] [TID 1] [INFO ]   [PASS] 2a: Rejected invalid ELF magic bytes.
[    2.160] [TID 1] [INFO ]   [PASS] 2b: Rejected non-x86_64 target machine.
[    2.160] [TID 1] [INFO ]   [PASS] 2c: Rejected entry point outside loadable segment.
[    2.170] [TID 1] [INFO ]   [PASS] 2d: Rejected invalid PT_LOAD segment bounds.
[    2.180] [TID 1] [INFO ]   [PASS] 2e: Rejected truncated ELF file.
[    2.180] [TID 1] [INFO ]   [PASS] 2f: process_exec error cleanly returned -1 and unwound.
[    2.190] [TID 1] [INFO ]   [PASS] Test 2: Corrupted/invalid ELF binary rejection (6 subcases, 0 leaks)
[    2.200] [TID 1] [INFO ] [usertest] 3. Kernel memory isolation enforcement...
[    2.210] [TID 23] [WARN ] User process page fault: CR2=0xFFFFFFFF80000000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x5 [P=1 W=0 U=1 R=0 I=0] (page protection violation (write to read-only or supervisor page))
[    2.210] [TID 23] [WARN ] Terminating user process PID 11 (kmem1.elf) due to page fault (SIGSEGV -11)
[    2.220] [TID 1] [INFO ]   [PASS] 3a: Access to 0xFFFFFFFF80000000 cleanly trapped (SIGSEGV -11).
[    2.230] [TID 24] [WARN ] User process page fault: CR2=0xFFFF800000000000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x5 [P=1 W=0 U=1 R=0 I=0] (page protection violation (write to read-only or supervisor page))
[    2.230] [TID 24] [WARN ] Terminating user process PID 12 (kmem2.elf) due to page fault (SIGSEGV -11)
[    2.240] [TID 1] [INFO ]   [PASS] 3b: Access to 0xFFFF800000000000 cleanly trapped (SIGSEGV -11).
[    2.240] [TID 1] [INFO ]   [PASS] Test 3: Kernel memory isolation enforcement (2 higher-half addresses, SIGSEGV -11 trapped, 0 leaks)
[    2.250] [TID 1] [INFO ] [usertest] 4. User stack guard page containment...
[    2.260] [TID 25] [WARN ] User process page fault: CR2=0x00007FFFFFFEDFF8, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x6 [P=0 W=1 U=1 R=0 I=0] (non-present page (unmapped virtual address))
[    2.260] [TID 25] [WARN ] Terminating user process PID 13 (guard1.elf) due to page fault (SIGSEGV -11)
[    2.270] [TID 1] [INFO ]   [PASS] 4a: Guard page top access cleanly trapped (SIGSEGV -11).
[    2.280] [TID 26] [WARN ] User process page fault: CR2=0x00007FFFFFFED000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x6 [P=0 W=1 U=1 R=0 I=0] (non-present page (unmapped virtual address))
[    2.280] [TID 26] [WARN ] Terminating user process PID 14 (guard2.elf) due to page fault (SIGSEGV -11)
[    2.290] [TID 1] [INFO ]   [PASS] 4b: Guard page base access cleanly trapped (SIGSEGV -11).
[    2.290] [TID 1] [INFO ]   [PASS] Test 4: User stack guard page containment (2 guard boundaries trapped, 0 leaks)
[    2.300] [TID 1] [INFO ] [usertest] 5. TSS/RSP0 Ring 3 hardware preemption...
[    2.480] [TID 1] [INFO ]   Preemption test elapsed timer ticks: 16
[    2.480] [TID 1] [INFO ]   [PASS] Test 5: TSS/RSP0 Ring 3 hardware preemption (exit_code=55, elapsed_ticks=16, 0 leaks)
[    2.490] [TID 1] [INFO ] [usertest] 6. 50-iteration zero memory leak test...
[    2.540] [TID 1] [INFO ]   [PASS] Test 6: 50-iteration zero memory leak test (frames=51052, heap_allocs=3, 0 leaks)
[    2.550] [TID 1] [INFO ] --------------------------------------------------
[    2.550] [TID 1] [INFO ] >>> [PASS] ALL K14 USERSPACE & PROCESS TESTS PASSED! <<<
[    2.560] [TID 1] [INFO ] ==================================================
...
[    3.810] [TID 1] [INFO ] KOS debug terminal ready.
KOS> 
```

---

## 2. Logic Chain

1. **Premise 1: Absence of Hardcoded Outputs or Facades**:
   - The test implementation in `kernel/command.c` checks runtime return values from `process_wait()`, `elf_load_binary()`, `timer_get_ticks()`, `pmm_free_frame_count()`, and `heap_active_allocation_count()`.
   - Any assertion failure aborts with `log_error` and returns `false`, which propagates to `all_passed = false` and prints `>>> [FAIL] SOME K14 USERSPACE & PROCESS TESTS FAILED! <<<`.
   - The binary disassembly confirms real execution paths, not constant prints or bypasses.

2. **Premise 2: Test 1 Genuinely Switches Privilege and Exits via Syscall**:
   - Test 1 creates an ELF binary with entry point `0x00400080` (offset by 128 bytes of headers).
   - Machine instructions execute `mov rax, 4; mov rdi, 42; int 0x80`.
   - IDT vector 0x80 is configured with DPL=3, allowing Ring 3 invocation.
   - Hardware switches to the kernel stack specified by TSS RSP0.
   - Vector 0x80 dispatches to `kos_syscall_dispatch()`, which maps syscall 4 (`KOS_SYSCALL_EXIT`) to `process_exit(proc, 42)`.
   - The child's state transitions to `PROCESS_STATE_ZOMBIE`, waking the parent via `exit_wq`.
   - Parent's `process_wait()` retrieves exit code 42, reaps the child to `PROCESS_STATE_DEAD`, and validates that PMM frame count and active heap allocations remain identical to baseline.

3. **Premise 3: Test 2 Genuinely Validates Binary Rejection and Unwinding**:
   - All 6 corruption modes (bad magic, invalid machine ID, entry point outside segment boundaries, memsz < filesz, truncated file length, and exec unwinding) invoke the actual loader functions `elf_load_binary()` and `process_exec()`.
   - Each error path triggers early return `false` or `-1`, unmapping allocated page tables and freeing memory.
   - Verified 0 frame and 0 heap leaks across all 6 subcases.

4. **Premise 4: Test 3 Genuinely Traps Kernel Memory Isolation Violations**:
   - From Ring 3 (CPL=3), dereferencing higher-half addresses `0xFFFFFFFF80000000` and `0xFFFF800000000000` causes the hardware AMD64 MMU to assert a protection fault.
   - Hardware pushes error code 0x5 (`P=1, W=0, U=1`).
   - The page fault handler detects `(context->cs & 3) == 3`, logs the exact faulting CR2, and cleanly invokes `process_exit(cur, -11)`.
   - The kernel continues normal operation without panic, and the parent process receives SIGSEGV (-11).

5. **Premise 5: Test 4 Genuinely Traps Guard Page Overflow**:
   - Addresses `0x00007FFFFFFEDFF8` and `0x00007FFFFFFED000` are located within the 4 KiB guard page directly below the 64 KiB user stack.
   - The guard page is unmapped by `process_setup_user_stack()`.
   - Writes to these addresses cause the MMU to assert error code 0x6 (`P=0, W=1, U=1`: non-present user write).
   - The kernel terminates the faulting process with SIGSEGV (-11) and reaps all resources with 0 leaks.

6. **Premise 6: Test 5 Genuinely Measures Hardware Timer Preemption**:
   - The user payload executes a 30M-iteration CPU loop in Ring 3.
   - During loop execution, PIT timer interrupts (IRQ0, 100 Hz) interrupt CPL=3 code.
   - The CPU safely switches to the thread's kernel stack via TSS RSP0, increments the monotonic tick counter, and resumes user space.
   - Empirical run measured `elapsed_ticks = 16` (~160 ms), strictly validating preemption without stall or corruption.

7. **Premise 7: Test 6 Genuinely Proves 50-Iteration Zero Leakage**:
   - The test executes 50 full cycles of process creation, address space allocation, execution, syscall exit, and reaping.
   - Both PMM free frame count (51052) and active heap allocation count (3) matched before and after the 50 iterations with 0 delta.

---

## 3. Caveats

No caveats. All 6 acceptance tests run genuinely on hardware/QEMU, the build compiles cleanly with 0 warnings and 0 errors, and the system reaches the interactive prompt.

---

## 4. Conclusion

**Verdict**: **CLEAN**

Milestone M5 satisfies all forensic integrity criteria:
- **No hardcoded results or fake passes**: All 6 tests perform authentic runtime verification.
- **Genuine AMD64 hardware mechanics**: Tests trigger real MMU page faults (error codes 0x5 and 0x6), execute genuine Ring 3 code via `enter_userspace`, handle PIT timer interrupts across privilege boundaries via TSS RSP0, and execute `int 0x80` syscalls.
- **Zero memory leakage**: Cumulative PMM and heap comparisons prove complete resource reclamation across single executions and 50-iteration loops.
- **Clean build and live execution**: 0 compiler warnings/errors, authentic ELF symbol table, and successful live QEMU execution terminating at the interactive terminal prompt.

The work product is approved without reservations.

---

## 5. Verification Method

To independently reproduce this forensic audit:

1. **Build the Kernel**:
   ```pwsh
   .\tools\kos-tool.exe build
   ```
   *Expected*: Exit code 0, 0 warnings, 0 errors, 703 symbols embedded.

2. **Verify Symbols in Output Binary**:
   ```pwsh
   & "C:/Program Files/LLVM/bin/llvm-nm.exe" build/kos.elf | Select-String "command_usertest"
   ```
   *Expected*: Symbol `command_usertest` present at `ffffffff8000b880 t`.

3. **Execute in QEMU**:
   ```pwsh
   $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 15; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 100
   ```
   *Expected*: Verbatim output contains `[PASS] Test 1` through `[PASS] Test 6`, `>>> [PASS] ALL K14 USERSPACE & PROCESS TESTS PASSED! <<<`, and reaches `KOS debug terminal ready. KOS> `.
