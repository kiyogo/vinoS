# Milestone M2 Implementation Changes (Requirement R1)

## Summary of Modified Files
- `include/kos/vmm.h`
- `mm/vmm.c`
- `include/kos/elf.h`
- `fs/elf.c`

---

## 1. `include/kos/vmm.h`
### Changes Made
- Added `VMM_PAGE_USER = (1ull << 2)` flag constant for page table user-mode permissions (CPL 3 access).
- Declared HHDM translation functions:
  - `void *vmm_physical_to_virtual(uint64_t physical_address)`
  - `uint64_t vmm_virtual_to_physical(const void *virtual_address)`
- Declared per-process address space lifecycle and mapping routines:
  - `uint64_t vmm_create_address_space(void)`
  - `void vmm_destroy_address_space(uint64_t pml4_phys)`
  - `void vmm_clear_user_address_space(uint64_t pml4_phys)`
  - `bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virtual_address, uint64_t physical_address, uint64_t flags)`
  - `uint64_t vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virtual_address)`
  - `bool vmm_query_page_in(uint64_t pml4_phys, uint64_t virtual_address, uint64_t *physical_address, uint64_t *flags)`

### Rationale & Design Decisions
- `VMM_PAGE_USER` corresponds to bit 2 of x86_64 page table entries, which must be set on all page hierarchy levels (PML4E, PDPTE, PDE, PTE) for user-space (Ring 3) code to access memory.
- `vmm_map_page_in`, `vmm_unmap_page_in`, and `vmm_query_page_in` allow mapping, querying, and tearing down page tables targeting any arbitrary process PML4 via the Higher-Half Direct Map (HHDM), without requiring the caller to switch CR3 to that process address space.

---

## 2. `mm/vmm.c`
### Changes Made
- Stored `kernel_pml4_phys` inside `struct virtual_memory_manager` during `vmm_initialize()`.
- Implemented `vmm_physical_to_virtual` and `vmm_virtual_to_physical` conversions using `vmm.hhdm_offset`.
- Implemented `vmm_create_address_space()`:
  - Allocates a physical frame via `pmm_allocate_frame()`.
  - Clears lower-half entries 0..255 (canonical user address space `0x0000000000000000` - `0x00007FFFFFFFFFFF`).
  - Clones higher-half entries 256..511 directly from the kernel PML4 (`vmm.pml4`), sharing all kernel mappings, HHDM, LAPIC/IOAPIC, MMIO, and kernel heap without duplication.
- Implemented `vmm_clear_user_address_space()`:
  - Traverses only lower-half entries 0..255 through PML4 -> PDPT -> PD -> PT.
  - Frees all user leaf page frames (`PTE_FRAME_MASK`) and intermediate table frames via `pmm_free_frame()`.
  - Zeroes entries 0..255 in the PML4.
  - Leaves higher-half entries 256..511 completely untouched.
- Implemented `vmm_destroy_address_space()`:
  - Checks against destroying `vmm.kernel_pml4_phys` or NULL (no-op with warning).
  - If the destroyed address space is currently loaded in CR3, safely reloads CR3 with `vmm.kernel_pml4_phys`.
  - Calls `vmm_clear_user_address_space()`.
  - Frees the PML4 root table frame via `pmm_free_frame()`.
- Implemented `vmm_map_page_in()`:
  - Enforces 4 KiB alignment on virtual and physical addresses.
  - Security check: if `flags & VMM_PAGE_USER`, rejects any virtual address `>= 0x0000800000000000ull` to prevent user mapping into kernel higher-half address space.
  - Dynamically allocates intermediate tables (PDPT, PD, PT) if not present, zeroing new tables.
  - Correctly propagates `PAGE_USER` and `PAGE_WRITABLE` permissions to intermediate table entries so user-mode code can access the mapped leaf page.
  - If the target PML4 is currently active in CR3, executes `invlpg` to flush the TLB entry.
- Implemented `vmm_unmap_page_in()` and `vmm_query_page_in()`:
  - Safely walks the 4-level paging hierarchy of the target PML4 via HHDM.
  - Clears leaf PTE, executes `invlpg` if target PML4 is active, and returns the physical frame address.
- Implemented `vmm_self_test()`:
  - Automatically runs on boot to verify: address space creation, security boundaries (rejection of user mapping in kernel space), page mapping, querying, unmapping, page table destruction, and 0 physical frame leakage (`pmm_get_free_frame_count()` before vs after).

