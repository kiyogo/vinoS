# Handoff Report: Milestone M5 — Empirical Challenge & Verification

**Agent ID**: `challenger_m5_1_gen2`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\challenger_m5_1_gen2`  
**Role**: Empirical Challenger (critic, specialist)  
**Parent Agent ID**: `c843668f-19e3-4d81-abc8-ea4359b7dde0` (`parent`)  
**Date**: 2026-09-18  
**Final Verdict**: **APPROVE**  

---

## 1. Observation

### 1.1 Authoritative Documents Reviewed
- `ORIGINAL_REQUEST.md`: Milestone K14 Acceptance Criteria:
  1. Valid ELF64 binaries load and execute in Ring 3 userspace successfully.
  2. Invalid/corrupted ELF binaries fail gracefully with error codes and do not panic the kernel.
  3. Memory protections are strictly enforced: Ring 3 cannot access kernel-only pages without triggering a page fault handled by the kernel.
  4. Hitting the user stack guard page cleanly faults and terminates the faulting process without crashing the kernel.
  5. TSS/RSP0 switching handles hardware interrupts (e.g., timer/PIT) and exceptions occurring in Ring 3 seamlessly, returning to userspace cleanly.
  6. Parent processes can fork/create children, retrieve child exit status via `process_wait`, and zombie processes are properly reaped upon cleanup.
  7. Repeated process creation and destruction loops demonstrate zero kernel heap or page frame leakage.
- `PROJECT.md`: Verified interface contracts (`vmm_create_address_space`, `vmm_destroy_address_space`, `enter_userspace`, `elf_load_binary`, `process_exec`, `process_wait`, `process_exit`).
- `worker_m5_gen2_1/handoff.md`: Implementation report of `command_usertest` and QEMU execution verification.

### 1.2 Independent Build Verification
Executed command:
```pwsh
.\tools\kos-tool.exe build
```
- Tool output:
  ```
  Kernel ELF built with 703 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  ```
- Exit code: `0`
- Compiler warnings: `0` (built under `-Wall -Wextra -Werror --fatal-warnings`)
- Link errors: `0`

### 1.3 Binary Symbol Verification
Audited `build/kos.elf` using `llvm-nm`:
```
ffffffff8000b880 t command_usertest
ffffffff80004810 T elf_load_binary
ffffffff80023f89 T enter_userspace
ffffffff800139b0 T process_exec
ffffffff800134d0 T process_exit
ffffffff80013990 t process_exit_condition
ffffffff80013820 T process_wait
ffffffff80030720 r test_corrupted_elf_rejection.valid_stub_code
ffffffff80030740 r test_kernel_mem_isolation.kmem_code_1
ffffffff80030760 r test_kernel_mem_isolation.kmem_code_2
ffffffff80030800 r test_leak_50_iterations.iter_code
ffffffff800307e0 r test_preemption_hardware.preempt_code
ffffffff80030780 r test_stack_guard_containment.guard_code_1
ffffffff800307b0 r test_stack_guard_containment.guard_code_2
ffffffff80030700 r test_valid_elf_execution.valid_code
ffffffff80001100 T tss_set_rsp0
ffffffff80023110 T vmm_create_address_space
ffffffff80023450 T vmm_destroy_address_space
```
All required symbols are physically present and properly linked.

### 1.4 Independent Headless QEMU Live Execution
Executed command:
```pwsh
if (Test-Path build/qemu_serial.log) { Remove-Item build/qemu_serial.log -Force }
$p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru
Start-Sleep -Seconds 15
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
```
Verbatim excerpts from the newly generated `build/qemu_serial.log`:
```
[    2.110] [TID 1] [INFO ] ==================================================
[    2.120] [TID 1] [INFO ] === Milestone K14 Ring 3 & Process Test Suite ===
[    2.130] [TID 1] [INFO ] ==================================================
[    2.130] [TID 1] [INFO ] [usertest] 1. Valid ELF64 binary execution...
[    2.150] [TID 1] [INFO ]   [PASS] Test 1: Valid ELF64 binary execution (exit_code=42, 0 leaks)
[    2.150] [TID 1] [INFO ] [usertest] 2. Corrupted / invalid ELF rejection...
[    2.160] [TID 1] [INFO ]   [PASS] 2a: Rejected invalid ELF magic bytes.
[    2.170] [TID 1] [INFO ]   [PASS] 2b: Rejected non-x86_64 target machine.
[    2.170] [TID 1] [INFO ]   [PASS] 2c: Rejected entry point outside loadable segment.
[    2.180] [TID 1] [INFO ]   [PASS] 2d: Rejected invalid PT_LOAD segment bounds.
[    2.190] [TID 1] [INFO ]   [PASS] 2e: Rejected truncated ELF file.
[    2.190] [TID 1] [INFO ]   [PASS] 2f: process_exec error cleanly returned -1 and unwound.
[    2.200] [TID 1] [INFO ]   [PASS] Test 2: Corrupted/invalid ELF binary rejection (6 subcases, 0 leaks)
[    2.210] [TID 1] [INFO ] [usertest] 3. Kernel memory isolation enforcement...
[    2.210] [TID 23] [WARN ] User process page fault: CR2=0xFFFFFFFF80000000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x5 [P=1 W=0 U=1 R=0 I=0] (page protection violation (write to read-only or supervisor page))
[    2.210] [TID 23] [WARN ] Terminating user process PID 11 (kmem1.elf) due to page fault (SIGSEGV -11)
[    2.220] [TID 1] [INFO ]   [PASS] 3a: Access to 0xFFFFFFFF80000000 cleanly trapped (SIGSEGV -11).
[    2.230] [TID 24] [WARN ] User process page fault: CR2=0xFFFF800000000000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x5 [P=1 W=0 U=1 R=0 I=0] (page protection violation (write to read-only or supervisor page))
[    2.230] [TID 24] [WARN ] Terminating user process PID 12 (kmem2.elf) due to page fault (SIGSEGV -11)
[    2.240] [TID 1] [INFO ]   [PASS] 3b: Access to 0xFFFF800000000000 cleanly trapped (SIGSEGV -11).
[    2.250] [TID 1] [INFO ]   [PASS] Test 3: Kernel memory isolation enforcement (2 higher-half addresses, SIGSEGV -11 trapped, 0 leaks)
[    2.260] [TID 1] [INFO ] [usertest] 4. User stack guard page containment...
[    2.260] [TID 25] [WARN ] User process page fault: CR2=0x00007FFFFFFEDFF8, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x6 [P=0 W=1 U=1 R=0 I=0] (non-present page (unmapped virtual address))
[    2.260] [TID 25] [WARN ] Terminating user process PID 13 (guard1.elf) due to page fault (SIGSEGV -11)
[    2.270] [TID 1] [INFO ]   [PASS] 4a: Guard page top access cleanly trapped (SIGSEGV -11).
[    2.280] [TID 26] [WARN ] User process page fault: CR2=0x00007FFFFFFED000, RIP=0x0000000000400082, RSP=0x00007FFFFFFFDFD0, error=0x6 [P=0 W=1 U=1 R=0 I=0] (non-present page (unmapped virtual address))
[    2.280] [TID 26] [WARN ] Terminating user process PID 14 (guard2.elf) due to page fault (SIGSEGV -11)
[    2.290] [TID 1] [INFO ]   [PASS] 4b: Guard page base access cleanly trapped (SIGSEGV -11).
[    2.300] [TID 1] [INFO ]   [PASS] Test 4: User stack guard page containment (2 guard boundaries trapped, 0 leaks)
[    2.300] [TID 1] [INFO ] [usertest] 5. TSS/RSP0 Ring 3 hardware preemption...
[    2.450] [TID 1] [INFO ]   Preemption test elapsed timer ticks: 14
[    2.460] [TID 1] [INFO ]   [PASS] Test 5: TSS/RSP0 Ring 3 hardware preemption (exit_code=55, elapsed_ticks=14, 0 leaks)
[    2.470] [TID 1] [INFO ] [usertest] 6. 50-iteration zero memory leak test...
[    2.520] [TID 1] [INFO ]   [PASS] Test 6: 50-iteration zero memory leak test (frames=51052, heap_allocs=3, 0 leaks)
[    2.520] [TID 1] [INFO ] --------------------------------------------------
[    2.530] [TID 1] [INFO ] >>> [PASS] ALL K14 USERSPACE & PROCESS TESTS PASSED! <<<
[    2.540] [TID 1] [INFO ] ==================================================
...
[    3.760] [TID 1] [INFO ] KOS debug terminal ready.
KOS> 
```

### 1.5 24-Point Automated Oracle Verification
Executed custom PowerShell test harness evaluating 24 regex patterns and numerical assertions against `build/qemu_serial.log`.
All 24 oracle checks returned `[VERIFIED] Match found`:
- Exit code for valid ELF: exactly `42`.
- Exit code for preemption loop: exactly `55`.
- Preemption ticks: `14 > 0`.
- All 6 corruption subcases rejected: `2a, 2b, 2c, 2d, 2e, 2f`.
- Kernel memory addresses `0xFFFFFFFF80000000` and `0xFFFF800000000000` triggered `#PF error=0x5` and SIGSEGV (-11).
- Stack guard boundary addresses `0x00007FFFFFFEDFF8` and `0x00007FFFFFFED000` triggered `#PF error=0x6` and SIGSEGV (-11).
- Memory leakage: 0 page frames leaked, 0 active heap allocations leaked after 50 iterations.
- Terminal prompt `KOS> ` reached.

