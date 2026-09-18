# BRIEFING — 2026-09-18T09:58:00Z

## Mission
Investigate KOS's current CPU state management, GDT, TSS, IDT, interrupt/exception handling, context switching, and syscall dispatching to provide an authoritative technical survey for Requirements R3 and R4 (Milestone K13 & K14).

## 🔒 My Identity
- Archetype: explorer
- Roles: Ring 3 Privilege Transition & Syscall ABI Explorer
- Working directory: D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_2
- Original parent: e14a4ad7-2efe-4455-96fc-1c0402fd136f
- Milestone: K13 & K14

## 🔒 Key Constraints
- Read-only investigation — do NOT implement or modify source code in kernel/
- Only write within working directory D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_2
- Follow Handoff Protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method)

## Current Parent
- Conversation ID: e14a4ad7-2efe-4455-96fc-1c0402fd136f
- Updated: 2026-09-18T09:58:00Z

## Investigation State
- **Explored paths**:
  - `arch/x86_64/descriptors.c` & `include/kos/descriptors.h`
  - `arch/x86_64/cpu.asm` & `include/kos/cpu.h`
  - `arch/x86_64/interrupts.c` & `include/kos/interrupts.h` & `arch/x86_64/interrupt_stubs.asm`
  - `kernel/task.c` & `arch/x86_64/task_switch.asm` & `include/kos/task.h`
  - `kernel/process.c` & `include/kos/process.h`
  - `kernel/syscall.c` & `include/kos/syscall.h`
  - `kernel/command.c` (usertest suite)
  - `tools/kos-tool.exe` & `tools/kos.c`
- **Key findings**:
  1. GDT layout has User Code at 0x28 (Selector 0x2B, index 5) and User Data at 0x30 (Selector 0x33, index 6). This is mathematically inverted for `sysretq` (`STAR[63:48] + 8 = SS`, `STAR[63:48] + 16 = CS`). However, returning via `iretq` works 100% reliably with 0x2B/0x33 and avoids the sysret descriptor inversion.
  2. TSS is statically allocated in `arch/x86_64/descriptors.c`, loaded via `ltr 0x18`. `tss.rsp0` is properly maintained in `schedule()` (`kernel/task.c:307-312`) and `process_exec()`. Double fault (#DF) uses IST1 (`double_fault_stack[16384]`). #PF correctly uses `tss.rsp0`.
  3. `enter_userspace` in `arch/x86_64/cpu.asm` currently only takes `(entry_point, user_rsp)` and zeros all GPRs. Requirement R3 needs `enter_user_mode(entry_point, user_rsp, argc, argv)` setting `rdi = argc`, `rsi = argv`.
  4. Context switch in `schedule()` (`kernel/task.c:301-315`) updates CR3 if `previous->process != next->process` and updates `tss.rsp0`. A slight hardening is recommended to ensure switching to kernel PML4 when `next->process == NULL`.
  5. Syscall subsystem currently has old enum in `syscall.h` (1=GETTIME, 2=SYSINFO, 3=WRITE, 4=EXIT). Requirement R4 mandates: 1=SYS_EXIT, 2=SYS_WRITE, 3=SYS_READ, 4=SYS_GETTIME, 5=SYS_SYSINFO, 6=SYS_YIELD, 7=SYS_SLEEP. Calling convention matches System V AMD64 (`rax=num, rdi, rsi, rdx, r10, r8, r9`).
  6. `kernel/command.c` test suite hardcodes `mov rax, 4` for `SYS_EXIT` across 6 tests and must be synchronized when `SYS_EXIT` becomes 1.
  7. Exception handling in `arch/x86_64/interrupts.c` already detects Ring 3 via `(context->cs & 3) == 3`, terminates user process cleanly via `process_exit(cur, exit_code)` and `schedule()`, preventing kernel panic.
- **Unexplored areas**: None for R3/R4 survey.

## Key Decisions Made
- Advocate using `iretq` as the primary syscall return path for `syscall_entry`, because it supports selectors 0x2B/0x33 without GDT descriptor re-ordering, cleanly restores all RFLAGS and RSP atomically, and handles blocked/preempted syscalls safely.
- Document the alternate `sysretq` path and provide the required GDT reorganization if pure `sysretq` instruction execution is demanded.
- Provide full drop-in assembly stubs for `enter_user_mode` and `syscall_entry`.
- Flag the migration required in `kernel/command.c` due to `SYS_EXIT` changing from 4 to 1.

## Artifact Index
- `DISPATCH.md` — User prompt and requirements
- `progress.md` — Liveness heartbeat
- `BRIEFING.md` — Working memory
- `handoff.md` — Comprehensive technical survey report
