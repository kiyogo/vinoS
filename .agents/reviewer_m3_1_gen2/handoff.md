# Review & Adversarial Critic Handoff Report: Milestone M3 (Gen 2)

**Agent**: Reviewer M3-1 (Gen 2) (`reviewer_m3_1_gen2`)  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\reviewer_m3_1_gen2`  
**Review Target**: Milestone M3 — Ring 3 Privilege Transition & User Fault Containment  
**Authoritative Contracts**: Requirement R2 (`ORIGINAL_REQUEST.md`), `PROJECT.md`  
**Worker Report**: `d:\DATA\source\OS\KOS\OS2\.agents\worker_m3_gen2\handoff.md`  
**Date**: 2026-09-18  

---

## Executive Summary

- **Verdict**: **APPROVE**
- **Integrity Assessment**: **PASS** (Zero integrity violations; genuine implementation, no dummy facades, no hardcoded cheating, no bypassed work, independent verification clean).
- **Build Status**: **PASS** (`tools\kos-tool.exe build` exited with code 0; 0 warnings, 0 errors; 665 embedded symbols).
- **Runtime Smoke Test**: **PASS** (QEMU boots completely to shell; VMM, PMM, ELF loader, IPC, and network tests pass with 0 leaks).

---

## 1. Observation

### 1.1 `enter_userspace()` in `arch/x86_64/cpu.asm`
- **File location**: `arch/x86_64/cpu.asm:40-81`
- **Direct code quote**:
  ```nasm
  ; void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn))
  ; Arguments:
  ;   rdi: entry_point (virtual RIP to execute in user mode)
  ;   rsi: user_rsp    (virtual RSP user stack pointer)
  enter_userspace:
      ; Disable interrupts while preparing the iretq stack frame
      cli

      ; Load data segment registers (DS, ES, FS, GS) with 0x33 (GDT_USER_DS)
      mov ax, 0x33
      mov ds, ax
      mov es, ax
      mov fs, ax
      mov gs, ax

      ; Push 5 quadwords for iretq:
      ;   [rsp + 32] SS:     0x33 (GDT_USER_DS)
      ;   [rsp + 24] RSP:    rsi  (user_rsp)
      ;   [rsp + 16] RFLAGS: 0x202 (IF=1, bit 1=1)
      ;   [rsp + 8]  CS:     0x2b (GDT_USER_CS)
      ;   [rsp + 0]  RIP:    rdi  (entry_point)
      push qword 0x33
      push rsi
      push qword 0x202
      push qword 0x2b
      push rdi

      ; Zero all general-purpose registers to prevent kernel information leakage
      xor eax, eax
      xor ebx, ebx
      xor ecx, ecx
      xor edx, edx
      xor esi, esi
      xor edi, edi
      xor ebp, ebp
      xor r8, r8
      xor r9, r9
      xor r10, r10
      xor r11, r11
      xor r12, r12
      xor r13, r13
      xor r14, r14
      xor r15, r15

      iretq
  ```
- **Observations**:
  1. `cli` disables interrupts before touching segment registers or pushing the frame.
  2. Segment registers `DS`, `ES`, `FS`, `GS` are loaded with `0x33`.
  3. GDT configuration (`arch/x86_64/descriptors.c:23-24`) confirms GDT entry 5 is User Code (`0x00affa000000ffffull`), selector `(5 << 3) | 3 = 0x2b`, and entry 6 is User Data (`0x00cff2000000ffffull`), selector `(6 << 3) | 3 = 0x33`.
  4. The 5 pushed quadwords strictly follow the AMD64 `iretq` stack frame:
     - `[rsp + 32]` = `0x33` (`SS`, RPL=3)
     - `[rsp + 24]` = `rsi` (`RSP` = `user_rsp`)
     - `[rsp + 16]` = `0x202` (`RFLAGS` with IF=1, reserved bit 1=1)
     - `[rsp + 8]`  = `0x2b` (`CS`, RPL=3)
     - `[rsp + 0]`  = `rdi` (`RIP` = `entry_point`)
  5. All 15 general-purpose registers (`RAX`, `RBX`, `RCX`, `RDX`, `RSI`, `RDI`, `RBP`, `R8`, `R9`, `R10`, `R11`, `R12`, `R13`, `R14`, `R15`) are zeroed using 32-bit `xor` (which x86_64 automatically zero-extends to 64 bits) or 64-bit `xor` (`r8`-`r15`).
  6. Atomic switch to Ring 3 via `iretq`.

### 1.2 Function Declaration in `include/kos/cpu.h`
- **File location**: `include/kos/cpu.h:24`
- **Direct code quote**:
  ```c
  KOS_NORETURN void enter_userspace(uint64_t entry_point, uint64_t user_rsp);
  ```
- **Observations**:
  - `KOS_NORETURN` is defined in `include/kos/compiler.h:6` as `__attribute__((noreturn))`.
  - Prototype matches calling conventions and types (`uint64_t entry_point, uint64_t user_rsp`).

### 1.3 User Page Fault Containment in `arch/x86_64/interrupts.c`
- **File location**: `arch/x86_64/interrupts.c:256-285`
- **Direct code quote**:
  ```c
      /* 4. Fault discrimination: Kernel mode vs User mode */
      bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);
      if (!is_user) {
          /* Kernel mode page fault is unrecoverable -> Panic & Halt */
          log_page_fault_panic(context, &record);
      } else {
          /* User mode page fault (Ring 3) */
          __atomic_sub_fetch(&s_pf_recursion, 1, __ATOMIC_SEQ_CST);

          log_warnf("User process page fault: CR2=0x%016lX, RIP=0x%016lX, RSP=0x%016lX, error=0x%lX [P=%u W=%u U=%u R=%u I=%u] (%s)",
                    (unsigned long)cr2,
                    (unsigned long)context->rip,
                    (unsigned long)context->rsp,
                    (unsigned long)context->error_code,
                    (unsigned int)(record.present ? 1 : 0),
                    (unsigned int)(record.write ? 1 : 0),
                    (unsigned int)(record.user ? 1 : 0),
                    (unsigned int)(record.reserved_bit ? 1 : 0),
                    (unsigned int)(record.instruction_fetch ? 1 : 0),
                    record.violation_cause ? record.violation_cause : "access violation");

          struct process *cur = process_get_current();
          if (cur != NULL) {
              log_warnf("Terminating user process PID %lu (%s) due to page fault (SIGSEGV -11)",
                        (unsigned long)cur->pid, cur->name);
              process_exit(cur, -11);
              schedule();
          }
          schedule();
      }
  ```
- **Observations**:
  - User mode is validated via `((context->cs & 3) == 3)` and `((context->error_code & 4) != 0)` (`PF_ERROR_USER`).
  - Kernel mode faults immediately panic and halt via `log_page_fault_panic()` -> `panic_with_context()`.
  - For user mode faults: `s_pf_recursion` is decremented atomically back to 0, diagnostic details (CR2, RIP, RSP, decoded error bits) are logged, `process_exit(cur, -11)` (SIGSEGV) is invoked to transition process to `PROCESS_STATE_ZOMBIE`, destroy handles, notify waiters, and call `thread_exit()`, followed by `schedule()`.
  - The kernel never panics, halts, or dies from a user page fault.

### 1.4 User Exception Containment in `arch/x86_64/interrupts.c`
- **File location**: `arch/x86_64/interrupts.c:356-425` and `467-470`
- **Direct code quote**:
  ```c
  static void handle_user_exception(const struct interrupt_context *context) {
      log_warnf("Unhandled user exception: vector=0x%02lX (%s), error_code=0x%lX, RIP=0x%016lX",
                (unsigned long)context->vector,
                exception_name(context->vector),
                (unsigned long)context->error_code,
                (unsigned long)context->rip);
      struct process *cur = process_get_current();
      log_warnf("User process exception details: PID %lu (%s)",
                (unsigned long)(cur ? cur->pid : 0),
                cur ? cur->name : "unknown");
      ...
      if (cur != NULL) {
          log_warnf("Terminating user process PID %lu (%s) due to unhandled exception",
                    (unsigned long)cur->pid, cur->name);
          process_exit(cur, -1);
          schedule();
      }
      schedule();
  }

  static void default_exception_panic(const struct interrupt_context *context) {
      if ((context->cs & 3) == 3) {
          log_warnf("Unhandled user exception: vector=0x%02lX (%s), error_code=0x%lX, RIP=0x%016lX",
                    (unsigned long)context->vector,
                    exception_name(context->vector),
                    (unsigned long)context->error_code,
                    (unsigned long)context->rip);
          struct process *cur = process_get_current();
          if (cur != NULL) {
              log_warnf("Terminating user process PID %lu (%s) due to unhandled exception",
                        (unsigned long)cur->pid, cur->name);
              process_exit(cur, -1);
              schedule();
          }
          schedule();
          return;
      }
      ...
      panic_with_context(context, "Unhandled CPU Exception: %s (Vector 0x%02lX)",
                         exception_name(context->vector), (unsigned long)context->vector);
  }
  ```
- **Observations**:
  - In `interrupt_dispatch`: vectors 0-31 with no registered handler check `((context->cs & 3) == 3)` and divert to `handle_user_exception()`.
  - `default_exception_panic()` also contains a safety check for `((context->cs & 3) == 3)` before calling `panic_with_context()`.
  - Cleanly logs register dump, calls `process_exit(cur, -1)` and `schedule()`.

### 1.5 Build and Smoke Test Execution
- **Command**: `.\tools\kos-tool.exe build`
  - Exit code: `0`
  - Output: `Kernel ELF built with 665 embedded symbols: build/kos.elf`
  - `UEFI ESP staged: build/esp`
  - 0 warnings, 0 errors.
- **QEMU Smoke Test**: System booted cleanly through all tests:
  - `[PASS] VMM address space lifecycle self-test passed (0 frame leaks).`
  - `[PASS] ELF loader & user stack self-test passed (0 frame leaks).`
  - `[PASS] Kernel heap self-test and page reclamation passed.`
  - `[PASS] ALL PROCESS & THREAD LIFECYCLE TESTS PASSED!`
  - `[PASS] ALL KERNEL OBJECT & HANDLE TESTS PASSED!`
  - Network and TCP/HTTP tests fully operational.

---

## 2. Logic Chain

1. **RPL and GDT Alignment**:
   - `KOS_USER_CODE_SELECTOR = 0x2b` (GDT index 5, RPL 3).
   - `KOS_USER_DATA_SELECTOR = 0x33` (GDT index 6, RPL 3).
   - `cpu.asm` loads `0x33` into DS, ES, FS, GS and pushes `0x33` for SS and `0x2b` for CS.
   - When `iretq` executes, hardware atomically transitions CPL to 3 with matching code and data descriptors.
2. **Interrupt & Context Hygiene**:
   - Disabling interrupts (`cli`) prevents asynchronous interrupts or context switches from interfering with stack frame setup.
   - Pushing `0x202` (IF=1) ensures interrupts are re-enabled as soon as `iretq` enters user mode.
   - Zeroing RAX..R15 clears all residual kernel register state, eliminating a critical information disclosure vector (kernel pointer/data leakage).
3. **Fault Containment & Kernel Resilience**:
   - AMD64 privilege level is reflected in `context->cs & 3` and `#PF` bit 2 (`PF_ERROR_USER`).
   - By verifying both conditions, any access violation, guard-page hit, or unhandled exception originating from CPL=3 is isolated to user space.
   - Terminating the offending process via `process_exit(cur, -11)` (or `-1`) destroys its handle table, wakes waiting parents (`exit_wq`), and delegates cleanup to the kernel reaper thread.
   - Calling `schedule()` smoothly transitions the CPU to the next runnable thread or `idle_task`, preventing kernel halts or panics.

