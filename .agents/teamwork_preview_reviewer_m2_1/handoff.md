# Handoff Report — Milestone M2 Review & Adversarial Audit

**Agent**: `teamwork_preview_reviewer_m2_1`  
**Roles**: `reviewer`, `critic`  
**Verdict**: **APPROVE**  
**Overall Risk Assessment**: **LOW**

---

## 1. Observation

### Source Code Examination
1. **`include/kos/vmm.h`**:
   - Lines 8-13: Added `VMM_PAGE_USER = (1ull << 2)` flag constant and macro definition.
   - Lines 18-28: Declared address space lifecycle and mapping routines:
     `vmm_create_address_space`, `vmm_destroy_address_space`, `vmm_clear_user_address_space`, `vmm_map_page_in`, `vmm_unmap_page_in`, `vmm_query_page_in`.
   - Lines 16-17: Declared HHDM physical-to-virtual and virtual-to-physical conversion routines.

2. **`mm/vmm.c`**:
   - Lines 27: Added `kernel_pml4_phys` to `struct virtual_memory_manager` initialized during `vmm_initialize()`.
   - Lines 97-125 (`next_table`): Correctly sets `PAGE_USER` on intermediate tables (PML4E, PDPTE, PDE) when `virtual_address < 0x0000800000000000ull` or `flags & VMM_PAGE_USER`.
   - Lines 148-171 (`vmm_create_address_space`): Allocates new PML4 frame, zeroes entries 0..255 (lower-half canonical user space `0x0000000000000000` - `0x00007FFFFFFFFFFF`), and clones entries 256..511 directly from `vmm.pml4` (higher-half shared kernel space).
   - Lines 173-246 (`vmm_clear_user_address_space`): Guards against clearing `vmm.kernel_pml4_phys`, walks lower-half entries 0..255 through PML4 -> PDPT -> PD -> PT, frees leaf and intermediate table frames via `pmm_free_frame()`, zeroes entries 0..255, and safely restores CR3 to `vmm.kernel_pml4_phys` if the cleared address space is currently loaded in CR3.
   - Lines 248-264 (`vmm_destroy_address_space`): Guards against destroying `vmm.kernel_pml4_phys` or 0, restores CR3 if active, clears user mappings via `vmm_clear_user_address_space()`, and frees root PML4 frame via `pmm_free_frame()`.
   - Lines 266-331 (`vmm_map_page_in`): Enforces 4 KiB page alignment, validates flags, rejects `VMM_PAGE_USER` when `virtual_address >= 0x0000800000000000ull`, rejects non-canonical addresses, dynamically allocates intermediate tables, checks for already present leaves, flushes TLB via `cpu_invalidate_page()` if active, and unwinds newly created tables via `release_owned_empty_table()` upon failure.
   - Lines 546-629 (`vmm_self_test`): Validates address space creation, higher-half sharing, lower-half zeroing, security boundary rejection of user mapping in kernel space, query, unmap, recursive teardown, and confirms 0 frame leaks via `pmm_free_frame_count()`.

3. **`include/kos/elf.h`**:
   - Lines 8-14: Standard ELF-64 scalar data types (`Elf64_Addr`, `Elf64_Off`, `Elf64_Half`, `Elf64_Word`, `Elf64_Sword`, `Elf64_Xword`, `Elf64_Sxword`).
   - Lines 17-76: Standard ELF identification constants (`ELFMAG0..3`, `ELFCLASS64`, `ELFDATA2LSB`, `EV_CURRENT`), object types (`ET_EXEC`), machine (`EM_X86_64`), segment types (`PT_LOAD`, `PT_INTERP`, etc.), and segment flags (`PF_X`, `PF_W`, `PF_R`).
   - Lines 78-108: Standard `Elf64_Ehdr` (64 bytes) and `Elf64_Phdr` (56 bytes) structs, with static assertions `_Static_assert(sizeof(Elf64_Ehdr) == 64)` and `_Static_assert(sizeof(Elf64_Phdr) == 56)`.
   - Lines 111-129: Definition of `struct elf_binary_image` and user stack layout constants:
     - `USER_STACK_TOP = 0x00007FFFFFFFE000ull`
     - `USER_STACK_SIZE = 64 * 1024ull` (16 pages)
     - `USER_STACK_BOTTOM = 0x00007FFFFFFEE000ull`
     - `USER_STACK_GUARD_PAGE = 0x00007FFFFFFED000ull`

