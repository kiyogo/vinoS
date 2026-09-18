# Dispatch: Worker M3 (Ring 3 Transition & Fault Recovery)

## Mission
Implement Milestone M3 (Requirement R2: Ring 3 Privilege Transition & Hardware Context Management):
1. `arch/x86_64/cpu.asm`:
   - Implement `enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn));`:
     - Load data segment registers (DS, ES, FS, GS) with `0x33` (`GDT_USER_DS`).
     - Clear/scrub general-purpose registers (RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8..R15) to prevent kernel data leakage.
     - Push `iretq` frame:
       - SS: `0x33` (`GDT_USER_DS`)
       - RSP: `user_rsp`
       - RFLAGS: `0x202` (IF=1, bit 1=1)
       - CS: `0x2b` (`GDT_USER_CS`)
       - RIP: `entry_point`
     - Execute `iretq`.
2. `include/kos/cpu.h`:
   - Declare `void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn));`.
3. `arch/x86_64/interrupts.c`:
   - In `page_fault_handler()`:
     - Check `if ((context->error_code & PF_ERROR_USER) != 0 || (context->cs & 3) == 3)`:
     - User-mode page fault (Ring 3):
       - Do NOT call `cpu_halt()`.
       - Log diagnostic message.
       - Call `struct process *cur = process_get_current();` if present, then `process_exit(cur, -11);` and `schedule();`.
   - In `interrupt_dispatch()` / exception handling:
     - Check if exception occurred while `(context->cs & 3) == 3` (e.g. #GP vector 13, #UD vector 6).
     - Cleanly terminate the user process with `process_exit(cur, -1);` and `schedule();` instead of executing `default_exception_panic()` which halts the entire kernel.
4. Verify build:
   - Run `tools\kos-tool.exe build` and ensure exit code 0, 0 warnings, 0 errors.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Working Directory
`d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m3`
Write `changes.md` and `handoff.md` in your working directory and report when done.

## 2026-09-18T00:54:36Z
You are teamwork_preview_worker_m3.
Your working directory is: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m3
Read d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting work.
Read d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_1\PROJECT.md for overall architecture and contracts.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m3\DISPATCH.md for your specific mission.

Mission: Milestone M3 (Ring 3 Transition & Fault Recovery - Requirement R2)
Files you own exclusively:
- arch/x86_64/cpu.asm
- include/kos/cpu.h
- arch/x86_64/interrupts.c

Tasks:
1. In arch/x86_64/cpu.asm:
   - Implement enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn)):
     - First argument in RDI (entry_point), second argument in RSI (user_rsp).
     - Load segment registers (DS, ES, FS, GS) with 0x33 (GDT_USER_DS).
     - Push 5 quadwords for iretq:
       - SS: 0x33 (GDT_USER_DS)
       - RSP: RSI (user_rsp)
       - RFLAGS: 0x202 (IF=1, bit 1=1)
       - CS: 0x2b (GDT_USER_CS)
       - RIP: RDI (entry_point)
     - Zero all general purpose registers (RAX, RBX, RCX, RDX, RBP, RSI, RDI, R8..R15) to prevent kernel information leakage.
     - Execute iretq.
2. In include/kos/cpu.h:
   - Declare void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn));
3. In arch/x86_64/interrupts.c:
   - In page_fault_handler():
     - If the fault originated in user mode (error_code & PF_ERROR_USER or (context->cs & 3) == 3):
       - Log diagnostic message: faulting address (CR2), RIP, error code, process ID/name.
       - Cleanly terminate the user process: call process_exit(process_get_current(), -11); and schedule();
       - Do NOT call cpu_halt().
   - In default_exception_panic() / interrupt_dispatch():
     - If an exception occurs in user mode ((context->cs & 3) == 3, such as #GP vector 13, #UD vector 6):
       - Log warning with exception vector and register dump.
       - Cleanly terminate the user process with process_exit(process_get_current(), -1); and schedule();
       - Do NOT panic the entire kernel when a user process causes an exception.
4. Verify build:
   - Run tools\kos-tool.exe build. Ensure exit code 0, 0 warnings, 0 errors.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Write your changes to changes.md and your handoff report to handoff.md in your working directory. Send a message when complete.
