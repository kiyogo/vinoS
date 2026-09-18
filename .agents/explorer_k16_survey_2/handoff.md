# Handoff Report: Explorer Survey 2 (Milestone K16 & K17)

**Task**: Investigation of Partition Table Parsers (MBR/GPT), Child Block Devices, and Block Cache Layer (R2 & R3)  
**Agent**: Explorer Survey 2 (`.agents/explorer_k16_survey_2`)  
**Handoff Type**: Hard (Investigation Completed)  
**Date**: 2026-09-18  

---

## 1. Observation

### 1.1 Automated Build Verification
- Command: `.\tools\kos-tool.exe build` executed in `D:\DATA\source\OS\KOS\OS2`.
- Result: Exit code 0, cleanly built kernel image `build/kos.elf` with 791 symbols and staged UEFI ESP directory `build/esp`.
- Compiler flags: Clang 17, `-std=c17 -ffreestanding -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`.

### 1.2 Existing Files & Implementation Status
1. **Partition Table Header & Implementation**:
   - `include/kos/partition.h` (83 lines): Defines `MBR_BOOT_SIGNATURE` (0xAA55), partition type codes (`MBR_TYPE_FAT32_LBA 0x0C`, `MBR_GPT_PROTECTIVE_TYPE 0xEE`), `struct mbr_partition_entry` (16 bytes packed), `struct mbr_sector` (512 bytes), `struct gpt_header` (92 bytes packed), `struct gpt_partition_entry` (128 bytes packed), `partition_crc32()`, `partition_scan()`, `partition_format_guid()`.
   - `fs/partition.c` (223 lines):
     - `partition_crc32()` (lines 12-26): IEEE 802.3 CRC32 bitwise implementation (`0xEDB88320`).
     - `partition_format_guid()` (lines 30-64): Formats mixed-endian UEFI GUID into 36-char string.
     - `partition_scan_gpt()` (lines 74-168): Validates `"EFI PART"` signature at LBA 1, computes header CRC32, allocates buffer for partition entries, checks partition array CRC32, registers child devices via `block_partition_register()`.
     - `partition_scan_mbr()` (lines 171-213): Reads sector 0, validates `0xAA55` signature, checks for protective MBR (type `0xEE`), parses 4 entries, verifies bounds, registers child devices.
2. **Block Device & Partition Abstraction**:
   - `include/kos/block.h` (96 lines) & `drivers/block.c` (371 lines):
     - Defines `struct block_device` with `parent`, `start_lba`, `partition_index`, `partition_type_mbr`, `partition_guid`.
     - Static array `block_devices[BLOCK_MAX_DEVICES]` (64 devices max), protected by `block_lock` spinlock.
     - `partition_read_blocks()` / `partition_write_blocks()` (lines 40-62): Translates partition LBA to parent LBA (`dev->start_lba + lba`) and verifies `lba + count <= dev->total_blocks`.
     - `block_partition_register()` (lines 155-230): Formats name e.g. `blk0p1` and registers device.
     - `block_device_unregister()` (lines 232-261): Marks offline, cleans list nodes, unregisters children.
3. **Block Cache Layer**:
   - `include/kos/block_cache.h` (34 lines) & `fs/block_cache.c` (300 lines):
     - `#define BLOCK_CACHE_CAPACITY 512` (512 sectors = 256 KiB cache).
     - `#define BLOCK_CACHE_HASH_SIZE 128` (128 hash buckets).
     - `cache_hash()` (lines 36-45): Thomas Wang 64-bit integer mix mapping `(dev_id, lba)` to 0..127.
     - `struct cache_entry`: Contains `dev`, `lba`, `data[512]`, `valid`, `dirty`, `last_access`, `lru_node`, `hash_node`.
     - `block_cache_read()` / `block_cache_write()`: Protected by `spinlock_lock_irqsave(&g_cache.lock)`.
     - `block_cache_flush_device()` (lines 245-266): Flushes dirty entries to device.
     - `block_cache_sync()` (lines 268-270): Flushes all dirty entries across all devices.
     - `block_cache_invalidate_device()` (lines 272-292): Evicts entries for a device.
