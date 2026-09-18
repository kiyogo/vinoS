# E2E Test Infra: KOS Milestone K13 & K14

## Test Philosophy
- Opaque-box, requirement-driven. Derives from user requirements in ORIGINAL_REQUEST.md.
- Verification across 4 systematic tiers:
  - Tier 1: Feature Coverage (isolated functionality of address space, privilege transition, syscalls, ELF loader)
  - Tier 2: Boundary & Corner Cases (null pointer, guard pages, address overflow, non-canonical addresses, truncated ELFs, corrupted magic)
  - Tier 3: Cross-Feature Combinations (timer preemption during syscalls, thread switching across address spaces, multi-page user copies)
  - Tier 4: Real-World Application Scenarios (interactive `exec` running C program in Ring 3, 50-iteration lifecycle with zero leaks)

## Feature Inventory & Test Mapping
| # | Feature | Requirement | Tier 1 | Tier 2 | Tier 3 | Tier 4 |
|---|---------|-------------|:------:|:------:|:------:|:------:|
| 1 | Address Space Isolation | R1 | 5 | 5 | ✓ | ✓ |
| 2 | Safe User Memory Copy | R1 | 5 | 5 | ✓ | ✓ |
| 3 | Ring 3 Privilege Transition | R3 | 5 | 5 | ✓ | ✓ |
| 4 | Syscall Entry & ABI (1..7) | R4 | 7 | 5 | ✓ | ✓ |
| 5 | ELF64 Binary Loader | R2 | 5 | 6 | ✓ | ✓ |
| 6 | User C Runtime & Binary | R5 | 5 | 5 | ✓ | ✓ |

## Test Runner Architecture
- Host compilation: `tools/kos-tool.exe build` (asserts clean build, exit code 0).
- QEMU automated test execution: `tools/kos-tool.exe run` executing `usertest all` and `exec C:\HELLO.ELF`.
- Acceptance criteria:
  1. `tools/kos-tool.exe build` succeeds with 0 warnings, 0 errors.
  2. Ring 3 user ELF execution succeeds in CPL 3.
  3. `SYS_WRITE` outputs correctly to console/serial.
  4. Deliberate faults (#PF on kernel address, NULL, guard page) cleanly caught and process terminated safely with 0 kernel panic.
  5. 100% memory reclamation with 0 frame leaks and 0 heap leaks after 50 cycles.