---

## 2. Logic Chain

1. **Premise 1: Privilege Isolation and Ring 3 Transition (Observation 1.3, 1.4)**
   - Transition to userspace requires valid user code (`CS=0x2b`, RPL=3) and user data (`SS=0x33`, RPL=3) descriptors, with user stack and valid entry point.
   - `enter_userspace` in `arch/x86_64/cpu.asm` loads data segment registers with `0x33`, pushes `SS=0x33`, `RSP=user_rsp`, `RFLAGS=0x202` (IF=1), `CS=0x2b`, `RIP=entry_point`, clears all general-purpose registers (`RAX`..`R15`) to prevent kernel info leaks, and executes `iretq`.
   - Test 1 ran user-mode code that cleanly called `int 0x80` (DPL=3 gate), exited with code 42, and was waited upon and reaped by its parent process with zero memory leaks.

2. **Premise 2: Corrupted ELF Binary Safety & Memory Unwinding (Observation 1.4)**
   - The ELF parser in `fs/elf.c` must reject malformed binaries (corrupted magic, invalid machine type, out-of-bounds entry points, invalid segment sizes, truncated headers) before execution.
   - In addition, if `process_exec` fails midway due to a corrupted binary, it must release allocated address space structures and return -1 without kernel panic.
   - Test 2 demonstrated all 6 corruption subcases were rejected without panic, and initial and final frame/heap counts were identical.

