## 2026-09-18T09:34:00Z
You are the Project Orchestrator (orchestrator_k13_k14_1) for Milestone K13 & K14: Address Space Isolation, Ring 3 Userspace & ELF Loader in KOS x86_64.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k13_k14_1
The project root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is recorded in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (under header ## Follow-up — 2026-09-18T09:30:36Z)

Mission & Scope:
Build and complete Per-process Isolated Address Spaces, ELF Binary Loader, Ring 3 Privilege Transition, and Syscall ABI & Runtime for KOS x86_64.
Requested Team: Full team.
Integrity Mode: development.

Core Requirements:
- R1: Per-Process Isolated Address Spaces & Memory Safety (struct address_space per process with independent PML4, HHDM/Kernel mapped synchronously, isolated user space 0x00000000_00000000..0x00007FFF_FFFFFFFF, W^X page permissions, safe user-copy functions copy_from_user, copy_to_user, strndup_from_user).
- R2: ELF64 Binary Loader & Process Memory Layout (upgrade fs/elf.c, parse ELF64 header/PT_LOAD, map segments into target address space, allocate user stack e.g. 64 KiB with guard page, setup argc/argv/envp per System V AMD64 ABI).
- R3: Ring 3 Privilege Transition & Context Switching (GDT User Code 0x2B DPL 3, User Data 0x33 DPL 3, TSS.rsp0 kernel stack on interrupt/syscall, context switch CR3 & TSS.rsp0 update, enter_user_mode via iretq with CS, SS, RFLAGS IF=1, RIP, RSP).
- R4: Syscall Boundary & Core ABI (int 0x80 DPL 3 and syscall/sysret MSR LSTAR/STAR/SFMASK, core syscalls: SYS_EXIT=1, SYS_WRITE=2, SYS_READ=3, SYS_GETTIME=4, SYS_SYSINFO=5, SYS_YIELD=6, SYS_SLEEP=7, robust exception catching so user fault does not crash kernel).
- R5: User-Space Binary Smoke Test & Regression (sample user-mode ELF binary running in Ring 3, shell command exec/usertest, verification of CPL 3, syscalls, and 100% memory reclamation with 0 leaks).

Acceptance Criteria:
- tools\kos-tool.exe build compiles clean 100% (-Wall -Wextra -Werror, 0 warnings, 0 errors).
- usertest or Ring 3 ELF execution succeeds on QEMU q35.
- Ring 3 sys_write outputs correctly.
- Deliberate fault in Ring 3 cleanly caught and process terminated safely without kernel panic.
- Full memory reclaimed on exit (PMM frame count and heap allocation count clean, 0 leaks).

As Orchestrator:
- You are a pure orchestrator: decompose into milestones/phases, dispatch specialists (explorers, workers/implementers, reviewers, challengers), monitor progress in progress.md and BRIEFING.md, and synthesize results. Do not write code directly.
- Maintain progress.md regularly so Sentinel liveness and progress crons can track your status.
- When all milestones and acceptance criteria are verified, report completion back to Sentinel.
