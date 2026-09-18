# Dispatch: Explorer M1.3 (Universal Block Device Abstraction & RAM Disk Validation)

## 2026-09-18T08:05:19Z
You are Explorer M1.3 for Milestone M1 (Universal Block Device Abstraction & RAM Disk Validation).
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md for project architecture and contracts.
Read D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\DISPATCH.md for your specific task instructions.

Investigate drivers/block.c, include/kos/block.h, drivers/ramdisk.c, include/kos/ramdisk.h, and kernel/command.c (blocktest).
Provide the exact code changes and implementation strategy for:
1. Universal DMA allocation helpers (block_alloc_dma_pages, block_free_dma_pages) in drivers/block.c & include/kos/block.h.
2. 64-bit overflow-safe bounds checking in block_read, block_write, partition_read_blocks, partition_write_blocks.
3. RAM disk initialization validation and blocktest test plan.

Write your analysis to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\analysis.md
and a self-contained handoff to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\handoff.md
Send a message back via send_message to parent (conversation ID a0cfd602-3f1b-4ffa-9469-2c19d542b4b0) when done.

## Mission Details
1. **Universal DMA Allocation Helper**:
   - Consider adding a common DMA allocation function in `drivers/block.c` and `include/kos/block.h`:
     ```c
     bool block_alloc_dma_pages(uint32_t count, void **out_virt, uint64_t *out_phys);
     void block_free_dma_pages(void *virt, uint64_t phys, uint32_t count);
     ```
     that encapsulates `pmm_alloc_pages` and `vmm_physical_to_virtual`.
2. **Overflow-Safe Bounds Checking**:
   - Check `drivers/block.c` functions `block_read`, `block_write`, `partition_read_blocks`, `partition_write_blocks` for potential 64-bit overflow:
     Replace `lba + count > dev->total_blocks` with:
     `count == 0 || lba > dev->total_blocks || count > dev->total_blocks - lba`.
3. **RAM Disk Hardening**:
   - Verify `ramdisk_create` ensures zero-initialized memory buffer (`kmemset(buffer, 0, size_bytes)`), validates non-zero size and block size power-of-two.
4. **Verification Plan**:
   - Inspect `blocktest` in `kernel/command.c`. Verify that `blocktest` passes completely in QEMU.

## 2026-09-18T10:08:34Z
You are the M1 Process Integration Explorer (explorer_m1_3) for Milestone K13/K14 in KOS x86_64.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3
The project root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (read section ## Follow-up — 2026-09-18T09:30:36Z).
You must read ORIGINAL_REQUEST.md before starting work.
Project architecture is documented in: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k13_k14_1\PROJECT.md.
Also review the prior technical surveys in:
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_1\handoff.md
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_3\handoff.md

Mission:
Develop the precise integration plan for `struct process` lifecycle, build toolchain updates, and automated self-tests for Milestone 1.

Specific Tasks:
1. Design `struct process` integration in `include/kos/process.h` and `kernel/process.c`:
   - Add `struct address_space *as` to `struct process`.
   - Update `process_create()`: initialize `proc->as`.
   - Update `process_exec()`: use `as_create()` or map into `proc->as`, synchronize `proc->pml4_physical = proc->as->pml4_phys`.
   - Update `process_destroy()`: clean unref `as_unref(proc->as)`.
2. Design Build System Updates in `tools/kos.c`:
   - Add `mm/address_space.c` to `c_sources[]` in `tools/kos.c`.
   - Recompile `tools/kos-tool.exe` from `tools/kos.c` with host Clang compiler.
   - Verify `tools\kos-tool.exe build` succeeds with zero warnings and zero errors.
3. Design M1 Automated Self-Tests:
   - Self-test function `address_space_self_test()` verifying:
     - `as_create()` and `as_destroy()` maintain `pmm_free_frame_count()` before == after.
     - `copy_from_user` and `copy_to_user` properly reject NULL, kernel addresses, out-of-bounds addresses, and successfully copy mapped user memory.
     - `strndup_from_user` handles valid strings and rejects bad addresses.
4. Write your complete design report to: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\handoff.md.
Update progress.md as you work.
When finished, send a message back with your summary and handoff path.
