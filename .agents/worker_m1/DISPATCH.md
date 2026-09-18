# Dispatch: Worker M1 (Universal Block Device Abstraction & Hardware Drivers)

## Mission
Implement Milestone M1: Universal Block Device Abstraction & Hardware Drivers for KOS x86_64:

### 1. Universal Block Device & DMA Helpers
- In `include/kos/block.h` and `drivers/block.c`:
  - Declare and implement `block_alloc_dma_pages` and `block_free_dma_pages` utilizing `pmm_alloc_pages` and `vmm_physical_to_virtual`.
  - Apply 64-bit overflow-safe bounds checking across `block_read`, `block_write`, `partition_read_blocks`, `partition_write_blocks`, and `block_partition_register`:
    `count == 0 || lba > dev->total_blocks || count > dev->total_blocks - lba`.

### 2. VirtIO Block Driver Hardening
- In `include/kos/virtio_blk.h` and `drivers/virtio_blk.c`:
  - Implement 2-page virtqueue allocation (8192 bytes) via `pmm_alloc_pages(2)` / `vmm_physical_to_virtual`.
  - Align `vdev->used` to 4096-byte offset: `vdev->desc = queue_mem`, `vdev->avail = queue_mem + 2048`, `vdev->used = queue_mem + 4096`.
  - Implement direct contiguous PMM DMA allocation for 1-page request header and 16-page (64 KiB) bounce buffer.
  - Implement LBA bounds checking and timeout spin fallback in `virtio_blk_submit_io`.

### 3. AHCI SATA Driver Hardening
- In `include/kos/ahci.h` and `drivers/ahci.c`:
  - Replace `ahci_alloc_dma` heap allocations with `pmm_alloc_pages` and `vmm_physical_to_virtual` for Command List (1 page), Received FIS (1 page), Command Tables (2 pages total for all 32 tables), and Bounce Buffer (16 pages).
  - Fix Command Table pointer management: allocate all 32 tables as a single 2-page buffer, saving the base pointer in `adev->cmd_table_raw` once (no overwrite in loop).
  - Verify LBA48 Read/Write/Flush and full cleanup in `ahci_destroy`.

### 4. RAM Disk Hardening & Block Test Expansion
- In `drivers/ramdisk.c`:
  - Validate `block_size` is power-of-two >= 512, ensure zero-initialized buffer on create, overflow-safe bounds checking.
- In `kernel/command.c`:
  - Update `command_blocktest` with comprehensive tests: DMA allocation/alignment/zero-init, RAM disk parameter rejection, 64-bit overflow rejection, write/read pattern verification, flush, and clean destruction.

### 5. Build and Execution Verification
- Run `tools\kos-tool.exe build` and verify 0 warnings, 0 errors (-Wall -Wextra -Werror).
- Run `tools\kos-tool.exe run` in QEMU and verify `command_blocktest` passes cleanly on serial output.

## Exclusive File Ownership
- `include/kos/block.h`
- `drivers/block.c`
- `include/kos/ramdisk.h`
- `drivers/ramdisk.c`
- `include/kos/virtio_blk.h`
- `drivers/virtio_blk.c`
- `include/kos/ahci.h`
- `drivers/ahci.c`
- `kernel/command.c`

## Reference Reports
Read these reports carefully before writing code:
- `D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\handoff.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\handoff.md`

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Output
Write your implementation report to `D:\DATA\source\OS\KOS\OS2\.agents\worker_m1\handoff.md`.
Notify parent via `send_message` when done.

## 2026-09-18T08:26:47Z
You are Worker M1 for Milestone M1 (Universal Block Device Abstraction & Hardware Drivers).
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\worker_m1
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md for architectural context.
Read D:\DATA\source\OS\KOS\OS2\.agents\worker_m1\DISPATCH.md for your specific mission.
Read the findings and code diffs in:
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\handoff.md
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_2\handoff.md
- D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3\handoff.md