4. **`fs/elf.c`**:
   - Lines 48-67 (`stack_write_bytes`): Safely writes bytes into mapped user stack pages using HHDM virtual addresses, with bounds checks against `USER_STACK_BOTTOM` and `USER_STACK_TOP`.
   - Lines 69-189 (`process_setup_user_stack`):
     - Validates `argc <= 64` and non-null `argv`.
     - Explicitly ensures `USER_STACK_GUARD_PAGE` is unmapped (lines 80-83).
     - Allocates and maps 16 pages from `USER_STACK_BOTTOM` to `USER_STACK_TOP` with permissions `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.
     - Reuses existing stack pages without duplicating allocations or leaking frames if called multiple times.
     - Lays out System V AMD64 ABI stack: argv strings, padding for 16-byte RSP alignment, `auxv[0]` (`AT_NULL`), `envp[0]` (`NULL`), `argv[argc] = NULL`, `argv[argc-1]..argv[0]`, `argc`.
     - Ensures `target_rsp % 16 == 0` (line 159: `(curr_sp - table_bytes) & ~0xFull`).
   - Lines 191-369 (`elf_load_binary`):
     - Validates ELF magic, 64-bit class, little endian, version, `ET_EXEC`, `EM_X86_64`, header sizes, and `e_phnum <= 64`.
     - Rejects dynamic executables (`PT_INTERP`).
     - Validates entry point: `ehdr->e_entry >= 0x00200000ull && ehdr->e_entry < 0x0000800000000000ull`.
     - Validates `PT_LOAD` segments: `p_memsz >= p_filesz`, file range validity, virtual address bounds, no overflow, power-of-two alignment.
     - Verifies non-overlapping `PT_LOAD` segments (lines 270-283).
     - Verifies entry point is located inside an executable (`PF_X`) `PT_LOAD` segment.
     - Enforces W^X page permissions: `VMM_PAGE_USER`, `VMM_PAGE_WRITABLE` only if `PF_W`, `VMM_PAGE_NO_EXECUTE` if `!(PF_X)`.
     - Allocates frames, zeroes them, copies file content, zeroes remaining BSS bytes.
     - Sets up user stack via `process_setup_user_stack()`.
     - On any failure, triggers complete rollback via `vmm_clear_user_address_space(pml4_phys)` and returns `false`.
   - Lines 413-476 (`elf_self_test`):
     - Verifies rejection of corrupted ELF headers and null arguments.
     - Verifies user stack creation, 16-byte RSP alignment, unmapped guard page, user permissions, address space destruction, and 0 frame leaks.

### Independent Build & Runtime Verification
1. **Compilation Command**: `tools\kos-tool.exe build`
   - Result: Exit code 0, 0 compiler warnings, 0 linker warnings.
   - Linker output: `Kernel ELF built with 663 embedded symbols: build/kos.elf`
   - ESP staged: `build/esp`
2. **QEMU Execution Command**: `tools\kos-tool.exe run --host-port 8092`
   - Verbatim console log output:
     ```
     [    0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).
     [    0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).
     ```
   - Both self-tests ran live in QEMU during boot and confirmed 0 page frame leaks.

---

## 2. Logic Chain

1. *Premise*: Requirement R1 and Project K14 Architecture mandate that each user process runs in an isolated address space where lower-half memory (0x0 to 0x00007FFFFFFFFFFF) is private to the process and higher-half memory (0xFFFF800000000000 to 0xFFFFFFFFFFFFFFFF) shares kernel tables.
   *Observation*: `vmm_create_address_space()` allocates a physical frame for PML4, zeroes entries 0..255, and copies entries 256..511 from `vmm.pml4`.
   *Inference*: Private lower half and shared higher half are correctly created with zero cross-process lower-half interference.

2. *Premise*: Ring 3 execution requires page table entries across all 4 levels (PML4E, PDPTE, PDE, PTE) to have the `PAGE_USER` (bit 2) set; otherwise hardware raises a page fault (#PF with U/S=1).
   *Observation*: `next_table()` explicitly sets `*entry |= PAGE_USER` when `virtual_address < 0x0000800000000000ull` or `flags & VMM_PAGE_USER`.
   *Inference*: User mode will have hardware permission to traverse intermediate page tables down to mapped user leaf pages.

3. *Premise*: Security boundaries must prevent user programs from creating or manipulating mappings in the kernel address space.
   *Observation*: `vmm_map_page_in()` explicitly rejects any mapping with `(flags & VMM_PAGE_USER) != 0` when `virtual_address >= 0x0000800000000000ull`, and rejects non-canonical addresses.
   *Inference*: Kernel memory is protected against user-accessible mappings.

4. *Premise*: ELF loading must be robust against corrupted, malicious, or non-static binaries, must enforce W^X permissions, and must not leak memory on load errors.
   *Observation*: `elf_load_binary()` validates magic, 64-bit, endianness, version, machine (`EM_X86_64`), object type (`ET_EXEC`), program header count, bounds, segment overlap, entry point within executable segment, sets `VMM_PAGE_NO_EXECUTE` on non-executable segments, and on any error branches to `load_error` which executes `vmm_clear_user_address_space(pml4_phys)`.
   *Inference*: Malicious or corrupted binaries fail gracefully with `false` without crashing the kernel or leaking frames.

5. *Premise*: System V AMD64 ABI mandates that upon process entry: (a) RSP is 16-byte aligned, (b) stack holds `argc`, `argv` pointers, `envp` pointer, and auxiliary vectors, (c) stack overflow is trapped by a guard page.
   *Observation*: `process_setup_user_stack()` calculates `target_rsp = (curr_sp - table_bytes) & ~0xFull`, writes `argc`, `argv` vector, `NULL` terminator, `envp[0] = NULL`, `AT_NULL` auxv, and verifies `USER_STACK_GUARD_PAGE` is unmapped.
   *Inference*: Stack conforms to System V AMD64 ABI with deterministic stack overflow detection.

6. *Premise*: Process termination must not leak memory or corrupt kernel structures.
   *Observation*: `vmm_clear_user_address_space()` strictly visits entries 0..255, freeing leaf frames and intermediate table frames with `pmm_free_frame()`. Kernel entries 256..511 are untouched. If active in CR3, CR3 is restored to `kernel_pml4_phys`. Both `vmm_self_test()` and `elf_self_test()` confirmed `pmm_free_frame_count()` matches initial count before test.
   *Inference*: Zero frame leaks across address space and ELF lifecycles.

---

## 3. Caveats

- **Static Binaries Only**: Dynamic linking (`ET_DYN` / `PT_INTERP`) is intentionally unsupported for Milestone M2 and correctly rejected by `elf_load_binary()`.
- **Fixed User Stack Size**: The user stack is fixed at 64 KiB (16 pages) with a 4 KiB unmapped guard page. Dynamic stack growth on page fault is not implemented in M2 (standard for freestanding embedded kernel loader).
- No caveats regarding Milestone M2 requirements.

---

## 4. Conclusion

Milestone M2 (Requirement R1: ELF64 Binary Loader & Address Space Mapping) is implemented to high engineering standards. It strictly fulfills all requirements:
1. `vmm_create_address_space()`, `vmm_destroy_address_space()`, `vmm_clear_user_address_space()`, and `vmm_map_page_in()` provide complete 4-level paging isolation with zero frame leaks.
2. `elf_load_binary()` parses, strictly validates, maps ELF-64 executables with W^X protections, zeroes BSS, and rolls back cleanly on error.
3. `process_setup_user_stack()` establishes a 64 KiB stack with an unmapped bottom guard page, 16-byte RSP alignment, and System V AMD64 ABI layout.
4. Clean compilation with 0 warnings, 0 errors, and automated live QEMU boot verification with 0 frame leaks.

**Verdict**: **APPROVE**

---

## 5. Review Summary & Quality Assessment

**Verdict**: **APPROVE**

### Findings
- **None (Critical / Major / Minor)**. All interface contracts, memory safety invariants, and architectural constraints are met.

### Verified Claims
- `vmm_create_address_space()` clones higher-half and zeroes lower-half -> Verified via code inspection and `vmm_self_test()` in QEMU -> PASS
- `vmm_destroy_address_space()` protects kernel PML4 and frees user frames/tables without leaks -> Verified via code inspection and frame count check -> PASS
- `vmm_map_page_in()` blocks user mappings in higher-half -> Verified via `vmm_self_test()` boundary test -> PASS
- `elf_load_binary()` validates headers, segment bounds, overlap, entry point, and enforces W^X -> Verified via code inspection and `elf_inspect_image` -> PASS
- `process_setup_user_stack()` formats System V AMD64 ABI, 16-byte RSP alignment, unmapped guard page -> Verified via code inspection and `elf_self_test()` -> PASS
- Build clean without warnings/errors -> Verified via `tools\kos-tool.exe build` -> PASS

### Coverage Gaps
- None for Milestone M2 scope. Subsequent Ring 3 transition (`enter_userspace`) and process lifecycle integration are scheduled for M3 and M4.

### Unverified Items
- None.

---

## 6. Adversarial Challenge & Stress-Test Report

**Overall Risk Assessment**: **LOW**

### Challenges & Attack Scenarios

#### 1. Overlapping PT_LOAD Segments Attack
- **Assumption Challenged**: An ELF file might declare multiple `PT_LOAD` segments that overlap in virtual address space, potentially causing memory corruption or unexpected overwrites.
- **Stress-Test Analysis**: `fs/elf.c` lines 270-283 implement a quadratic overlap check:
  `uint64_t max_start = start_i > start_j ? start_i : start_j;`
  `uint64_t min_end = end_i < end_j ? end_i : end_j;`
  `if (max_start < min_end) return false;`
- **Result**: PASS. Any overlapping segment is detected and rejected before memory allocation.

#### 2. Higher-Half Privilege Escalation via User Page Mapping
- **Assumption Challenged**: An attacker might craft an ELF program header with `p_vaddr` pointing to kernel space (`>= 0x0000800000000000ull`) or non-canonical space.
- **Stress-Test Analysis**:
  - `fs/elf.c` lines 251-255: `if (end_va < phdr->p_vaddr || end_va >= 0x0000800000000000ull) return false;`
  - `mm/vmm.c` line 272: `if ((flags & VMM_PAGE_USER) != 0 && virtual_address >= 0x0000800000000000ull) return false;`
  - `mm/vmm.c` line 275: `if (virtual_address >= 0x0000800000000000ull && virtual_address < 0xffff800000000000ull) return false;`
- **Result**: PASS. Defense-in-depth across both ELF loader and VMM prevents any user mapping outside the lower canonical half.

#### 3. Out-of-Memory / Partial Failure Rollback
- **Assumption Challenged**: If PMM runs out of memory mid-way through loading a multi-megabyte ELF or stack allocation, partial page tables and frames could leak.
- **Stress-Test Analysis**:
  - `fs/elf.c` lines 324, 329, 354: All jump to `load_error`, which calls `vmm_clear_user_address_space(pml4_phys)`.
  - `vmm_clear_user_address_space()` traverses all 256 user PML4 entries, freeing all present leaf frames and intermediate table frames.
- **Result**: PASS. Zero leaks on partial failure.

#### 4. Stack Guard Page Overflow Detection
- **Assumption Challenged**: If a user process overruns its 64 KiB stack, it might silently corrupt adjacent memory if the page below `USER_STACK_BOTTOM` is mapped.
- **Stress-Test Analysis**:
  - `USER_STACK_GUARD_PAGE` (`0x00007FFFFFFED000ull`) is checked and unmapped during `process_setup_user_stack()`.
  - In `elf_self_test()`, `vmm_query_page_in(test_pml4, USER_STACK_GUARD_PAGE, 0, 0)` explicitly verifies that the page is not present.
- **Result**: PASS. A non-present page fault will be triggered upon hitting the guard page.

#### 5. Active CR3 Destruction Safety
- **Assumption Challenged**: Destroying an address space that is currently active on the CPU could cause immediate CPU fault if page tables are freed while in use.
- **Stress-Test Analysis**:
  - `mm/vmm.c` lines 257-259 and 241-243 check `if ((cpu_read_cr3() & PAGE_ADDRESS_MASK) == pml4_phys)` and reload CR3 with `vmm.kernel_pml4_phys` before clearing or freeing tables.
- **Result**: PASS. Safe address space teardown.

---

## 7. Verification Method

### How to Independently Verify

1. **Clean Compilation**:
   ```powershell
   tools\kos-tool.exe build
   ```
   *Expected Output*: Exit code 0, 0 warnings, 0 errors, `build/kos.elf` generated.

2. **Live QEMU Execution & Automated Self-Tests**:
   ```powershell
   tools\kos-tool.exe run --host-port 8092
   ```
   *Expected Console Logs*:
   ```
   [    0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).
   [    0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).
   ```

3. **Invalidation Conditions**:
   - Any compiler or linker warning treated as error under `-Wall -Wextra -Werror`.
   - Any physical frame leak reported during address space creation/destruction.
   - Any user-accessible page mapped at or above `0x0000800000000000ull`.
   - Stack RSP not 16-byte aligned.
