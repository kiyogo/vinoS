## 2026-09-18T10:25:34Z
You are the M1 Implementation Worker (worker_m1_1) for Milestone K13 & K14 in KOS x86_64.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\worker_m1_1
The project root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (read section ## Follow-up — 2026-09-18T09:30:36Z).
You must read ORIGINAL_REQUEST.md before starting work.
Project architecture is documented in: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k13_k14_1\PROJECT.md.

Read the technical design reports prepared by the 3 M1 Explorers before implementing:
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\handoff.md (Address Space, VMAs, lifecycle, zero-leak reclamation)
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md (Safe user copy routines via HHDM, MSR access primitives)
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\handoff.md (Process integration, tools/kos.c update, automated self-tests)

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Scope & Exclusive File Ownership:
You have exclusive write ownership of the following files:
1. `include/kos/cpu.h`: Declare `cpu_read_msr`, `cpu_write_msr` and MSR constants (`IA32_EFER`, `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`/`IA32_SFMASK`).
2. `arch/x86_64/cpu_control.c`: Implement `cpu_read_msr` and `cpu_write_msr`.
3. `include/kos/address_space.h`: New header defining `struct vm_area`, `struct address_space`, VMA flags (`VMA_READ`, `VMA_WRITE`, `VMA_EXEC`, `VMA_STACK`, `VMA_HEAP`), lifecycle prototypes (`as_create`, `as_ref`, `as_unref`, `as_destroy`, `as_map_region`, `as_unmap_region`, `as_find_vma`), and safe memory prototypes (`user_memory_validate`, `copy_from_user`, `copy_to_user`, `strndup_from_user`, `as_self_test`).
4. `mm/address_space.c`: New source file implementing:
   - Address space lifecycle (`as_create` using `vmm_create_address_space()`, `as_ref`, `as_unref`, `as_destroy` using `vmm_destroy_address_space()`, zero frame leaks).
   - VMA list management and region mapping (`as_map_region` with overlap check, `as_unmap_region`, `as_find_vma`).
   - Safe user copy functions (`user_memory_validate`, `copy_from_user`, `copy_to_user`, `strndup_from_user`) validating canonical bounds (`< 0x0000800000000000ull`), page presence and user/writable flags via `vmm_query_page_in()`, and copying via HHDM virtual addresses to eliminate kernel page fault panics.
   - Self-test `as_self_test()` verifying frame accounting before == after, null/kernel address rejection with -EFAULT (-14), multi-page copies, and strndup.
5. `include/kos/process.h`: Forward declare `struct address_space;` and add `struct address_space *as;` to `struct process`.
6. `kernel/process.c`:
   - Initialize `proc->as = NULL;` in `process_create()`, `idle_process`, and `kernel_process`.
   - Update `process_exec()` to create/associate `proc->as`, map VMAs, synchronize `proc->pml4_physical = proc->as->pml4_phys`, and clean up prior address space via `as_unref(old_as)`.
   - Update `process_destroy()` to call `as_unref(proc->as)`.
7. `tools/kos.c`: Add `"mm/address_space.c"` to `c_sources[]`.
8. Host build tool: Recompile `tools/kos-tool.exe` with host Clang compiler (`clang tools/kos.c -o tools/kos-tool.exe` or equivalent), then run `tools\kos-tool.exe build` to verify 100% clean compilation (-Wall -Wextra -Werror, 0 warnings, 0 errors).
