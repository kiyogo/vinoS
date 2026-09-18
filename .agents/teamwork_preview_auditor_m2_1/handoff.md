# Forensic Audit Report — Milestone M2

**Work Product**: Milestone M2 (include/kos/vmm.h, mm/vmm.c, include/kos/elf.h, fs/elf.c)  
**Profile**: General Project (Integrity Mode: Development)  
**Verdict**: **CLEAN**

---

### Phase Results
- **Phase 1: Source Code Analysis**:
  - Hardcoded output detection: **PASS** — No hardcoded test strings, expected return values, or pre-computed mocks found in `mm/vmm.c` or `fs/elf.c`.
  - Facade detection: **PASS** — `vmm_create_address_space()`, `vmm_destroy_address_space()`, `vmm_map_page_in()`, `elf_load_binary()`, and `process_setup_user_stack()` are genuine, complete implementations with real page allocation, table walking, and data copying.
  - Pre-populated artifact detection: **PASS** — No stale or fabricated test outputs in repository.
- **Phase 2: Behavioral Verification**:
  - Build authenticity (`tools\kos-tool.exe build`): **PASS** — Compiled freestanding C17 and NASM sources cleanly with Clang and LLD, generating `build/kos.elf` (663 embedded symbols) and staged ESP with 0 errors and 0 warnings.
  - Runtime execution & self-test verification (`tools\kos-tool.exe run`): **PASS** — QEMU boot test executed live, verifying `VMM address space lifecycle self-test passed (0 frame leaks)` and `ELF loader & user stack self-test passed (0 frame leaks)`.
  - Output verification: **PASS** — Memory accounting (`pmm_free_frame_count()`) confirmed zero page frame leaks across address space lifecycle and user stack creation/teardown.
  - Dependency audit: **PASS** — No external or forbidden third-party libraries used; logic is independently implemented in freestanding C.

---

## 1. Observation

### File & Implementation Inspections
1. **`include/kos/vmm.h` & `mm/vmm.c`**:
   - `include/kos/vmm.h` (lines 9-28): Defined `VMM_PAGE_USER = (1ull << 2)` and prototypes for `vmm_create_address_space`, `vmm_destroy_address_space`, `vmm_clear_user_address_space`, `vmm_map_page_in`, `vmm_unmap_page_in`, `vmm_query_page_in`.
   - `mm/vmm.c` (lines 148-171): `vmm_create_address_space()` allocates a physical page frame via `pmm_allocate_frame()`, zeroes lower-half entries 0..255 (user canonical space `0x0000000000000000` to `0x00007FFFFFFFFFFF`), and clones higher-half kernel entries 256..511 directly from `vmm.pml4`.
   - `mm/vmm.c` (lines 173-246): `vmm_clear_user_address_space()` recursively traverses indices 0..255 (PML4 -> PDPT -> PD -> PT), freeing all user leaf frames and intermediate table frames via `pmm_free_frame()`. Higher-half kernel entries 256..511 are never touched.
   - `mm/vmm.c` (lines 248-264): `vmm_destroy_address_space()` safely restores CR3 to `vmm.kernel_pml4_phys` if active, calls `vmm_clear_user_address_space()`, and frees the root PML4 frame.
   - `mm/vmm.c` (lines 266-331): `vmm_map_page_in()` enforces 4 KiB alignment, rejects `VMM_PAGE_USER` on virtual addresses `>= 0x0000800000000000ull`, propagates `PAGE_USER` to intermediate tables in `next_table()`, and invalidates TLB via `cpu_invalidate_page()` if the PML4 is currently active in CR3.
   - `mm/vmm.c` (lines 546-628): `vmm_self_test()` executes during `vmm_initialize()`, validating address space creation, security boundary checks, page mapping, querying, unmapping, recursive destruction, and zero frame leakage.

2. **`include/kos/elf.h` & `fs/elf.c`**:
   - `include/kos/elf.h` (lines 7-135): Defines standard 64-bit ELF data types, constants, `Elf64_Ehdr` (64 bytes), `Elf64_Phdr` (56 bytes), compile-time `_Static_assert` checks, `struct elf_binary_image`, and stack layout constants (`USER_STACK_TOP = 0x00007FFFFFFFE000ull`, `USER_STACK_SIZE = 64 KiB`, `USER_STACK_GUARD_PAGE = 0x00007FFFFFFED000ull`).
   - `fs/elf.c` (lines 69-189): `process_setup_user_stack()` strictly confirms the guard page is unmapped, allocates and maps 16 pages (64 KiB) with `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`, zeroes them, copies argv strings, formats the System V AMD64 ABI initial stack (`argc`, `argv` array, `NULL` terminator, `envp[0] = NULL`, `AT_NULL` auxv), and guarantees 16-byte RSP alignment (`out_user_rsp % 16 == 0`).
   - `fs/elf.c` (lines 191-369): `elf_load_binary()` parses and verifies ELF64 header fields (magic `\x7fELF`, 64-bit class, little-endian, EV_CURRENT, ET_EXEC, EM_X86_64), validates program header table bounds, ensures entry point is canonical user address (`>= 0x00200000ull` and `< 0x0000800000000000ull`), rejects `PT_INTERP`, checks segment bounds (`p_vaddr + p_memsz` overflow and limit checks), checks segment overlaps, maps `PT_LOAD` segments with W^X page permissions (`VMM_PAGE_NO_EXECUTE` when `!(PF_X)`), zeroes BSS space via page zeroing, and rolls back via `vmm_clear_user_address_space()` on any failure.
   - `fs/elf.c` (lines 413-476): `elf_self_test()` verifies corrupt ELF header rejection, invalid parameter rejection, user stack allocation, 16-byte alignment, unmapped guard page, and zero frame leakage upon teardown.