### Rationale & Design Decisions
- AMD64 architecture requires bit 2 (`PAGE_USER`) to be set on every level of the page hierarchy (PML4E, PDPTE, PDE, PTE) for user-mode ring 3 execution; otherwise, any Ring 3 access generates a page fault (#PF with US=1). Our `next_table_in` helper properly sets `PAGE_USER` on intermediate tables when a user-space mapping is created.
- The recursive user page table freeing logic strictly bounds traversal to indices 0..255, completely isolating kernel page tables from accidental destruction or frame leakage.

---

## 3. `include/kos/elf.h`
### Changes Made
- Added standard ELF-64 scalar data types (`Elf64_Addr`, `Elf64_Off`, `Elf64_Half`, `Elf64_Word`, `Elf64_Sword`, `Elf64_Xword`, `Elf64_Sxword`).
- Added standard ELF identification constants (`EI_MAG0`..`EI_NIDENT`, `ELFMAG0`..`ELFMAG3`, `ELFCLASS64`, `ELFDATA2LSB`, `EV_CURRENT`), object types (`ET_EXEC`, `ET_DYN`), machine types (`EM_X86_64`), segment types (`PT_LOAD`, `PT_INTERP`, `PT_GNU_EH_FRAME`, `PT_GNU_STACK`, `PT_GNU_RELRO`), and segment flags (`PF_X`, `PF_W`, `PF_R`).
- Defined standard `Elf64_Ehdr` (64 bytes) and `Elf64_Phdr` (56 bytes) structs.
- Added compile-time static assertions `_Static_assert(sizeof(Elf64_Ehdr) == 64)` and `_Static_assert(sizeof(Elf64_Phdr) == 56)`.
- Defined `struct elf_binary_image`:
  - `uint64_t entry_point`
  - `uint64_t user_stack_top`
  - `uint64_t user_stack_bottom`
  - `uint64_t user_rsp`
- Defined user stack memory constants:
  - `USER_STACK_TOP` = `0x00007FFFFFFFE000ull`
  - `USER_STACK_SIZE` = `64 * 1024ull` (16 pages)
  - `USER_STACK_BOTTOM` = `0x00007FFFFFFEE000ull`
  - `USER_STACK_GUARD_PAGE` = `0x00007FFFFFFED000ull`
- Declared functions:
  - `bool elf_load_binary(const char *path, uint64_t pml4_phys, struct elf_binary_image *out_image)`
  - `bool process_setup_user_stack(uint64_t pml4_phys, int argc, char *const argv[], uint64_t *out_user_rsp)`
  - `bool elf_self_test(void)`

---

## 4. `fs/elf.c`
### Changes Made
- Implemented `process_setup_user_stack()`:
  - Explicitly ensures the guard page (`USER_STACK_GUARD_PAGE = 0x00007FFFFFFED000ull`) is unmapped (returns 0 / non-present) so any stack overflow triggers a deterministic page fault (#PF).
  - Allocates 16 physical frames via `pmm_allocate_frame()` and maps them into the process address space from `USER_STACK_BOTTOM` up to `USER_STACK_TOP` with permissions `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.
  - Zeroes each allocated stack page via HHDM virtual addresses.
  - Lays out the initial user stack conforming to the System V AMD64 ABI specification:
    1. Null-terminated argument strings copied to high memory in the stack.
    2. Alignment padding so that upon user entry point, RSP satisfies 16-byte alignment (`rsp % 16 == 0`).
    3. Auxiliary vector: `AT_NULL` entries (`a_type = 0, a_val = 0`).
    4. Environment vector: NULL pointer (`envp[0] = NULL`).
    5. Argument pointers: `argv[argc] = NULL`, `argv[argc-1]`, ..., `argv[0]`.
    6. Argument count: `argc`.
  - On any mapping failure, cleans up all allocated stack pages and intermediate structures via `vmm_clear_user_address_space()` and returns false.
- Implemented `elf_load_binary()`:
  - Loads ELF file into temporary kernel buffer via `vfs_read_file()`.
  - Validates ELF magic (`\x7fELF`), 64-bit class (`ELFCLASS64`), little-endian data (`ELFDATA2LSB`), version (`EV_CURRENT`), machine (`EM_X86_64`), and file type (`ET_EXEC`).
  - Rejects dynamically linked executables (`PT_INTERP` segment).
  - Validates entry point address: strictly `< 0x0000800000000000ull` and `>= 0x00200000ull` (above 2 MiB null-pointer barrier).
  - Iterates over all `PT_LOAD` program headers:
    - Validates bounds: segment virtual address and `vaddr + memsz` must be canonical user addresses `< 0x0000800000000000ull`.
    - Validates `filesz <= memsz`.
    - Validates file offset and size within image bounds.
    - Checks against overlapping segments.
  - Verifies that `entry_point` falls within an executable (`PF_X`) `PT_LOAD` segment.
  - Allocates and maps pages for each `PT_LOAD` segment:
    - Page flags set strictly based on segment flags:
      - `VMM_PAGE_USER`
      - `VMM_PAGE_WRITABLE` (if `p_flags & PF_W`)
      - `VMM_PAGE_NO_EXECUTE` (if `!(p_flags & PF_X)`) - enforcing W^X security.
    - Uses HHDM to copy file bytes from segment image into the newly mapped page frame.
    - If `p_memsz > p_filesz`, zeroes remaining bytes in the page (BSS segment).
  - Calls `process_setup_user_stack()` to initialize the user stack.
  - Populates `struct elf_binary_image` (`entry_point`, `user_stack_top`, `user_stack_bottom`, `user_rsp`).
  - Comprehensive error rollback: if any validation, frame allocation, mapping, or stack setup step fails, frees all allocated frames and tables using `vmm_clear_user_address_space()`, frees the file buffer, and returns false without leaking frames or causing kernel panics.
- Implemented `elf_self_test()`:
  - Verifies rejection of corrupted/invalid ELF headers.
  - Verifies rejection of NULL parameters and invalid paths.
  - Verifies full user stack allocation, ABI layout, 16-byte alignment, unmapped guard page, and 0 frame leakage upon destruction.

---

## Verification
- **Build**: `tools\kos-tool.exe build` passed with 0 warnings and 0 errors.
- **QEMU Boot & Live Self-Tests**:
  - `[ 0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).`
  - `[ 0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).`
  - Zero frame leaks verified via `pmm_get_free_frame_count()`.
