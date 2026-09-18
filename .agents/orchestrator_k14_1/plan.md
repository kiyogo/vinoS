# Master Plan — Milestone K14: Process and Ring 3 Userspace

## Executive Summary
Milestone K14 introduces userspace capabilities to KOS x86_64:
1. **R1. ELF64 Binary Loader & Address Space Mapping**:
   - Parse and validate ELF64 headers and program headers (magic, 64-bit, little endian, entry point, alignment, limits).
   - Map `PT_LOAD` segments into process virtual address space with accurate page permissions (R/W/X/U).
   - Dedicated user stack with guard page protection against stack overflow.
   - Initial user stack setup adhering to System V AMD64 ABI (`argc`, `argv`, `envp`, auxiliary vector / strings).
2. **R2. Ring 3 Privilege Transition & Hardware Context Management**:
   - Safe Ring 0 -> Ring 3 transition using `iretq` with user code/data segment selectors (`GDT_USER_CS`, `GDT_USER_DS`).
   - Task State Segment (TSS) setup and dynamic `RSP0` updating on every context switch so interrupts/exceptions in Ring 3 switch safely to kernel stack.
3. **R3. Process & Thread Lifecycle Subsystem**:
   - `process_t` and `thread_t` hierarchy (parent-child relationships, states: Ready, Running, Blocked, Zombie, exit status).
   - Management primitives: `process_create`, `process_exec`, `process_exit`, `process_wait`.
   - Thread primitives: `thread_create`, `thread_exit`, `thread_sleep`, `yield`.
   - Parent waiting and zombie reaping.
   - Total resource reclamation (page directories/tables, memory mappings, user stacks, descriptors) without leaks.
   - Foundation signals/events for inter-process synchronization.
   - Abstract interfaces avoiding tight coupling with FS/sockets/TLS.
4. **Verification & Acceptance**:
   - `tools\kos-tool.exe build` compiles without error or warning.
   - QEMU execution verifying:
     - Valid ELF64 binary execution in Ring 3.
     - Graceful rejection of invalid/corrupted ELFs.
     - Page fault enforcement for kernel-only memory access from Ring 3.
     - User stack guard page fault isolation and termination.
     - TSS/RSP0 interrupt/exception handling from Ring 3 with clean return.
     - Parent/child lifecycle, `process_wait`, and zombie reaping.
     - Zero memory leak under repeated process creation/destruction.

## Execution Phases
- **Phase 0: Survey Codebase**
  - Dispatch 3 parallel Explorers to inspect existing GDT/TSS, paging/VMM, task/scheduler, interrupt handlers, and tests.
- **Phase 1: Architecture Synthesis & Detailed Specification**
  - Synthesize findings into `PROJECT.md` / `plan.md`.
- **Phase 2: R1 - ELF64 Loader & Address Space Implementation**
  - Worker -> Reviewers -> Challenger -> Auditor.
- **Phase 3: R2 - TSS, Ring 3 Context Switching & iretq Implementation**
  - Worker -> Reviewers -> Challenger -> Auditor.
- **Phase 4: R3 - Process Lifecycle Subsystem & Memory Reclamation**
  - Worker -> Reviewers -> Challenger -> Auditor.
- **Phase 5: Automated Build & QEMU E2E Verification**
  - Worker/Test Writer -> Challengers -> Auditor.
- **Phase 6: Final Milestone Sign-off and Sentinel Reporting**
