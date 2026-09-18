# Dispatch: Explorer Survey 2 (Partition Parsers & Block Cache Layer)

## Mission
Investigate Partition Table parsing (MBR & GPT), child block device registration, and Block Cache Layer architecture for Milestone K16 & K17 (R2 & R3).

## Authoritative Reference
Read `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` before starting.

## Key Focus Areas
1. **Partition Table Parsers (MBR & GPT)**:
   - MBR Parser: sector 0, signature `0x55AA` at bytes 510-511, 4 partition table entries (offset 446..509). Validating LBA start and sector count against parent disk capacity. Partition types (FAT12, FAT16, FAT32 0x0B/0x0C, Linux 0x83, GPT protective 0xEE).
   - GPT Parser: Protective MBR at LBA 0; GPT Primary Header at LBA 1 ("EFI PART" magic, revision, header size, header CRC32, current LBA 1, backup LBA, first/last usable LBA, disk GUID, partition entries starting LBA, number of partition entries, entry size 128 bytes, partition array CRC32).
   - Backup GPT Header at disk end (backup LBA) fallback.
   - CRC32 calculation algorithm (IEEE 802.3 polynomial 0xEDB88320 or standard table-driven implementation in C).
   - Partition GUID extraction (unique partition GUID and partition type GUID).
2. **Child Block Device Model**:
   - How a partition is exposed as a `block_device_t` slice: translation from partition-relative LBA to parent disk absolute LBA (`parent_lba = partition_start_lba + part_lba`), boundary checking (`part_lba + count <= partition_sector_count`).
   - Naming convention: e.g., `blk0p1`, `blk0p2`, `blk1p1`.
   - Automatic scanning on block device discovery or registration.
3. **Block Cache Layer (R3)**:
   - Specification: 512 sectors / 256 KiB total cache size, 128 hash buckets.
   - Hash function mapping `(device_id, lba)` to bucket index (0..127).
   - Doubly-linked LRU list for eviction of clean buffers.
   - Dirty buffer tracking and writeback flush.
   - Global sync primitive: `bcache_sync()` flushing all dirty blocks across all devices or a specific device.
   - Concurrency & locking: spinlocks or mutexes around hash bucket chains and LRU list.
   - Cache bypass or direct I/O considerations.
4. **Existing Codebase Primitives**:
   - Inspect memory allocators (`kmalloc`, `kfree`), spinlocks (`spinlock_t`), mutexes (`mutex_t`), atomic operations in `include/kos/`.
   - Existing data structures and utility functions.

## Output
Write comprehensive analysis and architecture report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2\analysis.md`
and write a self-contained handoff report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2\handoff.md`
Report back when complete.

## 2026-09-18T07:37:00Z
You are Explorer Survey 2 for Milestone K16 & K17.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2
Read D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md before starting.
Read D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2\DISPATCH.md for your specific instructions.

Mission: Investigate MBR & GPT partition table parsing, child block device registration, and the Block Cache Layer (512 sectors / 256 KiB, 128 hash buckets, LRU replacement, dirty writeback, bcache_sync).
Examine existing codebase data structures, locks, memory allocators in include/kos/ and kernel/.
Write your detailed findings to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2\analysis.md
and a self-contained handoff report to:
D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_2\handoff.md
When done, use send_message to notify your orchestrator parent (conversation ID a0cfd602-3f1b-4ffa-9469-2c19d542b4b0) with a summary and link to handoff.md.