---

## 3. Caveats

1. **Signal Handlers in Userspace**:
   - At Milestone M3, user-space page faults and exceptions directly terminate the process with standard exit codes (`-11` for SIGSEGV, `-1` for exceptions). User-level signal handlers (`sigaction`, signal stack frames) are reserved for subsequent milestones.
2. **SMP Extension**:
   - Current GDT and TSS configurations are uniprocessor. If SMP is enabled in future milestones, `tss_set_rsp0` will need per-CPU TSS management.

---

## 4. Adversarial Review & Stress Testing

### 4.1 Challenge Matrix
| # | Dimension | Attack Scenario / Hypothesis | Stress Test Result | Status |
|---|-----------|-----------------------------|--------------------|--------|
| 1 | Register Scrubbing | Does `enter_userspace` leave any GPR unscrubbed? | Verified: RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8..R15 all zeroed. User RSP restored from `rsi`. | PASS |
| 2 | IRETQ Stack Frame | Could stack frame ordering cause GPF on `iretq`? | Verified: Stack layout matches AMD64 ABI: `[rsp]=RIP`, `[rsp+8]=CS`, `[rsp+16]=RFLAGS`, `[rsp+24]=RSP`, `[rsp+32]=SS`. | PASS |
| 3 | Selector Verification | Are `0x2b` and `0x33` valid user selectors? | Verified against `descriptors.c:23-24`: Entry 5 is 64-bit code DPL 3 (`0x28 | 3`), Entry 6 is data DPL 3 (`0x30 | 3`). | PASS |
| 4 | Recursive Fault Handling | Can user page fault corrupt `s_pf_recursion`? | Verified: `s_pf_recursion` is decremented atomically (`__atomic_sub_fetch`) on user fault branch, preventing false recursive panic. | PASS |
| 5 | Kernel Panic Immunity | Can user code deliberately trigger `#UD` or `#GP` to panic the OS? | Verified: `interrupt_dispatch` and `default_exception_panic` both check `(context->cs & 3) == 3` and route to `process_exit()`. | PASS |
| 6 | Integrity Check | Were any fake/hardcoded results or facades introduced? | Verified: Real implementation across all files; build and QEMU tests independently run. | PASS |

