# Dispatch: Worker M2 (VMM Address Space & ELF64 Loader)

## Mission
Implement Milestone M2 (Requirement R1: ELF64 Binary Loader & Address Space Mapping):
1. `include/kos/vmm.h` & `mm/vmm.c`:
   - `uint64_t vmm_create_address_space(void)`:
     - Allocates physical frame for PML4 via `pmm_allocate_frame()`.
     - Zeroes lower-half entries 0..255 (user space).
     - Copies higher-half entries 256..511 from `vmm.pml4`.
     - Returns physical address of new PML4.
   - `void vmm_destroy_address_space(uint64_t pml4_phys)`:
     - Recursively walks user PML4 entries 0..255, PDPTE entries 0..511, PDE entries 0..511, and PTE entries 0..511.
     - Frees mapped user physical frames with `pmm_free_frame()`.
     - Frees PT, PD, and PDPT intermediate table frames with `pmm_free_frame()`.
     - Finally, frees the PML4 frame itself. (Never frees higher-half entries 256..511!).
   - `bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virtual_address, uint64_t physical_address, uint64_t flags)`:
     - Maps a 4 KiB page into `pml4_phys`.
     - Enforces security: rejects user mappings (`flags & VMM_PAGE_USER`) in the higher half (`virtual_address >= 0x0000800000000000ull`).
     - Allocates intermediate tables as needed with `PAGE_USER` for user mappings.
2. `include/kos/elf.h` & `fs/elf.c`:
   - Define canonical ELF64 structures: `Elf64_Ehdr`, `Elf64_Phdr`, and constants (`ET_EXEC`, `EM_X86_64`, `PT_LOAD`, `PF_R`, `PF_W`, `PF_X`, etc.).
   - Define `struct elf_binary_image { uint64_t entry_point; uint64_t user_stack_top; uint64_t user_rsp; };`.
   - Implement `bool elf_load_binary(const char *path, uint64_t pml4_phys, struct elf_binary_image *out_image)`:
     - Reads image via `vfs_read_file()`.
     - Performs strict validation: magic, 64-bit, little-endian, machine EM_X86_64, type ET_EXEC, entry point within canonical lower-half range (`0x00200000` to `0x00007fffffffffff`), non-overlapping `PT_LOAD` segments.
     - Allocates frames, maps pages into `pml4_phys` with exact permissions (`VMM_PAGE_USER`, `VMM_PAGE_WRITABLE`, `VMM_PAGE_NO_EXECUTE`).
     - Copies segment file data and zeroes BSS (`p_memsz > p_filesz`).
     - On any validation or allocation failure, cleans up allocated frames and returns false without kernel panic.
3. User Stack Allocation & System V AMD64 ABI:
   - Implement `bool process_setup_user_stack(uint64_t pml4_phys, int argc, char *const argv[], uint64_t *out_user_rsp)` (can be in `fs/elf.c` or `kernel/process.c` / `mm/vmm.c`):
     - Maps 64 KiB (16 pages) ending at `USER_STACK_TOP = 0x00007FFFFFFFE000ull` with `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.
     - Leaves bottom page (`USER_STACK_TOP - 64 KiB - 4 KiB`) unmapped as the NON-PRESENT GUARD PAGE.
     - Formats initial user stack according to System V AMD64 ABI:
       - Strings for `argv` and `envp` (`envp[0] = NULL`), auxiliary vector (`AT_NULL`).
       - Table of `argv` pointers, `argc`.
       - Aligns `RSP` to 16 bytes before entry.
4. Verify build:
   - Run `tools\kos-tool.exe build` and ensure exit code 0, 0 warnings, 0 errors.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Working Directory
`d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m2`
Write `changes.md` and `handoff.md` in your working directory and report when done.

## 2026-09-18T00:38:36Z
You are teamwork_preview_worker_m2.
Your working directory is: d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m2
Read d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting work.
Read d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_1\PROJECT.md for overall architecture and contracts.
Read d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m2\DISPATCH.md for your specific mission.

Mission: Milestone M2 (VMM Address Space & ELF64 Loader - Requirement R1)
Files you own exclusively:
- include/kos/vmm.h & mm/vmm.c
- include/kos/elf.h & fs/elf.c

Tasks:
1. In include/kos/vmm.h and mm/vmm.c:
   - Implement uint64_t vmm_create_address_space(void):
     - Allocate physical frame for PML4 via pmm_allocate_frame().
     - Zero lower-half entries 0..255 (user address space).
     - Copy higher-half entries 256..511 from kernel PML4 (vmm.pml4).
     - Return physical address of new PML4.
   - Implement void vmm_destroy_address_space(uint64_t pml4_phys):
     - Recursively traverse lower-half user entries 0..255 (PML4 -> PDPT -> PD -> PT).
     - Free all mapped user physical frames with pmm_free_frame().
     - Free user intermediate table frames (PT, PD, PDPT) with pmm_free_frame().
     - Free the PML4 frame itself with pmm_free_frame().
     - NEVER touch or free entries 256..511 (shared kernel space).
   - Implement bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virtual_address, uint64_t physical_address, uint64_t flags):
     - Map 4 KiB page into the specified PML4 table.
     - Reject user mappings (flags & VMM_PAGE_USER) if virtual_address >= 0x0000800000000000ull.
     - Allocate intermediate tables as needed with PAGE_USER if virtual_address < 0x0000800000000000ull or flags & VMM_PAGE_USER.
2. In include/kos/elf.h and fs/elf.c:
   - Define standard Elf64_Ehdr and Elf64_Phdr structures, and standard ELF constants (EI_*, ELFCLASS64, ELFDATA2LSB, ET_EXEC, EM_X86_64, PT_LOAD, PF_R, PF_W, PF_X).
   - Define struct elf_binary_image { uint64_t entry_point; uint64_t user_stack_top; uint64_t user_rsp; };
   - Implement bool elf_load_binary(const char *path, uint64_t pml4_phys, struct elf_binary_image *out_image):
     - Read binary via vfs_read_file().
     - Strictly validate: magic (0x7f, 'E', 'L', 'F'), 64-bit, little-endian, EM_X86_64 machine, ET_EXEC static type.
     - Validate entry point is in user space (0x00200000 <= entry < 0x0000800000000000ull).
     - Validate all PT_LOAD segments: boundaries within file, virtual addresses strictly < 0x0000800000000000ull, non-overlapping.
     - For each PT_LOAD segment: allocate frames with pmm_allocate_frame(), map with vmm_map_page_in(), copy file content, zero BSS.
     - On any error: clean up allocated frames and return false without panicking the kernel.
   - Implement bool process_setup_user_stack(uint64_t pml4_phys, int argc, char *const argv[], uint64_t *out_user_rsp):
     - Allocate 64 KiB stack ending at USER_STACK_TOP = 0x00007FFFFFFFE000ull.
     - Leave guard page at USER_STACK_TOP - 64 KiB - 4 KiB unmapped (PAGE_PRESENT=0).
     - Format initial stack per System V AMD64 ABI: copy argv strings, envp strings (envp[0]=NULL), auxv (AT_NULL), argv pointers, argc.
     - Align RSP to 16 bytes before entry point.
3. Build verification:
   - Run tools\kos-tool.exe build. Ensure exit code 0, 0 warnings, 0 errors.

