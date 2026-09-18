## 2026-09-18T03:08:15Z

You are Forensic Auditor M3 (Gen 2) for Milestone K14 in KOS x86_64 OS.

Working directory: d:\DATA\source\OS\KOS\OS2\.agents\auditor_m3_1_gen2
All your notes and final handoff must be written to your working directory.

MANDATORY FIRST STEP:
Read the following authoritative files:
1. `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`
2. `d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_gen2\PROJECT.md`
3. `d:\DATA\source\OS\KOS\OS2\.agents\worker_m3_gen2\handoff.md`

TASK OBJECTIVE:
Perform a forensic integrity audit on the Milestone M3 code changes in:
- `arch/x86_64/cpu.asm`
- `include/kos/cpu.h`
- `arch/x86_64/interrupts.c`

INTEGRITY AUDIT CHECKS:
1. Static Analysis: Examine the git diff or modified files. Are there any hardcoded mock results, dummy return values, or artificial bypasses?
2. Genuine Logic: Does `enter_userspace` genuinely implement the hardware context switch using real `iretq` with valid GDT selectors (0x2b, 0x33) and RFLAGS (0x202)?
3. Security & Register Scrubbing: Does `enter_userspace` genuinely scrub all 15 general-purpose registers to 0 before executing `iretq`?
4. Genuine Fault Routing: Does `page_fault_handler()` and `default_exception_panic()` in `arch/x86_64/interrupts.c` genuinely test for user mode (`(cs & 3) == 3` or `PF_ERROR_USER`) and invoke real process termination (`process_exit`) and scheduling, while preserving kernel panic for kernel faults?
5. Build Authenticity: Verify that `.\tools\kos-tool.exe build` produces a genuine `build/kos.elf` containing the real symbols (`enter_userspace`).
6. Verdict: CLEAN or INTEGRITY VIOLATION. If any cheating, dummy facades, or shortcuts are found, report INTEGRITY VIOLATION with detailed evidence.

Write your complete audit report to `d:\DATA\source\OS\KOS\OS2\.agents\auditor_m3_1_gen2\handoff.md` and send a message to caller with your verdict and findings.