---

## 5. Findings & Quality Review

### Verified Claims
- `enter_userspace` sets DS/ES/FS/GS to 0x33: **VERIFIED** (`arch/x86_64/cpu.asm:45-49`).
- `enter_userspace` pushes SS=0x33, RSP=user_rsp, RFLAGS=0x202, CS=0x2b, RIP=entry_point: **VERIFIED** (`arch/x86_64/cpu.asm:57-61`).
- `enter_userspace` scrubs all 15 general-purpose registers: **VERIFIED** (`arch/x86_64/cpu.asm:64-78`).
- `enter_userspace` executes `iretq`: **VERIFIED** (`arch/x86_64/cpu.asm:80`).
- `enter_userspace` prototype declared with `KOS_NORETURN`: **VERIFIED** (`include/kos/cpu.h:24`).
- `page_fault_handler()` catches user-mode faults and calls `process_exit(cur, -11)` + `schedule()`: **VERIFIED** (`arch/x86_64/interrupts.c:256-285`).
- `handle_user_exception()` / `default_exception_panic()` catches unhandled Ring 3 exceptions and calls `process_exit(cur, -1)` + `schedule()`: **VERIFIED** (`arch/x86_64/interrupts.c:356-425, 467-470`).
- Build succeeds with 0 errors and 0 warnings: **VERIFIED** (`.\tools\kos-tool.exe build`).
- Self tests and QEMU smoke tests pass: **VERIFIED** (Live QEMU execution).

