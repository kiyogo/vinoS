# Dispatch: Explorer Survey 2 (VMM, Paging, ELF64 Loader, User Stack & Address Space)

## Mission
Investigate KOS x86_64 VMM paging, address space management, ELF64 binary parsing and loading, and user stack configuration for Milestone K14.

## Key Focus Areas
1. VMM & Address Space: Check `kernel/vmm.c`, `include/kos/vmm.h`, etc. How are page tables (PML4) created and cloned? Is there an existing kernel PML4 vs per-process PML4? How is higher-half kernel space mapped across all address spaces?
2. User Page Permissions: Are user-accessible page flags (`PTE_USER` / bit 2) supported in `vmm_map_page` or similar functions? What about Read/Write (`PTE_WRITE`) and Execute Disable (`PTE_NX`)?
3. ELF64 Loader: Is there an existing ELF parser or loader? If not, what headers/structures (`elf.h`, `Elf64_Ehdr`, `Elf64_Phdr`, `PT_LOAD`, `PT_INTERP`, etc.) are needed? What validation checks are required for headers, boundaries, alignments, entry points?
4. User Stack & Guard Pages: How should user stacks be allocated (virtual address range, size e.g. 2MB or 64KB)? How is the non-present guard page allocated at the bottom of the user stack?
5. Initial User Stack Layout: System V AMD64 ABI initial stack format (`argc`, `argv`, `envp`, strings, 16-byte alignment before call).
6. Binary Storage: Where are ELF binaries stored or loaded from in KOS (e.g. RAMFS, bootloader modules, initramfs)?
7. Recommendations and required changes for R1.

## Output
Write comprehensive analysis and findings to `.agents/teamwork_preview_explorer_survey_2/analysis.md` and a self-contained handoff report to `.agents/teamwork_preview_explorer_survey_2/handoff.md`.
Report back when complete.