3. **Premise 3: Paging Protection & User Fault Containment (Observation 1.4)**
   - When user-mode code attempts to read supervisor pages (such as `0xFFFFFFFF80000000` or `0xFFFF800000000000`), the AMD64 hardware MMU triggers Exception 14 (#PF) with error code `0x5` (`P=1, W=0, U=1`).
   - `interrupts.c:260-285` checks `(context->cs & 3) == 3 || (context->error_code & 4)`. Detecting a user fault, it terminates only the offending process (`process_exit(cur, -11)`) and invokes `schedule()`.
   - Test 3 confirmed both kernel addresses triggered user-mode page faults, terminated cleanly with SIGSEGV (-11), and left the kernel and parent running normally.

4. **Premise 4: Guard Page Boundary Detection (Observation 1.4)**
   - The user stack is 64 KiB ending at `0x00007FFFFFFEE000`, with an unmapped 4 KiB guard page at `0x00007FFFFFFED000`.
   - Accesses to the top (`0x00007FFFFFFEDFF8`) or base (`0x00007FFFFFFED000`) of the guard page raise #PF with error code `0x6` (`P=0, W=1, U=1`).
   - Test 4 confirmed both boundary accesses were caught as unmapped user page faults, terminating the faulting processes with SIGSEGV (-11) and zero leaks.

5. **Premise 5: Hardware Timer Preemption Across Privilege Levels (Observation 1.4)**
   - During user-mode compute execution, PIT hardware timer interrupts fire at 100 Hz.
   - The hardware transitions to the thread's kernel stack via TSS `RSP0`, executes the timer interrupt service routine, updates quantum slices, reschedules if needed, and returns to Ring 3 via `iretq`.
   - Test 5 confirmed 14 timer interrupts fired during the 30M-iteration loop, the process ran to completion and exited with code 55, and the child was cleanly reaped.

6. **Premise 6: 50-Iteration Zero Cumulative Leakage (Observation 1.4)**
   - The process lifecycle subsystem must reclaim every physical frame (PML4, PDPT, PD, PT, mapped user memory, user stack) and every heap structure (task structs, handle tables, list nodes).
   - Test 6 executed 50 complete process creation, exec, execution, wait, and destroy cycles. Free PMM frames remained exactly 51052 and active heap allocations remained exactly 3 before and after all 50 iterations (delta = 0).

---

## 3. Challenge Summary

**Overall risk assessment**: **LOW**

### Challenges Evaluated

#### Challenge 1: Privilege Boundary Escapes and Register Leakage
- **Assumption challenged**: Transitioning from Ring 0 to Ring 3 using `iretq` could leak sensitive kernel pointers in uninitialized GPRs or segment registers.
- **Investigation**: Inspected `arch/x86_64/cpu.asm:enter_userspace`. Lines 45-50 explicitly load `DS`, `ES`, `FS`, `GS` with `0x33`. Lines 64-78 explicitly zero `eax`, `ebx`, `ecx`, `edx`, `esi`, `edi`, `ebp`, `r8`..`r15` before `iretq`.
- **Verdict**: PASS. Complete register scrubbing is implemented.

#### Challenge 2: Kernel Panic on User Faults
- **Assumption challenged**: Page faults or invalid instruction exceptions originating in Ring 3 might invoke `default_exception_panic` and crash the OS.
- **Investigation**: Inspected `arch/x86_64/interrupts.c:260-285` and lines 401-417. Both page faults and general exceptions verify `(context->cs & 3) == 3`. When true, they log a warning, call `process_exit(cur, signal)`, and reschedule without invoking `panic_with_context()`.
- **Verdict**: PASS. Fault containment is complete.

#### Challenge 3: Incomplete Unwind on Corrupted Executables
- **Assumption challenged**: If an executable fails mid-load (e.g. valid header but bad segment bounds), memory mapped up to that point might leak.
- **Investigation**: Inspected `kernel/process.c:377-380` and `mm/vmm.c:vmm_destroy_address_space`. If `elf_load_binary` fails, `vmm_destroy_address_space(pml4_phys)` is called, which recursively frees all intermediate page tables and mapped frames. Test 2f specifically validates this path and verified 0 frame leaks.
- **Verdict**: PASS. Error rollback is leak-free.

#### Challenge 4: False Positive on Preemption Test
- **Assumption challenged**: A loop could finish before a timer tick occurs, causing preemption not to actually be exercised while still passing if ticks were unasserted.
- **Investigation**: Worker M5-1 Gen 2 updated Test 5 to explicitly check `elapsed_ticks > 0`. In empirical execution, 14 timer ticks fired while executing the user compute loop.
- **Verdict**: PASS. Real hardware interrupts serviced during user execution.

---

## 4. Stress Test Results

| Scenario | Expected Behavior | Actual Behavior | Pass / Fail |
|:---|:---|:---|:---:|
| Build with `-Werror --fatal-warnings` | Exit code 0, 0 warnings | Exit code 0, 0 warnings | **PASS** |
| Test 1: Valid ELF64 execution | Exit code 42, child reaped to DEAD, 0 leaks | Exit code 42, child DEAD, 0 leaks | **PASS** |
| Test 2a: Corrupted ELF magic | Rejected, 0 leaks | Rejected, 0 leaks | **PASS** |
| Test 2b: Non-x86_64 machine | Rejected, 0 leaks | Rejected, 0 leaks | **PASS** |
| Test 2c: Entry point outside segment | Rejected, 0 leaks | Rejected, 0 leaks | **PASS** |
| Test 2d: PT_LOAD memsz < filesz | Rejected, 0 leaks | Rejected, 0 leaks | **PASS** |
| Test 2e: Truncated ELF header | Rejected, 0 leaks | Rejected, 0 leaks | **PASS** |
| Test 2f: process_exec error rollback | Clean -1 return, page tables unwound, 0 leaks | Clean -1 return, 0 leaks | **PASS** |
| Test 3a: Ring 3 read kernel text | #PF (0x5) trapped, SIGSEGV -11, 0 leaks | #PF (0x5) trapped, exit -11, 0 leaks | **PASS** |
| Test 3b: Ring 3 read direct physical map | #PF (0x5) trapped, SIGSEGV -11, 0 leaks | #PF (0x5) trapped, exit -11, 0 leaks | **PASS** |
| Test 4a: Write guard page top | #PF (0x6) trapped, SIGSEGV -11, 0 leaks | #PF (0x6) trapped, exit -11, 0 leaks | **PASS** |
| Test 4b: Write guard page base | #PF (0x6) trapped, SIGSEGV -11, 0 leaks | #PF (0x6) trapped, exit -11, 0 leaks | **PASS** |
| Test 5: TSS/RSP0 Ring 3 preemption | 100Hz PIT ticks elapsed > 0, exit 55, 0 leaks | 14 ticks elapsed, exit 55, 0 leaks | **PASS** |
| Test 6: 50-iteration lifecycle stress | Baseline == Final frames & heap allocs | Baseline=51052/3, Final=51052/3 (0 delta) | **PASS** |
| Boot to interactive terminal | Reaches `KOS> ` prompt without hanging | Reaches `KOS debug terminal ready. KOS> ` | **PASS** |

---

## 5. Caveats

- **No Caveats**: All 6 acceptance tests were independently executed in live QEMU headless emulation, verified via automated oracle script against raw serial logs, and confirmed to satisfy 100% of Acceptance Criteria.

---

## 6. Conclusion

Milestone M5 is completely verified, robust, leak-free, and meets all requirements set forth in `ORIGINAL_REQUEST.md` and `PROJECT.md`.

**Final Challenger Verdict**: **APPROVE**

---

## 7. Verification Method

To independently reproduce the empirical verification:

### 7.1 Build Verification
```pwsh
.\tools\kos-tool.exe build
```
Verify exit code 0 and 0 compiler warnings/errors.

### 7.2 QEMU Live Execution
```pwsh
if (Test-Path build/qemu_serial.log) { Remove-Item build/qemu_serial.log -Force }
$p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru
Start-Sleep -Seconds 15
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Content build/qemu_serial.log | Select-String -Pattern "\[PASS\] Test [1-6]"
```
Verify all 6 tests output `[PASS]` and system reaches `KOS> `.

### 7.3 Invalidation Conditions
- Any test fails or encounters a kernel panic during user mode fault injection.
- PMM frame count or heap active allocations change across the 50-iteration leak test.
- Preemption test completes with 0 elapsed timer ticks.
