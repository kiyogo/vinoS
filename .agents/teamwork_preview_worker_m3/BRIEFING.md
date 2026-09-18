# BRIEFING — 2026-09-18T00:54:36Z

## Mission
Implement Milestone M3: Ring 3 Transition & Fault Recovery (Requirement R2) for KOS x86_64 kernel.

## 🔒 My Identity
- Archetype: worker
- Roles: implementer, qa, specialist
- Working directory: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m3
- Original parent: 1ae50773-9b0e-4950-a60c-718073a789e9
- Milestone: M3 (Ring 3 Transition & Fault Recovery - Requirement R2)

## 🔒 Key Constraints
- Files owned exclusively:
  - `arch/x86_64/cpu.asm`
  - `include/kos/cpu.h`
  - `arch/x86_64/interrupts.c`
- Never place source, tests, or data files in `.agents/`
- Minimal change principle: only modify what is necessary
- Genuine implementations only: no hardcoding, no dummy facades
- Clean build: `tools\kos-tool.exe build` with exit code 0, 0 warnings, 0 errors
- Do NOT call `cpu_halt()` or panic kernel on user faults; terminate process cleanly via `process_exit()` and `schedule()`

## Current Parent
- Conversation ID: 1ae50773-9b0e-4950-a60c-718073a789e9
- Updated: 2026-09-18T00:54:36Z

## Task Summary
- **What to build**:
  1. In `arch/x86_64/cpu.asm`: `enter_userspace(uint64_t entry_point, uint64_t user_rsp)` with iretq stack frame (SS=0x33, RSP=user_rsp, RFLAGS=0x202, CS=0x2b, RIP=entry_point), DS/ES/FS/GS=0x33, GPR scrubbing, `iretq`.
  2. In `include/kos/cpu.h`: Declaration of `enter_userspace(...) __attribute__((noreturn));`.
  3. In `arch/x86_64/interrupts.c`:
     - User page fault containment in `page_fault_handler()` -> log diagnostic, call `process_exit(process_get_current(), -11)` and `schedule()`.
     - User exception containment in `interrupt_dispatch()` / `default_exception_panic()` -> log warning, call `process_exit(process_get_current(), -1)` and `schedule()`.
- **Success criteria**:
  - `tools\kos-tool.exe build` succeeds with 0 warnings, 0 errors.
  - Ring 3 transitions execute via iretq with scrubbed registers and correct segment selectors.
  - User mode exceptions (#PF, #GP, #UD) terminate the faulting process without crashing/panicking the kernel.
- **Interface contracts**: `.agents/orchestrator_k14_1/PROJECT.md`
- **Code layout**: `.agents/orchestrator_k14_1/PROJECT.md § Code Layout`

## Key Decisions Made
- [TBD]

## Artifact Index
- `.agents/teamwork_preview_worker_m3/DISPATCH.md` — Assignment instructions
- `.agents/teamwork_preview_worker_m3/BRIEFING.md` — Working state and memory
- `.agents/teamwork_preview_worker_m3/progress.md` — Progress tracker and heartbeat
- `.agents/teamwork_preview_worker_m3/changes.md` — Code changes record
- `.agents/teamwork_preview_worker_m3/handoff.md` — Self-contained handoff report

## Change Tracker
- **Files modified**: None yet
- **Build status**: Not tested yet
- **Pending issues**: None

## Quality Status
- **Build/test result**: Not yet run
- **Lint status**: 0 violations
- **Tests added/modified**: None yet

## Loaded Skills
- None
