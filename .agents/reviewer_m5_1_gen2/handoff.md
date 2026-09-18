# Milestone M5 Review & Adversarial Audit Report (Gen 2)

**Reviewer**: Reviewer M5-1 (Gen 2)  
**Roles**: Reviewer, Adversarial Critic  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\reviewer_m5_1_gen2`  
**Target Milestone**: K14 (Process and Ring 3 Userspace — Milestone M5 / Acceptance Criteria Verification)  
**Authoritative References**:  
1. `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`  
2. `d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_gen2\PROJECT.md`  
3. `d:\DATA\source\OS\KOS\OS2\.agents\worker_m5_gen2_1\handoff.md`  
**Date**: 2026-09-18  

---

## Review Summary

**Verdict**: **APPROVE**  
**Integrity Status**: **PASS** (Zero integrity violations; genuine Ring 3 execution, real hardware MMU exception traps, genuine TSS RSP0 kernel stack transitions, genuine scheduler preemption under load, and zero heap/frame memory leakage across 50 execution cycles).

---

## 1. Observation

### 1.1 Command Registration & Argument Parsing (`kernel/command.c`)
- **Help Command Listing** (`kernel/command.c:50`):
  `usertest` is explicitly listed in `command_help`:
  ```c
  log_info("help clear meminfo uptime sysinfo netinfo ifconfig arp arping ping dhcp udptest tcptest nslookup curl httpd netstat echo reboot irqinfo kbdinfo drivers lspci taskinfo tasktest taskdemo vol ls cat elfinfo handletest waittest synctest usertest atomictest locktest mutextest timertest sleeptest preempttest processtest stacktest pftest panictest kstress taskstress");
  ```
- **Command Registry** (`kernel/command.c:3810`):
  ```c
  { "usertest", command_usertest },
  ```
- **Argument Parsing Helpers** (`kernel/command.c:2104-2135`):
  - `usertest_is_empty(const char *arg)` correctly handles `NULL` and skips leading/trailing whitespace (`' '`, `\t`, `\r`, `\n`).
  - `usertest_match_arg(const char *arg, const char *target)` skips leading whitespace, matches target characters, and confirms remaining characters are whitespace only.
  - Subcommands supported: `all`, `valid`, `corrupted`, `kernel_mem`, `stack_overflow`, `preemption`, `leak`.
  - Unrecognized commands trigger usage:
    `log_error("Usage: usertest [all|valid|corrupted|kernel_mem|stack_overflow|preemption|leak]");`
- **Boot Self-Test Hook** (`kernel/command.c:1327`):
  `command_synctest` invokes `command_usertest("all");`, guaranteeing automated regression execution on boot.

### 1.2 Verification of the 6 Acceptance Tests (`kernel/command.c`)

1. **Test 1 (`valid`) — Valid ELF64 binary execution in Ring 3 (`kernel/command.c:2196-2278`)**:
   - Constructs a genuine ELF64 executable image in memory via `create_test_elf` with `e_type = ET_EXEC`, `e_machine = EM_X86_64`, `e_entry = 0x00400078`, and `PT_LOAD` at `0x00400000`.
   - Payload machine code:
     ```assembly
     mov rax, 4   ; KOS_SYSCALL_EXIT
     mov rdi, 42  ; exit status 42
     int 0x80     ; dispatch to process_exit via IDT DPL=3 syscall gate
     jmp $
     ```
   - Writes `D:/valid.elf` to RAMFS, spawns `valid_proc` child process with `valid_t` thread executing `process_exec`.
   - `process_exec` builds private address space, maps PT_LOAD segment, sets up System V AMD64 ABI user stack at `USER_STACK_TOP`, sets `tss_set_rsp0`, writes CR3, and enters Ring 3 via `enter_userspace` (`iretq`).
   - Parent waits via `process_wait()`, receives exit code 42, reaps child to `PROCESS_STATE_DEAD`.
   - Asserts `initial_frames == final_frames` and `initial_heap == final_heap` (0 leaked frames, 0 leaked heap allocations).

2. **Test 2 (`corrupted`) — Corrupted / Invalid ELF Rejection (`kernel/command.c:2283-2459`)**:
   - Subcase 2a (`kernel/command.c:2304-2322`): Corrupts magic bytes to `'X','Y','Z'` -> `elf_load_binary` returns `false`.
   - Subcase 2b (`kernel/command.c:2324-2345`): Sets `e_machine = EM_386` -> `elf_load_binary` returns `false`.
   - Subcase 2c (`kernel/command.c:2347-2366`): Sets `e_entry = 0x00100000` (outside loadable segments) -> `elf_load_binary` returns `false`.
   - Subcase 2d (`kernel/command.c:2368-2389`): Sets `p_memsz = p_filesz - 1` -> `elf_load_binary` returns `false`.
   - Subcase 2e (`kernel/command.c:2391-2408`): Truncates header to 32 bytes (< 64 bytes) -> `elf_load_binary` returns `false`.
   - Subcase 2f (`kernel/command.c:2410-2439`): Spawns child process running `process_exec` on bad ELF -> `process_exec` returns -1, unwinds allocated PML4 page tables cleanly, parent retrieves exit status -1.
   - Verifies 0 leaked frames and 0 leaked heap allocations across all 6 subcases.

3. **Test 3 (`kernel_mem`) — Kernel Memory Isolation Enforcement (`kernel/command.c:2464-2586`)**:
   - Subcase 3a: Ring 3 executes `movabs rax, 0xFFFFFFFF80000000; mov rbx, [rax]`.
   - Subcase 3b: Ring 3 executes `movabs rax, 0xFFFF800000000000; mov rbx, [rax]`.
   - CPU MMU triggers hardware #PF (Vector 14) with error code `0x5` (`P=1, W=0, U=1`).
   - `page_fault_handler()` in `arch/x86_64/interrupts.c:256-284` detects user mode fault `(context->cs & 3) == 3`, logs the violation, calls `process_exit(cur, -11 /* SIGSEGV */)`, and reschedules.
   - Scheduler continues running; parent unblocks with exit code -11; child reaped with 0 leaks.

4. **Test 4 (`stack_overflow`) — User Stack Guard Page Containment (`kernel/command.c:2591-2713`)**:
   - Subcase 4a: Ring 3 writes to top of guard page: `0x00007FFFFFFEDFF8`.
   - Subcase 4b: Ring 3 writes to base of guard page: `0x00007FFFFFFED000`.
   - CPU MMU triggers #PF with error code `0x6` (`P=0, W=1, U=1`).
   - `page_fault_handler()` intercepts non-present user write, terminates child with SIGSEGV (-11).
   - Parent receives -11; child reaped to `DEAD` with 0 leaks; kernel remains stable.

5. **Test 5 (`preemption`) — TSS/RSP0 Ring 3 Hardware Preemption (`kernel/command.c:2718-2817`)**:
   - Ring 3 executes a 30,000,000-iteration compute loop (`dec rcx; jnz`).
   - PIT timer interrupts (100 Hz) fire while CPL=3.
   - CPU automatically loads RSP from `TSS.rsp0` on each interrupt.
   - Monotonic timer tick counter advances; test captures `elapsed_ticks` (measured 14 ticks in QEMU run) and explicitly asserts `elapsed_ticks > 0`.
   - Child completes loop, calls `int 0x80` with RAX=4 and RDI=55; parent reaps exit code 55 with 0 leaks.

6. **Test 6 (`leak`) — 50-Iteration Zero Memory Leak Test (`kernel/command.c:2822-2898`)**:
   - Loops 50 times through full process lifecycle: `process_create()`, `thread_create()`, `process_exec()`, Ring 3 entry, `int 0x80` exit with code 42, `process_wait()`, and `process_destroy()`.
   - Records baseline PMM frames (51052) and active heap allocations (3).
   - After 50 iterations: final frames = 51052 (delta = 0), final heap = 3 (delta = 0).

### 1.3 Automated Build Tool Execution
- Command: `.\tools\kos-tool.exe build`
- Output:
  ```
  Kernel ELF built with 703 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  ```
- Result: **0 errors, 0 warnings** (compiled under strict `-Wall -Wextra -Werror` and `--fatal-warnings`).

### 1.4 Live QEMU Execution (`build/qemu_serial.log`)
Direct serial log excerpt from boot test run:
```
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
[    3.710] [TID 1] [INFO ] KOS debug terminal ready.
KOS>
```

---

## 2. Logic Chain

1. **Premise 1: Genuine Hardware Privilege Switching & Syscall Execution**:
   - Supporting Evidence: `arch/x86_64/cpu.asm:40-81` implements `enter_userspace` using `iretq` with user data selectors (`0x33`), user code selector (`0x2b`), and RFLAGS `0x202`. In Test 1, the user code executes `int 0x80` with RAX=4 and RDI=42.
   - Result: Vector 0x80 handler switches stack using `TSS.rsp0`, executes `kos_syscall_dispatch()`, transitions process to `ZOMBIE`, and parent in `process_wait()` retrieves exit code 42. Child reaped to `DEAD` with 0 memory leaks.
2. **Premise 2: Resilient Malformed ELF Defense & Rollback**:
   - Supporting Evidence: `fs/elf.c:202-298` verifies magic, 64-bit, little-endian, EV_CURRENT, ET_EXEC, EM_X86_64, canonical user entry point >= 2 MiB, non-overlapping loadable segments, and executable entry segment.
   - Result: In Test 2, all 5 corrupt header variants return `false`, and Test 2f demonstrates that `process_exec()` failure triggers clean rollback via `vmm_destroy_address_space()`, unwinding child state to exit code -1 without leaking frames or heap memory.
3. **Premise 3: Paging Isolation & Fault Containment**:
   - Supporting Evidence: `arch/x86_64/interrupts.c:256-284` intercepts page faults where `(context->cs & 3) == 3` or `error_code & 4 != 0`. Rather than halting or panicking, it logs CR2 and issues `process_exit(cur, -11)`.
   - Result: In Test 3, Ring 3 accesses to `0xFFFFFFFF80000000` and `0xFFFF800000000000` trigger hardware error `0x5` (supervisor protection fault). The kernel contains the fault, terminates the child with SIGSEGV (-11), and continues executing without interruption.
4. **Premise 4: Guard Page Boundary Detection**:
   - Supporting Evidence: `fs/elf.c:80-84` ensures `USER_STACK_GUARD_PAGE` (`0x00007FFFFFFED000`) is unmapped.
   - Result: In Test 4, writes to both the top (`0x00007FFFFFFEDFF8`) and base (`0x00007FFFFFFED000`) of the guard page trigger hardware error `0x6` (non-present write). The fault is cleanly trapped, the process terminates with -11, and the parent reaps the child with 0 leaks.
5. **Premise 5: TSS/RSP0 Hardware Preemption under Workload**:
   - Supporting Evidence: `kernel/task.c:307-312` calls `tss_set_rsp0()` on every thread context switch. During hardware interrupts, CPU loads `TSS.rsp0`.
   - Result: In Test 5, a 30M-iteration CPU loop in Ring 3 took 14 timer interrupts. The CPU switched to kernel stack on each tick, updated monotonic time and task quantum, returned to Ring 3 via `iretq`, and completed with exit code 55.
6. **Premise 6: Sustained Zero Memory Leakage**:
   - Supporting Evidence: In Test 6, 50 full process lifecycle loops were executed consecutively.
   - Result: Both initial and final free frame counts were 51052 (delta = 0). Both initial and final heap allocation counts were 3 (delta = 0).

---

## 3. Adversarial Challenges & Stress Testing

### 3.1 Challenge: Hardcoded or Facade Implementations (Integrity Audit)
- **Attack Scenario**: Did the test author hardcode exit codes, bypass the ELF loader, or mock system calls?
- **Finding**: **PASSED**. The test ELF binaries are dynamically generated with actual machine code instructions (`0xCD, 0x80`, `0x48, 0xB8...`, `0x48, 0x8B, 0x18`). Physical pages are allocated via PMM, mapped via VMM, loaded into CR3, and executed via real hardware transitions (`iretq` / `int 0x80` / `#PF`). The test logic queries live hardware counters (`timer_get_ticks()`, `pmm_free_frame_count()`, `heap_active_allocation_count()`).