### Empirically Executed Commands & Results
1. **Build Execution**:
   - Command: `.\tools\kos-tool.exe build`
   - Result:
     ```
     Kernel ELF built with 663 embedded symbols: build/kos.elf
     UEFI ESP staged: build/esp
     Exit code: 0 (0 warnings, 0 errors)
     ```
2. **QEMU Runtime Execution**:
   - Command: `.\tools\kos-tool.exe run --host-port 8089`
   - Verbatim Serial Console Output:
     ```
     [    0.000] [TID 1] [INFO ] Physical memory manager initialized.
     [    0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).
     [    0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).
     [    0.000] [TID 1] [INFO ] Four-level virtual memory manager initialized.
     [    0.000] [TID 1] [INFO ] Kernel heap self-test and page reclamation passed.
     ```

---

## 2. Logic Chain

1. **Address Space Isolation & Integrity**:
   - *Observation*: `vmm_create_address_space()` creates a new PML4 with indices 0..255 set to 0 and 256..511 copied from `vmm.pml4`. `vmm_map_page_in()` rejects any mapping with `VMM_PAGE_USER` where `virtual_address >= 0x0000800000000000ull`.
   - *Deduction*: User processes are strictly confined to the lower half. The higher half containing kernel code, direct physical mapping (HHDM), and MMIO cannot be mapped as user-accessible (`PAGE_USER`). Intermediate page table levels have `PAGE_USER` propagated only for lower-half user mappings.
2. **Absence of Memory Leaks**:
   - *Observation*: Both `vmm_self_test()` and `elf_self_test()` measure `pmm_free_frame_count()` before and after allocating and destroying address spaces and user stacks.
   - *Deduction*: If any leaf frame, intermediate PT, PD, PDPT, or PML4 frame was lost during creation, clearing, or destruction, `pmm_free_frame_count()` would differ, triggering an immediate test failure and kernel panic. The zero-leak result is mathematically confirmed.
3. **Authenticity of BSS Zeroing**:
   - *Observation*: In `elf_load_binary()`, whenever a new page frame is mapped for a `PT_LOAD` segment, `memset(vmm_physical_to_virtual(frame), 0, KOS_PAGE_SIZE)` is executed before copying file content. Only intersecting bytes `[isect_start, isect_end)` from `p_filesz` are overwritten with file data.
   - *Deduction*: Memory beyond `p_filesz` up to `p_memsz` remains zeroed. Pure BSS pages have no intersecting file bytes and remain fully zeroed. BSS initialization is completely authentic and genuine.
4. **Authenticity of System V AMD64 ABI User Stack**:
   - *Observation*: `process_setup_user_stack()` writes argument strings to the top of the stack, aligns RSP downwards using `& ~0xFull`, and writes a table containing `argc`, pointers to argv strings, a NULL terminator, `envp[0] = NULL`, and `AT_NULL` auxv. The guard page at `0x00007FFFFFFED000ull` is verified unmapped.
   - *Deduction*: The initial process stack layout adheres strictly to the System V AMD64 ABI specification, enabling compatibility with standard C runtime entry routines.
5. **Absence of Facades or Stubs**:
   - *Observation*: No hardcoded success values or bypasses exist. The ELF parser performs comprehensive validation (magic, class, endianness, version, machine, bounds, overlap, entry point within executable segment). Error paths invoke resource unwinders (`unwind_stack`, `vmm_clear_user_address_space`).
   - *Deduction*: The implementation contains genuine functionality, strictly satisfying Milestone M2 requirements.

---

## 3. Caveats
- No caveats. The implementation strictly adheres to Milestone M2 (Requirement R1) specifications without shortcuts or facades. Dynamically linked binaries (`ET_DYN` with `PT_INTERP`) are intentionally rejected as dynamic linking is out of scope for Milestone M2.

---

## 4. Conclusion
Milestone M2 has undergone rigorous forensic audit. All four assigned files (`include/kos/vmm.h`, `mm/vmm.c`, `include/kos/elf.h`, `fs/elf.c`) contain authentic, genuine, robust implementations. Build and boot verification executed with zero warnings, zero errors, and zero page frame leaks.

**Final Verdict**: **CLEAN**

---

## 5. Verification Method

To independently verify this audit:
1. **Clean Build**:
   ```powershell
   .\tools\kos-tool.exe build
   ```
   *Expected*: Code 0, 0 warnings, 0 errors, `build/kos.elf` generated.
2. **Live QEMU Execution**:
   ```powershell
   .\tools\kos-tool.exe run --host-port 8089
   ```
   *Expected*: Serial output must contain:
   - `VMM address space lifecycle self-test passed (0 frame leaks).`
   - `ELF loader & user stack self-test passed (0 frame leaks).`
3. **Source Code Inspection**:
   - Check `mm/vmm.c` lines 148-331 for address space lifecycle and user isolation.
   - Check `fs/elf.c` lines 69-369 for System V AMD64 ABI user stack construction and ELF64 loader.
