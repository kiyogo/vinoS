# Dispatch: Explorer M1.2 (AHCI SATA Driver Hardening & DMA Allocator)

## Mission
Design the exact fix for the AHCI SATA Driver (`drivers/ahci.c`, `include/kos/ahci.h`) and DMA memory management for Milestone M1:
1. **DMA Allocator Refactoring**:
   - In `drivers/ahci.c:48-73`, replace `kmalloc` contiguity checks for Command List, Received FIS, Command Tables, and 64 KiB bounce buffer with direct contiguous PMM allocation (`pmm_alloc_pages`) + HHDM virtual mapping (`vmm_physical_to_virtual`).
2. **Command Table Pointer Management**:
   - In `drivers/ahci.c:310-330`, fix the loop where `adev->cmd_table_raw` is overwritten 32 times. Either allocate all 32 command tables as a single contiguous block (e.g. 32 * sizeof(struct ahci_cmd_table) = 32 * 256B or aligned size) or track an array of pointers `adev->cmd_table_raw[32]` for clean lifecycle management.
3. **LBA48 Read/Write Verification**:
   - Verify PRDT building, ATA commands `0x25` (READ DMA EXT), `0x35` (WRITE DMA EXT), and `0xE7` (FLUSH CACHE EXT).

## Inputs
- `D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md`
- Code files: `drivers/ahci.c`, `include/kos/ahci.h`, `include/kos/pmm.h`, `mm/pmm.c`, `include/kos/vmm.h`, `mm/vmm.c`.

## Output
Write analysis to `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\analysis.md` and handoff to `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md`.
Notify parent via `send_message`.

## 2026-09-18T08:05:19Z
<USER_REQUEST>
You are Explorer M1.2 for Milestone M1 (AHCI SATA Driver Hardening & DMA Allocator).
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md for project architecture and contracts.
Read D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\DISPATCH.md for your specific task instructions.

Investigate drivers/ahci.c and include/kos/ahci.h.
Provide the exact code changes and implementation strategy for:
1. DMA buffer allocations for Command List, Received FIS, Command Tables, and 64 KiB bounce buffer using pmm_alloc_pages instead of kmalloc contiguity checks.
2. Command table pointer management so adev->cmd_table_raw is not overwritten and lost in the loop.
3. LBA48 DMA Read/Write and flush verification.

Write your analysis to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\analysis.md
and a self-contained handoff to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md
39: Send a message back via send_message to parent (conversation ID a0cfd602-3f1b-4ffa-9469-2c19d542b4b0) when done.
40: </USER_REQUEST>
41: 
42: ## 2026-09-18T10:08:34Z
43: <USER_REQUEST>
44: You are the M1 User Copy & MSR Explorer (explorer_m1_2) for Milestone K13/K14 in KOS x86_64.
45: Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2
46: The project root is: D:\DATA\source\OS\KOS\OS2
47: The authoritative user request is in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (read section ## Follow-up — 2026-09-18T09:30:36Z).
48: You must read ORIGINAL_REQUEST.md before starting work.
49: Project architecture is documented in: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k13_k14_1\PROJECT.md.
50: Also review the prior technical surveys in:
51: - D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_1\handoff.md
52: - D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_2\handoff.md
53: 
54: Mission:
55: Develop the precise, production-ready specification and implementation design for Safe User Memory Copy and MSR access primitives in Milestone 1 (Requirement R1).
56: 
57: Specific Tasks:
58: 1. Design MSR Primitives:
59:    - In `include/kos/cpu.h` and `arch/x86_64/cpu_control.c`:
60:      `uint64_t cpu_read_msr(uint32_t msr)` and `void cpu_write_msr(uint32_t msr, uint64_t val)`.
61:    - MSR constants: IA32_EFER, IA32_STAR, IA32_LSTAR, IA32_FMASK.
62: 2. Design Safe User Memory Functions (in `include/kos/address_space.h` and `mm/address_space.c`):
63:    - `bool user_memory_validate(const void *user_ptr, size_t len, bool write_access)`:
64:      - Strict boundary check: start and end must be < 0x0000800000000000ull, not null, no integer overflow.
65:      - Page-by-page validation via `vmm_query_page_in()` on current process's PML4: must be present, must have VMM_PAGE_USER, must have VMM_PAGE_WRITABLE if write_access.
66:    - `int copy_from_user(void *dst, const void *src_user, size_t n)`:
67:      - Pre-validates range. Chunked copy via HHDM virtual addresses (`vmm_physical_to_virtual(phys)`). Returns 0 or -14 (-EFAULT).
68:    - `int copy_to_user(void *dst_user, const void *src, size_t n)`:
69:      - Pre-validates writable user memory. Chunked copy via HHDM virtual addresses. Returns 0 or -14 (-EFAULT).
70:    - `char *strndup_from_user(const char *src_user, size_t max_len)`:
71:      - Safely traverses user string up to max_len without causing page fault, copies into newly allocated kmalloc buffer.
72: 3. Write your complete design report to: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md.
73: Update progress.md as you work.
74: When finished, send a message back with your summary and handoff path.
75: </USER_REQUEST>
