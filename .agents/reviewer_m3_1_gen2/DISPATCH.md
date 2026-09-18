## 2026-09-18T03:08:15Z
You are Reviewer M3-1 (Gen 2) for Milestone K14 in KOS x86_64 OS.

Working directory: d:\DATA\source\OS\KOS\OS2\.agents\reviewer_m3_1_gen2
All your notes and final handoff must be written to your working directory.

MANDATORY FIRST STEP:
Read the following authoritative files:
1. `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` (Requirement R2)
2. `d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_gen2\PROJECT.md`
3. `d:\DATA\source\OS\KOS\OS2\.agents\worker_m3_gen2\handoff.md`

TASK OBJECTIVE:
Objectively and critically review the Milestone M3 implementation:
- `arch/x86_64/cpu.asm`: `enter_userspace()` implementation. Check CLI protection, segment registers (DS, ES, FS, GS = 0x33), iretq frame format (SS=0x33, RSP=user_rsp, RFLAGS=0x202, CS=0x2b, RIP=entry_point), scrubbing of all 15 general-purpose registers (RAX..R15) to 0, and iretq instruction.
- `include/kos/cpu.h`: `enter_userspace` declaration with `KOS_NORETURN` / `__attribute__((noreturn))`.
- `arch/x86_64/interrupts.c`: User page fault containment in `page_fault_handler()` and unhandled user exception containment in `default_exception_panic()` / `handle_user_exception()`. Verify check for user mode `((context->cs & 3) == 3)` and `PF_ERROR_USER`, proper diagnostic logging, `process_exit()` call with SIGSEGV (-11) or exception code (-1), and yielding via `schedule()`. Verify that kernel never panics or halts when fault originates in Ring 3.
- Run `.\tools\kos-tool.exe build` to verify 0 errors and 0 warnings.
- Determine verdict: APPROVE or REQUEST_CHANGES.

Write your complete review report to `d:\DATA\source\OS\KOS\OS2\.agents\reviewer_m3_1_gen2\handoff.md` and send a message to caller with your verdict and summary.