### Findings
- None (No Critical, Major, or Minor blockers).

---

## 6. Conclusion

Milestone M3 (Requirement R2) is implemented completely, robustly, and safely:
- The privilege transition mechanism `enter_userspace` correctly manages GDT selectors, stack formatting, register sanitation, and CPU flags for Ring 3 entry.
- Fault containment preserves kernel integrity and smoothly terminates faulting user processes via `process_exit()` without panicking or halting the OS.
- Toolchain build compiles cleanly with zero warnings and zero errors.
- **Verdict**: **APPROVE**

---

## 7. Verification Method

### 1. Build Verification
Run the build command from project root:
```pwsh
.\tools\kos-tool.exe build
```
**Expected Output**:
- Exit code: 0
- 0 compiler warnings, 0 compiler errors.
- `Kernel ELF built with 665 embedded symbols: build/kos.elf`
- `UEFI ESP staged: build/esp`

### 2. Static Code Inspection
- Inspect `arch/x86_64/cpu.asm:40-81` to verify `cli`, selectors `0x33`/`0x2b`, RFLAGS `0x202`, GPR scrubbing, and `iretq`.
- Inspect `include/kos/cpu.h:24` to verify `KOS_NORETURN void enter_userspace(uint64_t entry_point, uint64_t user_rsp);`.
- Inspect `arch/x86_64/interrupts.c:256-285, 356-425, 467-470` to verify `(context->cs & 3) == 3` checks and `process_exit()` invocations.

### 3. QEMU Runtime Smoke Test
Run:
```pwsh
$p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -netdev user,id=kosnet,ipv4=on,ipv6=off -device e1000,netdev=kosnet,mac=52:54:00:12:34:56 -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 40
```
**Expected Output**:
- System boots cleanly.
- All self-tests pass (`VMM address space lifecycle self-test passed`, `ELF loader & user stack self-test passed`).
- Terminal ready for input.