4. **Kernel Diagnostics & Automated Tests**:
   - `kernel/command.c`:
     - `command_mbrtest()` (lines 4072-4155): Tests MBR creation, parsing, 2 partition detection, and LBA offset translation on 4 MiB ramdisk.
     - `command_gpttest()` (lines 4158-4251): Tests protective MBR, GPT header, CRC32 header and table, GUID extraction on 8 MiB ramdisk.
     - `command_cachetest()` (lines 4254-4330): Tests 64 sector write/read, hit/miss stats, flush, and direct disk verification on 2 MiB ramdisk.
     - `command_fdisk()` (lines 3776-3806): Displays block devices and partition hierarchy.
   - `kernel/main.c`: Storage initialization at boot (lines 345-367) and test suite execution (lines 439-444).

---

## 2. Logic Chain

### 2.1 Partition Parsers (MBR & GPT)
1. **Observation**: In `fs/partition.c:80-110`, `partition_scan_gpt()` only reads LBA 1. If reading LBA 1 fails or `calc_crc != orig_crc`, it immediately returns 0 without inspecting `dev->total_blocks - 1` (the standard GPT backup header location).
2. **Logic Step**: According to UEFI Specification 2.10 (Section 5.3.1) and DISPATCH.md Key Focus Area 1 ("Backup GPT Header at disk end (backup LBA) fallback"), disks with corrupted primary headers must attempt recovery from the backup GPT header at `total_blocks - 1`. Failing to do so causes avoidable boot/mount failures when primary headers experience single-sector corruption.
3. **Observation**: In `fs/partition.c:122-144`, if `hdr->num_partition_entries > 128`, the code clamps `num_entries = 128`. Then it reads `entries_bytes = 128 * entry_size` and computes `partition_crc32(entries_buf, entries_bytes)`.
4. **Logic Step**: In UEFI GPT, `hdr->partition_array_crc32` is computed over ALL entries (`hdr->num_partition_entries * hdr->size_partition_entry`). Computing CRC on only the first 128 entries when the disk has more entries will always result in a false CRC mismatch warning (`0x... != 0x...`). The parser must read the full array to verify CRC32, and then parse supported entries.
5. **Observation**: In `fs/partition.c:162-163` (GPT) and lines 205-206 (MBR), partitions are numbered sequentially using `valid_parts++` rather than their slot index (`i + 1`).
6. **Logic Step**: If partition slot 1 is unallocated but partition slot 2 is active, the child device is named `p1` instead of `p2`. Standard operating system conventions (Linux, Windows, BSD) maintain stable partition numbers matching table entry slots.

### 2.2 Child Block Devices
1. **Observation**: In `drivers/block.c:44, 56`, bounds checks use `lba + count > dev->total_blocks`.
2. **Logic Step**: If `lba + count` overflows a 64-bit integer, this check can be bypassed. Safe bounds checking must be structured as `count == 0 || lba > dev->total_blocks || count > dev->total_blocks - lba`.
3. **Observation**: In `drivers/block.c:176-179`, partition name formatting does `part_name[plen++] = (char)('0' + (part_index / 10))` and `(char)('0' + (part_index % 10))`.
4. **Logic Step**: If `part_index >= 100` (GPT supports up to 128 partitions), this produces non-digit ASCII characters (e.g. `100 / 10 = 10` -> `':'`).
5. **Observation**: In `drivers/block.c:232-261` (`block_device_unregister`), devices and child partitions are removed from the list and destroyed, but `block_cache_invalidate_device()` is never called.
6. **Logic Step**: If dirty or cached blocks exist in `g_cache`, their `entry->dev` pointer becomes dangling. During subsequent LRU eviction, writing back a dirty block will invoke `block_write()` through a dangling pointer, leading to kernel panic or heap corruption.

