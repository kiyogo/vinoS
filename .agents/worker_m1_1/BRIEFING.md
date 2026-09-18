# BRIEFING — 2026-09-18T10:25:34Z

## Mission
Implement Milestone K13 & K14: KOS x86_64 Address Space, VMAs, Safe User Copy via HHDM, MSR Access Primitives, Process Integration, and Self-Tests.

## 🔒 My Identity
- Archetype: worker
- Roles: implementer, qa, specialist
- Working directory: D:\DATA\source\OS\KOS\OS2\.agents\worker_m1_1
- Original parent: e14a4ad7-2efe-4455-96fc-1c0402fd136f
- Milestone: K13 & K14

## 🔒 Key Constraints
- Exclusive file ownership:
  1. `include/kos/cpu.h`
  2. `arch/x86_64/cpu_control.c`
  3. `include/kos/address_space.h`
  4. `mm/address_space.c`
  5. `include/kos/process.h`
  6. `kernel/process.c`
  7. `tools/kos.c`
- Zero frame leaks (frame accounting before == after).
- Strict canonical bounds (< 0x0000800000000000ull).
- No kernel page fault panics: copy via HHDM virtual addresses after vmm_query_page_in.
- Integrity: no dummy or hardcoded test returns.
- Clean build: tools/kos-tool.exe build must pass with -Wall -Wextra -Werror, 0 warnings, 0 errors.

## Current Parent
- Conversation ID: e14a4ad7-2efe-4455-96fc-1c0402fd136f
- Updated: 2026-09-18T10:25:34Z

## Task Summary
- **What to build**: Address space lifecycle, VMAs, safe user copy routines, MSR primitives, process integration, and self-tests.
- **Success criteria**: Full implementation passing all tests, zero frame leaks, clean compilation with kos-tool.exe.
- **Interface contracts**: PROJECT.md & explorer handoffs.
- **Code layout**: include/kos, arch/x86_64, mm, kernel, tools.

## Change Tracker
- **Files modified**: [TBD]
- **Build status**: [TBD]
- **Pending issues**: none

## Quality Status
- **Build/test result**: [TBD]
- **Lint status**: clean
- **Tests added/modified**: as_self_test() in mm/address_space.c

## Loaded Skills
- None.

## Key Decisions Made
- [TBD]

## Artifact Index
- D:\DATA\source\OS\KOS\OS2\.agents\worker_m1_1\handoff.md
- D:\DATA\source\OS\KOS\OS2\.agents\worker_m1_1\progress.md
