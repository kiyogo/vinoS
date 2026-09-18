# Dispatch: Explorer Survey 1 (Block Device Abstraction & Hardware Drivers)

## Mission
Investigate KOS x86_64 storage architecture, PCI subsystem, memory mapping, and hardware drivers for Milestone K16 & K17 (R1).

## Authoritative Reference
Read `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` before starting.

## Key Focus Areas
1. **Existing Drivers & PCI**:
   - Inspect `drivers/`, `include/kos/`, `drivers/pci.c`, `include/kos/pci.h`. How are PCI devices scanned and enumerated?
   - How are MMIO regions mapped into virtual address space (e.g., `vmm_map_page`, `vmm_map_mmio`, or direct physical map)?
   - How are physical DMA buffers allocated (`pmm_alloc_page`, contiguous page allocation)?
2. **Universal Block Device Abstraction**:
   - What block device structures exist (or need to be designed) in `include/kos/block.h` or `drivers/block/`?
   - Define interface operations: `read_blocks`, `write_blocks`, `flush`, `block_size` (usually 512), `capacity` (in sectors or bytes).
   - Independent device registry (by ID or object pointer, e.g. `blk0`, `blk1`, independent of drive letters).
3. **RAM Block Device (`ramdisk`)**:
   - Design and placement: memory-backed block device for isolated tests and benchmarking.
4. **VirtIO Block Driver (QEMU `0x1AF4:0x1001`)**:
   - VirtIO legacy/modern PCI configuration, I/O ports or MMIO BARs.
   - Virtqueue structure (vring_desc, vring_avail, vring_used), request headers (`VIRTIO_BLK_T_IN`, `VIRTIO_BLK_T_OUT`), DMA bounce buffer, status polling/timeouts.
5. **AHCI SATA Driver**:
   - PCI class `0x01` (Mass Storage), subclass `0x06` (SATA).
   - ABAR (BAR5) MMIO mapping, HBA Global Host Control (`ghc`), port discovery (`pi` bitmask).
   - Port initialization, Command List, Received FIS, Command Tables, PRDT (Physical Region Descriptor Table).
   - ATA commands: `0xEC` (IDENTIFY DEVICE), `0x25` (READ DMA EXT LBA48), `0x35` (WRITE DMA EXT LBA48).
   - Error detection, command issue/polling, timeout handling.
6. **Build System & Files**:
   - Check `tools/kos-tool.exe` or Makefile/build scripts to see how new source files are compiled and linked.

## Output
Write comprehensive analysis and architecture report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1\analysis.md`
and write a self-contained handoff report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1\handoff.md`
Report back when complete.

## 2026-09-18T07:36:56Z
You are Explorer Survey 1 for Milestone K16 & K17.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1\DISPATCH.md for your specific instructions.

Mission: Investigate Block Device Abstraction, RAM disk, VirtIO Block, AHCI SATA, PCI subsystem, DMA buffer allocation, and MMIO mapping in KOS.
Examine existing codebase files in drivers/, include/kos/, arch/, mm/, etc.
Write your detailed findings to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1\analysis.md
and a self-contained handoff report to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1\handoff.md
When done, use send_message to notify your orchestrator parent (conversation ID a0cfd602-3f1b-4ffa-9469-2c19d542b4b0) with a summary and link to handoff.md.