### 2.3 Block Cache Layer
1. **Observation**: In `fs/block_cache.c:162-203`, `block_cache_read()` holds `spinlock_lock_irqsave(&g_cache.lock)` while calling `block_read(dev, cur_lba, 1, entry->data)`. In `cache_allocate_slot_locked()` (lines 118-120), dirty writeback calls `block_write()`.
2. **Observation**: In `drivers/virtio_blk.c:188-199` and `drivers/ahci.c:240`, hardware drivers poll waiting for DMA completion using `while (timer_get_ticks() < timeout_tick)`. In `drivers/timer.c:71-75`, `timer_get_ticks()` is incremented exclusively inside `timer_interrupt()` (PIT IRQ0).
3. **Logic Step**: `spinlock_lock_irqsave` disables CPU interrupts (`cli`). When interrupts are disabled, PIT IRQ0 cannot fire, and `timer_get_ticks()` NEVER increments. If hardware takes more than an instant to respond, the timeout loop can freeze or wait indefinitely. Furthermore, holding a global spinlock with interrupts disabled during disk I/O introduces severe interrupt latency across the operating system.
4. **Observation**: In `fs/block_cache.c:36-45`, `cache_hash()` uses `((uint64_t)dev_id << 48) ^ lba`.
5. **Logic Step**: Child partitions have a unique `dev->id` distinct from their parent disk. Accessing physical sector $S$ through `blk0p1` at partition LBA 0 hashes to a different bucket than accessing sector $S$ through `blk0` at parent LBA 2048. This creates cache aliasing where writes to the partition are not immediately visible on the parent disk.
6. **Observation**: In `fs/block_cache.c:262-264`, `block_flush(dev)` is only called if `dev != NULL`.
7. **Logic Step**: In `block_cache_sync()` (called during system shutdown, unmount, or `sync` command), `dev` is `NULL`. Dirty blocks are written to the controllers, but the underlying hardware controller write caches (`ATA_CMD_FLUSH_CACHE_EX`, `VIRTIO_BLK_T_FLUSH`) are never triggered.

---

## 3. Caveats

1. **No Hardware Storage Controller Tested in Survey Turn**:
   - QEMU smoke tests in this survey utilized memory-backed block devices (`ramdisk`) and the existing boot diagnostic commands. Physical SATA AHCI controllers with real magnetic/SSD drives were not physically tested, though driver code was statically verified.
2. **Extended Partition (EBR) Slicing**:
   - MBR Extended Partition tables (types `0x05`, `0x0F`) containing linked-list EBRs (logical partitions) are not supported by the current 4-slot MBR parser. Only the 4 primary slots are parsed.
3. **Dynamic Buffer Size in GPT Array Reading**:
   - Current GPT partition array allocation uses `kmalloc(entries_sectors * dev->block_size)` where `entries_sectors` is typically 32 (16 KiB). This assumes heap availability of $\ge 16$ KiB during boot, which is verified safe by `mm_heap` capacity (> 32 MiB).

---

## 4. Conclusion

1. **Compliance with R2 & R3**: The core functionality for Milestone K16 & K17 requirements R2 (MBR/GPT Parsing, Child Slicing) and R3 (Block Cache, LRU, Dirty Tracking, Sync) is present, well-structured, and compiling cleanly.
2. **Required Hardening for Workers**:
   - Implement Backup GPT Header fallback at `dev->total_blocks - 1` in `fs/partition.c`.
   - Normalize cache device and LBA lookups to root hardware device in `fs/block_cache.c` to prevent cache aliasing.
   - Decouple hardware I/O from the interrupt-disabled spinlock in `fs/block_cache.c` to avoid freezing timer ticks during disk polling.
   - Automatically invalidate cache entries when unregistering block devices in `drivers/block.c`.
   - Invoke hardware controller flush across all online devices in `block_cache_sync()`.

---

## 5. Verification Method

To independently reproduce and verify all findings:
1. **Compilation Check**:
   ```powershell
   cd D:\DATA\source\OS\KOS\OS2
   .\tools\kos-tool.exe build
   ```
   *Expected result*: Build completes with 0 errors, 0 warnings.
2. **Inspection of Key Code Sections**:
   - GPT Header and lack of backup fallback: inspect `fs/partition.c` lines 74-115.
   - Block cache spinlock and I/O execution: inspect `fs/block_cache.c` lines 162-203.
   - Timer tick dependency in driver polling: inspect `drivers/timer.c` lines 71-79 and `drivers/virtio_blk.c` lines 188-199.
   - Cache unregistration leak: inspect `drivers/block.c` lines 232-261.
3. **Execution of Automated Test Suite in QEMU**:
   - Run KOS in QEMU and observe serial output for:
     - `blocktest`: RAM disk allocation, read, write, boundary check.
     - `mbrtest`: MBR signature check, partition detection, LBA offset translation.
     - `gpttest`: Protective MBR, GPT header CRC32, table CRC32, GUID verification.
     - `cachetest`: Cache hit/miss ratio, dirty writeback, disk flush verification.
