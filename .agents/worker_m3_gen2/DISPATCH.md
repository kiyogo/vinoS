## 2026-09-18T03:02:50Z

You are Worker M3 (Gen 2) for Milestone K14 in KOS x86_64 OS.

Working directory: d:\DATA\source\OS\KOS\OS2\.agents\worker_m3_gen2
All your progress, logs, and handoff must be written to your working directory.

MANDATORY FIRST STEP:
Read the following authoritative specification files before taking any action:
1. `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` (specifically Requirement R2)
2. `d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_gen2\PROJECT.md`
3. `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_1\handoff.md`

WRITE OWNERSHIP:
You have exclusive write ownership of:
- `arch/x86_64/cpu.asm`
- `include/kos/cpu.h`
- `arch/x86_64/interrupts.c`
Do not modify files outside your ownership scope unless strictly required for header includes.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

TASK OBJECTIVE:
Implement Milestone M3: Ring 3 Privilege Transition & User Fault Containment (Requirement R2).

DETAILED REQUIREMENTS:
1. `arch/x86_64/cpu.asm`:
   - Implement global function `enter_userspace(uint64_t entry_point, uint64_t user_rsp)`.
   - In System V AMD64 ABI: entry_point is in `rdi`, user_rsp is in `rsi`.
   - Disable interrupts (`cli`) during transition frame setup.
   - Load segment registers DS, ES, FS, GS with user data selector `0x33` (`KOS_USER_DATA_SELECTOR`).
   - Push the 5-quadword `iretq` stack frame onto current stack:
     - SS = 0x33 (RPL=3)
     - RSP = `rsi` (user stack pointer)
     - RFLAGS = 0x202 (IF=1, bit 1 reserved 1)
     - CS = 0x2b (KOS_USER_CODE_SELECTOR, RPL=3)
     - RIP = `rdi` (user entry point)
   - Scrub all general-purpose registers (RAX, RBX, RCX, RDX, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15) to zero to prevent leaking kernel stack/data to userspace.
   - Execute `iretq`.

2. `include/kos/cpu.h`:
   - Declare: `void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn));`.

3. `arch/x86_64/interrupts.c`:
   - Include `<kos/process.h>` if not already present.
   - In `page_fault_handler(struct interrupt_context *context)`:
     Check if the fault occurred in user mode:
     `bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);`
     If `is_user` is true:
     - Log clear diagnostic warning with CR2 virtual address, RIP, and error code bits.
     - Look up current process: `struct process *cur = process_get_current();`
     - If `cur != NULL`, log process termination and call `process_exit(cur, -11);` (SIGSEGV).
     - If `process_exit()` does not return or if needed, ensure thread/scheduler yields via `schedule();`.
     - DO NOT call `cpu_halt()` or crash kernel when fault is from user space!
   - In `default_exception_panic(struct interrupt_context *context)`:
     Check if exception originated in user mode:
     `if ((context->cs & 3) == 3)`:
     - Log diagnostic warning: unhandled user exception vector, error code, RIP.
     - Call `struct process *cur = process_get_current(); if (cur) { process_exit(cur, -1); schedule(); }`.
     - DO NOT call `panic_with_context()` or crash kernel when exception is from user space!

4. BUILD & VERIFY:
   - Run `.\tools\kos-tool.exe build` to ensure 0 errors and 0 warnings.
   - Run `.\tools\kos-tool.exe run` (or headless timeout) to ensure existing boot and self-tests continue to pass cleanly without regressions.
   - Document verification commands and full outputs in your handoff.

5. REPORT:
   - Write your complete findings, diffs, and verification proof to `d:\DATA\source\OS\KOS\OS2\.agents\worker_m3_gen2\handoff.md`.
   - Send a message to caller with a summary of changes and verification status.
