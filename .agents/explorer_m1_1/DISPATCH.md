# Dispatch: Explorer M1.1 (VirtIO Block Driver Hardening)

## Mission
Design the exact fix for the VirtIO Block Driver (`drivers/virtio_blk.c`, `include/kos/virtio_blk.h`) for Milestone M1:
1. **Virtqueue Allocation & Alignment**:
   - In `drivers/virtio_blk.c:321-330`, change virtqueue allocation from 4096 bytes (1 page) to 8192 bytes (2 contiguous pages).
   - Set `vdev->desc = (struct vring_desc *)vdev->queue_mem;`
   - Set `vdev->avail = (struct vring_avail *)(vdev->queue_mem + 2048);`
   - Set `vdev->used = (struct vring_used *)(vdev->queue_mem + 4096);` (4096-byte aligned per VirtIO 0.9.5 specification).
2. **DMA Bounce Buffer Allocation**:
   - Replace `kmalloc` contiguity check with direct physical page allocation via `pmm_alloc_pages(16)` (64 KiB) and `vmm_physical_to_virtual(phys)`.
3. **Safe Bounds & Status Checks**:
   - Check `lba + count` bounds and status polling.

## Inputs
- `D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`
- `D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md`
- Code files: `drivers/virtio_blk.c`, `include/kos/virtio_blk.h`, `include/kos/pmm.h`, `mm/pmm.c`, `include/kos/vmm.h`, `mm/vmm.c`.

## Output
Write analysis to `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\analysis.md` and handoff to `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\handoff.md`.
Notify parent via `send_message`.

## 2026-09-18T08:05:19Z
You are Explorer M1.1 for Milestone M1 (VirtIO Block Driver Hardening).
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1\PROJECT.md for project architecture and contracts.
Read D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\DISPATCH.md for your specific task instructions.

Investigate drivers/virtio_blk.c and include/kos/virtio_blk.h.
Provide the exact code changes and implementation strategy for:
1. 2-page virtqueue allocation (8192 bytes) and proper alignment of vdev->used at 4096-byte offset per VirtIO 0.9.5 specification.
2. DMA buffer allocation using pmm_alloc_pages instead of kmalloc contiguity checks.
3. Status polling and bounds checks.

Write your analysis to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\analysis.md
and a self-contained handoff to

## 2026-09-18T10:08:34Z
You are the M1 Address Space Explorer (explorer_m1_1) for Milestone K13/K14 in KOS x86_64.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1
The project root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (read section ## Follow-up — 2026-09-18T09:30:36Z).
You must read ORIGINAL_REQUEST.md before starting work.
Project architecture is documented in: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k13_k14_1\PROJECT.md.
Also review the prior technical survey in: D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_1\handoff.md.

Mission:
Develop the precise, production-ready specification and implementation design for Milestone 1 (M1): Per-Process Isolated Address Spaces & Memory Safety (Requirement R1).

Specific Tasks:
1. Design `include/kos/address_space.h`:
   - `struct vm_area`: start_va, end_va, flags (VMA_READ, VMA_WRITE, VMA_EXEC, VMA_STACK, VMA_HEAP), name, list node.
   - `struct address_space`: pml4_phys, pml4_virt, spinlock, refcount, vma_list, page_count.
   - Function prototypes: as_create, as_ref, as_unref, as_destroy, as_map_region, as_unmap_region, as_find_vma.
2. Design `mm/address_space.c`:
   - Allocation and initialization of `struct address_space` via kmalloc.
   - Creation of new PML4 via `vmm_create_address_space()`.
   - VMA insertion, lookup, and removal logic.
   - Destruction via `vmm_destroy_address_space()` and freeing all VMAs when refcount reaches 0.
   - Ensure 100% PMM frame reclamation with 0 frame leaks.
3. Write your complete design report to: D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_1\handoff.md.
Update progress.md as you work.
When finished, send a message back with your summary and handoff path.