### 3.2 Challenge: Argument Parsing Robustness & Injection
- **Attack Scenario**: Can malformed or whitespace-padded arguments crash or bypass `command_usertest`?
- **Finding**: **PASSED**. `usertest_is_empty` and `usertest_match_arg` strictly parse tokens, ignore leading and trailing whitespace (`' '`, `\t`, `\r`, `\n`), and safely reject any invalid string by displaying the usage string without crashing.

### 3.3 Challenge: Preemption Liveness Verification
- **Attack Scenario**: Could the preemption test pass if timer interrupts were disabled or not firing?
- **Finding**: **PASSED**. `test_preemption_hardware` explicitly calculates `elapsed_ticks = ticks_after - ticks_before` and fails with `[FAIL] Preemption test finished without any timer ticks elapsed!` if `elapsed_ticks == 0`. Live execution recorded 14 ticks.

---

## 4. Caveats

- **No Caveats**: All 6 tests execute genuinely on hardware/QEMU, achieve 100% pass rate, produce 0 memory leaks across 50 iterations, and compile cleanly with 0 warnings and 0 errors.

---

## 5. Conclusion

The Milestone M5 implementation in `kernel/command.c` is exemplary, thoroughly tested, and completely satisfies all 7 Acceptance Criteria of Milestone K14.
**Final Verdict: APPROVE**.

---

## 6. Verification Method

### 6.1 Build Verification
```pwsh
.\tools\kos-tool.exe build
```
*Expected Result*: Exit code 0, 0 warnings, 0 errors.

### 6.2 QEMU Live Execution
```pwsh
$p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 12; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 100
```
*Expected Result*: All 6 tests print `[PASS]`, followed by `>>> [PASS] ALL K14 USERSPACE & PROCESS TESTS PASSED! <<<`, and the OS boots to `KOS debug terminal ready. KOS> `.
