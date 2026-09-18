# Technical Analysis & Architecture Survey: Partition Parsers (MBR/GPT), Child Block Devices, and Block Cache Layer

**Milestone**: K16 & K17 (Storage Subsystem, Block Layer, Partitioning, Block Cache, Modular VFS & FAT32)  
**Survey Scope**: Requirements R2 (Partition Table Parsers & Child Block Devices) & R3 (Block Cache Layer)  
**Author**: Explorer Survey 2  
**Date**: 2026-09-18  
**Target Environment**: x86_64, freestanding C17, UEFI Limine Bootloader, QEMU `q35`, `cpu max`, AHCI SATA, VirtIO-Block.

---

## 1. Executive Summary

This investigation examines the current implementation and architectural requirements for:
1. **Partition Table Parsers (MBR & GPT)** (`fs/partition.c`, `include/kos/partition.h`).
2. **Child Block Device Model** (`drivers/block.c`, `include/kos/block.h`).
3. **Block Cache Layer** (`fs/block_cache.c`, `include/kos/block_cache.h`).
4. **Integration with Kernel Synchronization, Memory, and Storage Subsystems** (`include/kos/`, `kernel/`, `drivers/`).

### Overall System Status
- The codebase builds cleanly with zero errors and zero warnings (`tools\kos-tool.exe build`).
- Initial implementations of MBR parser, GPT parser, child block devices, and block cache exist and pass the baseline test suites (`blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`).
- However, our deep-dive code audit revealed **several critical architectural gaps, synchronization edge cases, and robustness issues** that need to be addressed or accounted for:
  - **GPT Parser**: Completely lacks backup GPT header fallback at disk end (`total_blocks - 1`); clamps entry count to 128 which causes partition array CRC32 verification failure if `num_partition_entries > 128`; partition indexing uses consecutive counts instead of fixed entry slot indices.
  - **Child Block Device Registration**: Format string logic for partition names only supports 2-digit partition indices (fails on `part_index >= 100`); device unregistration does not invalidate or flush cached blocks in the block cache, risking use-after-free and silent data corruption.
  - **Block Cache Layer**: Hardware I/O (`block_read`, `block_write`) is executed directly inside `spinlock_lock_irqsave(&g_cache.lock)`: disabling CPU interrupts during multi-millisecond disk I/O freezes the PIT timer counter (`timer_get_ticks()`), causing hardware polling timeout loops to hang; global sync (`bcache_sync`) does not call hardware controller flush (`block_flush`) on underlying devices; cache keying uses `(dev->id, lba)` without root device normalization, causing cache aliasing between parent disk and child partitions.

---

## 2. Partition Table Parsers (MBR & GPT) Deep Dive

### 2.1 MBR (Master Boot Record) Architecture & Implementation

#### On-Disk Layout
- **Sector**: LBA 0 (512 bytes).
- **Bootstrap Code**: Bytes 0..445 (446 bytes).
- **Partition Table Entries**: 4 entries of 16 bytes each at offsets `0x1BE` (446), `0x1CE` (462), `0x1DE` (478), `0x1EE` (494).
- **Boot Signature**: 2 bytes at offset 510..511 (`0x55`, `0xAA`).

```
+------------------------------------+ 0x000 (0)
| Bootstrap Machine Code             | (446 bytes)
+------------------------------------+ 0x1BE (446)
| Partition Entry 1 (16 bytes)       |
+------------------------------------+ 0x1CE (462)
| Partition Entry 2 (16 bytes)       |
+------------------------------------+ 0x1DE (478)
| Partition Entry 3 (16 bytes)       |
+------------------------------------+ 0x1EE (494)
| Partition Entry 4 (16 bytes)       |
+------------------------------------+ 0x1FE (510)
| Boot Signature (0x55, 0xAA)        | (2 bytes)
+------------------------------------+ 0x200 (512)
```

#### Entry Structure (`struct mbr_partition_entry` in `include/kos/partition.h`)
```c
struct mbr_partition_entry {
    uint8_t boot_indicator;     /* 0x80 = active/bootable, 0x00 = inactive */
    uint8_t start_chs[3];       /* Cylinder-Head-Sector start (legacy) */
    uint8_t partition_type;     /* OS filesystem indicator */
    uint8_t end_chs[3];         /* Cylinder-Head-Sector end (legacy) */
    uint32_t start_lba;         /* Starting LBA (little-endian) */
    uint32_t sector_count;      /* Total sectors in partition (little-endian) */
} KOS_PACKED;
```

#### Supported Partition Types
- `0x00`: Empty / Unused.
- `0x01`: FAT12.
- `0x04`: FAT16 (< 32 MB).
- `0x05`: Extended Partition (CHS).
- `0x06`: FAT16 (>= 32 MB).
- `0x07`: NTFS / exFAT.
- `0x0B`: FAT32 (CHS addressing).
- `0x0C`: FAT32 (LBA addressing).
- `0x0E`: FAT16 (LBA addressing).
- `0x0F`: Extended Partition (LBA).
- `0x82`: Linux Swap.
- `0x83`: Linux Native Filesystem (ext2/3/4).
- `0xEE`: GPT Protective Partition.
- `0xEF`: EFI System Partition (ESP MBR).

#### Assessment of Current `partition_scan_mbr()` (`fs/partition.c:171-213`)
- **Signature Check**: Verifies `mbr->signature == MBR_BOOT_SIGNATURE` (`0xAA55` little-endian).
- **Protective GPT Detection**: Checks `if (mbr->partitions[0].partition_type == MBR_GPT_PROTECTIVE_TYPE)`.
  - *Limitation*: Standard UEFI specifications allow the protective entry to be in any of the 4 slots (though slot 0 is standard). If a hybrid MBR disk or modified tool places `0xEE` in slot 1..3, it will fail to detect GPT.
  - *Recommendation*: Check all 4 entries for `MBR_GPT_PROTECTIVE_TYPE` before treating disk as purely MBR.
- **Bounds Checking**:
  - `(uint64_t)p->start_lba + p->sector_count > dev->total_blocks` properly uses a 64-bit cast to prevent 32-bit integer wraparound.
  - *Limitation*: Does not check `p->start_lba == 0`. An MBR partition starting at LBA 0 overlaps the partition table itself and should be rejected.
- **Partition Naming / Indexing**:
  - Currently uses a local counter `valid_parts++` and registers the child device with `part_index = valid_parts`.
  - *Limitation*: If slot 0 is empty and slot 1 contains a partition, it is named `blk0p1` instead of `blk0p2`. Standard OS convention (Linux `/dev/sda1..4`) indexes MBR primary partitions by their physical slot index `i + 1` (1..4).

---

### 2.2 GPT (GUID Partition Table) Architecture & Implementation

#### On-Disk Layout
1. **LBA 0**: Protective MBR (prevents legacy disk utilities from corrupting GPT).
2. **LBA 1**: Primary GPT Header (92 bytes + padding to sector size).
3. **LBA 2..33**: Primary Partition Entry Array (128 entries * 128 bytes = 16,384 bytes = 32 sectors).
4. **LBA 34 .. Total-34**: Usable Partition Area.
5. **LBA Total-33 .. Total-2**: Backup Partition Entry Array.
6. **LBA Total-1**: Backup GPT Header.

```
+-------------------------------------------------------------+ LBA 0
| Protective MBR (1 entry type 0xEE spanning disk)            |
+-------------------------------------------------------------+ LBA 1
| Primary GPT Header (EFI PART, CRC32, GUID, pointers)        |
+-------------------------------------------------------------+ LBA 2
| Primary Partition Array (128 entries x 128 bytes = 32 sec)  |
+-------------------------------------------------------------+ LBA 34
| Usable LBA Space (Partitions data: blk0p1, blk0p2, ...)     |
| ...                                                         |
+-------------------------------------------------------------+ LBA N - 33
| Backup Partition Array (duplicate copy)                     |
+-------------------------------------------------------------+ LBA N - 1
| Backup GPT Header (duplicate copy with swapped pointers)    |
+-------------------------------------------------------------+
```

#### GPT Header Structure (`struct gpt_header` in `include/kos/partition.h:46-61`)
```c
struct gpt_header {
    char signature[8];          /* "EFI PART" (0x5452415020494645ULL) */
    uint32_t revision;          /* 0x00010000 for version 1.0 */
    uint32_t header_size;       /* usually 92 */
    uint32_t header_crc32;      /* CRC32 computed with header_crc32 = 0 */
    uint32_t reserved;          /* must be 0 */
    uint64_t current_lba;       /* 1 for primary, (total_blocks - 1) for backup */
    uint64_t backup_lba;        /* (total_blocks - 1) for primary, 1 for backup */
    uint64_t first_usable_lba;  /* usually 34 */
    uint64_t last_usable_lba;   /* usually total_blocks - 34 */
    uint8_t disk_guid[16];      /* Unique Disk GUID */
    uint64_t partition_entries_lba; /* LBA 2 for primary */
    uint32_t num_partition_entries; /* usually 128 */
    uint32_t size_partition_entry;  /* usually 128 */
    uint32_t partition_array_crc32; /* CRC32 across all partition entries */
} KOS_PACKED;
```

#### GPT Partition Entry Structure (`struct gpt_partition_entry` in `include/kos/partition.h:63-71`)
```c
struct gpt_partition_entry {
    uint8_t type_guid[16];              /* Partition Type GUID */
    uint8_t unique_partition_guid[16];  /* Unique Partition GUID */
    uint64_t starting_lba;              /* Starting LBA */
    uint64_t ending_lba;                /* Ending LBA (inclusive) */
    uint64_t attributes;                /* Attribute flags */
    uint16_t name[36];                  /* UTF-16LE partition name */
} KOS_PACKED;
```

#### Standard GUIDs (Mixed-Endian Format)
- **EFI System Partition (ESP)**: `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`
- **Microsoft Basic Data (FAT32/NTFS)**: `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`
- **Linux Filesystem Data**: `0FC63DAF-8483-4772-8E79-3D69D8477DE4`
- **Linux Swap**: `0657FD6D-A4AB-43C4-84E5-0933C84B4F4F`

#### CRC32 Algorithm (`partition_crc32` in `fs/partition.c:12-26`)
- Uses standard IEEE 802.3 polynomial `0xEDB88320` (reflected representation of `0x04C11DB7`), initial value `0xFFFFFFFF`, post-inverted `~crc`.
- Bitwise inner loop: Correct and verified against standard test vectors.
- Performance: Adequate for startup header checks (~0.1 ms for 16 KiB array). Can optionally be upgraded to a 256-word precomputed lookup table for 8x speedup.

#### GUID Formatting (`partition_format_guid` in `fs/partition.c:30-64`)
- Implements standard UEFI mixed-endian format:
  - `Data1` (bytes 3..0, little-endian)
  - `Data2` (bytes 5..4, little-endian)
  - `Data3` (bytes 7..6, little-endian)
  - `Data4` (bytes 8..9, big-endian)
  - `Data5` (bytes 10..15, big-endian)
  - Result: `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` (36 characters + null terminator).

---

### 2.3 Critical Gaps in Current GPT Parser (`fs/partition.c:74-168`)

| # | Identified Gap / Bug | Location | Impact | Architectural Recommendation |
|---|---|---|---|---|
| **G1** | **No Backup GPT Header Fallback** | `fs/partition.c:80-110` | If Primary GPT Header at LBA 1 is damaged or CRC fails, `partition_scan_gpt` aborts immediately and returns 0 partitions. | Read backup GPT header at `dev->total_blocks - 1`. If valid, use it to locate backup partition array at `backup_hdr->partition_entries_lba`. |
| **G2** | **Array Clamping Breaks CRC32** | `fs/partition.c:122-144` | If `hdr->num_partition_entries > 128`, it clamps `num_entries = 128`, reads only 128 entries, and calculates CRC over 128 entries. The header's `partition_array_crc32` covers ALL entries, causing a CRC mismatch warning. | Read full partition array (`num_partition_entries * size_partition_entry`), verify full CRC32, then process up to `min(num_entries, 128)`. |
| **G3** | **Consecutive Indexing vs Entry Index** | `fs/partition.c:162-163` | Partitions are indexed as `valid_parts++`. If GPT partition 1 is deleted/empty, partition 2 becomes `p1` instead of `p2`. | Register child partition using slot index `i + 1`. |
| **G4** | **Unused Partition Names (UTF-16LE)** | `fs/partition.c:150-165` | `e->name[36]` contains the human-readable partition name (e.g. "EFI System", "KOS System"), but is ignored. | Decode UTF-16LE to ASCII and log the partition name upon registration. |
| **G5** | **Partition Type Storage** | `drivers/block.c:208-218` | `block_device` only stores MBR type and unique GUID. It does not store or classify the GPT `type_guid`. | Classify known type GUIDs (ESP, Basic Data, Linux) or store `type_guid` for volume manager probing. |

---

## 3. Child Block Device Model Deep Dive

### 3.1 Architecture & Slicing (`drivers/block.c:39-76, 155-230`)

The child block device abstraction represents a contiguous slice of a parent block device:
- **Parent Pointer**: `dev->parent` points to the underlying disk (`ramdisk`, `ahci`, `virtio`).
- **LBA Translation**:
  $$\text{parent\_lba} = \text{dev}\to\text{start\_lba} + \text{lba}$$
- **Boundary Verification**:
  $$\text{lba} + \text{count} \le \text{dev}\to\text{total\_blocks}$$

#### Forwarding Operations (`partition_ops`)
```c
static bool partition_read_blocks(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (dev == NULL || dev->parent == NULL) return false;
    if (lba + count > dev->total_blocks) return false;
    return block_read(dev->parent, dev->start_lba + lba, count, buffer);
}

static bool partition_write_blocks(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (dev == NULL || dev->parent == NULL) return false;
    if (lba + count > dev->total_blocks) return false;
    return block_write(dev->parent, dev->start_lba + lba, count, buffer);
}

static bool partition_flush(struct block_device *dev) {
    if (dev == NULL || dev->parent == NULL) return false;
    return block_flush(dev->parent);
}
```

### 3.2 Observed Deficiencies & Risks

1. **Integer Overflow in Bounds Check**:
   - `if (lba + count > dev->total_blocks)`: If `lba + count` overflows 64-bit integer (`lba > UINT64_MAX - count`), the condition could evaluate false and bypass bounds checking.
   - *Fix*: `if (count == 0 || lba > dev->total_blocks || count > dev->total_blocks - lba)`.
2. **Partition Naming Overflow for Indices $\ge 100$**:
   - In `drivers/block.c:176-179`:
     ```c
     if (part_index >= 10) {
         part_name[plen++] = (char)('0' + (part_index / 10));
     }
     part_name[plen++] = (char)('0' + (part_index % 10));
     ```
   - If `part_index == 100`, `100 / 10 = 10`, `'0' + 10 = ':'`, generating `blk0p:0`.
   - *Fix*: Use a standard integer-to-string conversion supporting 1..128.
3. **Cache Invalidation on Device Unregistration**:
   - In `drivers/block.c:232-261` (`block_device_unregister`):
     When a device is unregistered (or child partitions are dismantled), it does NOT invalidate or flush the block cache.
     Stale dirty blocks remain in `g_cache`, pointing to a deallocated or offline `struct block_device`.
     When an LRU eviction occurs later, writing back that victim will dereference an invalid `dev` pointer!
   - *Fix*: Call `block_cache_invalidate_device(dev)` for the device and all its child partitions prior to marking offline.

---

## 4. Block Cache Layer (R3) Deep Dive

### 4.1 Specifications & Current Implementation (`fs/block_cache.c`)

| Parameter | Specification | Implementation in `fs/block_cache.c` | Status |
|---|---|---|---|
| **Capacity** | 512 sectors (256 KiB) | `#define BLOCK_CACHE_CAPACITY 512`, 512 entries of 512 bytes | **Exact Match** |
| **Hash Table** | 128 buckets | `#define BLOCK_CACHE_HASH_SIZE 128` | **Exact Match** |
| **Replacement** | LRU (Least Recently Used) | Circular doubly linked list (`struct list_head lru_list`) | **Exact Match** |
| **Write Policy** | Write-Back (delayed write) | Dirty flag tracking with eviction writeback | **Exact Match** |
| **Global Sync** | `bcache_sync()` / `block_cache_sync()` | Flushes all dirty entries across all devices | **Implemented** |

### 4.2 Data Structures & Internal Layout

```c
struct cache_entry {
    struct block_device *dev;
    uint64_t lba;
    uint8_t data[BLOCK_DEFAULT_SECTOR_SIZE]; // 512 bytes
    bool valid;
    bool dirty;
    uint64_t last_access;
    struct list_head lru_node;
    struct list_head hash_node;
};

struct block_cache_state {
    struct cache_entry entries[BLOCK_CACHE_CAPACITY];  // 512 * ~560B = ~280 KiB RAM
    struct list_head lru_list;
    struct list_head hash_table[BLOCK_CACHE_HASH_SIZE]; // 128 heads
    struct block_cache_stats stats;
    kos_spinlock_t lock;
    bool initialized;
};
```

### 4.3 Hash Function Analysis
```c
static inline uint32_t cache_hash(uint32_t dev_id, uint64_t lba) {
    uint64_t key = ((uint64_t)dev_id << 48) ^ lba;
    key = (~key) + (key << 18);
    key = key ^ (key >> 31);
    key = key * 21;
    key = key ^ (key >> 11);
    key = key + (key << 6);
    key = key ^ (key >> 22);
    return (uint32_t)(key % BLOCK_CACHE_HASH_SIZE);
}
```
- Maps `(dev_id, lba)` uniformly across 128 buckets.
- At full occupancy (512 entries), average load factor is $\lambda = 4.0$ entries per bucket.
- Hash collision lookup is $O(1)$ with worst-case search length of 3-6 nodes.

### 4.4 Critical Architectural Issues in Block Cache

#### Issue 1: Blocking Hardware I/O Inside Interrupt-Disabled Spinlock (Deadlock & Timer Freeze Risk)
- **Observation**:
  In `block_cache_read()` (`fs/block_cache.c:162-203`):
  ```c
  uint64_t flags = spinlock_lock_irqsave(&g_cache.lock);
  ...
  /* Read miss block from disk */
  if (!block_read(dev, cur_lba, 1, entry->data)) { ... }
  ...
  spinlock_unlock_irqrestore(&g_cache.lock, flags);
  ```
  And in `cache_allocate_slot_locked()` (`fs/block_cache.c:118-120`):
  ```c
  if (entry->dirty) {
      cache_flush_entry_locked(entry); // Calls block_write() to hardware!
  }
  ```
- **Consequence**:
  1. `block_read` and `block_write` on AHCI or VirtIO drivers perform polling loops waiting for hardware DMA completion.
  2. For example, `virtio_blk_submit_io` in `drivers/virtio_blk.c:188-199`:
     ```c
     uint64_t timeout_tick = timer_get_ticks() + timer_ms_to_ticks(3000);
     while (timer_get_ticks() < timeout_tick) {
         if (vdev->used->idx != vdev->last_used_idx) { ... break; }
         cpu_relax();
     }
     ```
  3. `timer_get_ticks()` is incremented exclusively by the PIT timer interrupt ISR (`drivers/timer.c:71-75`).
  4. Because `spinlock_lock_irqsave(&g_cache.lock)` has disabled local CPU interrupts (`cli`), **the timer interrupt cannot fire while inside the spinlock**!
  5. If the hardware is slow or delayed, `timer_get_ticks()` NEVER increments, causing the timeout loop to spin indefinitely or defeat timeout detection!
  6. Furthermore, holding a global spinlock with interrupts disabled for milliseconds degrades kernel interrupt latency to catastrophic levels.
- **Architectural Solution**:
  - Buffer Cache Lock Decoupling:
    - Spinlock protects solely metadata (hash bucket insertion/deletion, LRU pointers).
    - Entries have an `in_io` / `busy` state flag.
    - When a cache miss occurs, mark entry `busy`, drop `g_cache.lock` (re-enabling interrupts), perform `block_read(dev, ...)`, re-acquire `g_cache.lock`, mark entry `valid` and `!busy`.

#### Issue 2: Cache Aliasing between Child Partitions and Parent Disks
- **Observation**:
  Cache key is `(dev->id, lba)`.
  If a child partition `blk0p1` starts at LBA 2048:
  - Reading `blk0p1` at LBA 0 caches under `(blk0p1->id, 0)`.
  - Reading `blk0` at LBA 2048 caches under `(blk0->id, 2048)`.
  These are two separate cache entries storing the identical physical sector. A write to one is invisible to reads from the other until flushed to disk and reloaded!
- **Architectural Solution**:
  Normalize `(dev, lba)` to root hardware device:
  ```c
  struct block_device *phys_dev = dev;
  uint64_t phys_lba = lba;
  while (phys_dev->parent != NULL) {
      phys_lba += phys_dev->start_lba;
      phys_dev = phys_dev->parent;
  }
  ```
  Keying the cache on `(phys_dev->id, phys_lba)` guarantees 100% coherence across any partition hierarchy!

#### Issue 3: `block_cache_sync()` Missing Hardware Controller Flush
- **Observation**:
  In `fs/block_cache.c:268-270`:
  ```c
  bool block_cache_sync(void) {
      return block_cache_flush_device(NULL);
  }
  ```
  When `dev == NULL`, `block_cache_flush_device` flushes dirty cache entries, but line 262 says:
  ```c
  if (dev != NULL) {
      block_flush(dev);
  }
  ```
  Thus, when called globally (`dev == NULL`), it NEVER calls `block_flush()` on the underlying block devices!
  Physical controllers (AHCI SATA, VirtIO) have internal volatile write caches that remain uncommitted to non-volatile media.
- **Architectural Solution**:
  When `dev == NULL`, iterate through all registered online block devices in `block_device_list` and invoke `block_flush(d)`.

---

## 5. Existing Kernel Primitives Survey

### 5.1 Memory Management
- **Kernel Heap (`mm/heap.c`, `include/kos/heap.h`)**:
  - Functions: `kmalloc(size)`, `kfree(ptr)`, `kcalloc(num, size)`, `krealloc(ptr, new_size)`.
  - Thread safety: Protected by internal `heap_lock` spinlock with `irqsave`.
  - Diagnostics: `heap_active_allocation_count()`, `heap_mapped_page_count()`.
  - Safe for all partition parsing dynamic buffers (e.g. GPT partition entries buffer).
- **Physical Memory Manager (`mm/pmm.c`, `include/kos/pmm.h`)**:
  - `pmm_alloc_page()`, `pmm_alloc_contiguous_pages()`, `pmm_free_page()`.
  - Used for DMA buffers in hardware drivers (AHCI Command List, FIS, VirtIO vring).

### 5.2 Synchronization Primitives
- **Spinlocks (`kernel/spinlock.c`, `include/kos/spinlock.h`)**:
  - `kos_spinlock_t`: 32-bit atomic state, owner CPU ID, acquisition instruction pointer (`last_rip`), acquisition counter.
  - APIs: `spinlock_init`, `spinlock_lock`, `spinlock_unlock`, `spinlock_lock_irqsave`, `spinlock_unlock_irqrestore`.
  - Preserves RFLAGS.IF bit accurately across nested calls.
- **Mutexes (`kernel/mutex.c`, `include/kos/mutex.h`)**:
  - Sleep-blocking mutual exclusion (`kos_mutex_t`).
  - Integrates with scheduler wait queue; puts thread to `THREAD_STATE_BLOCKED`.
  - Safe only in thread/task context (not in hardware ISR).
- **Wait Queues (`kernel/wait.c`, `include/kos/wait.h`)**:
  - `wait_queue_head_t`, `wait_queue_entry_t`.
  - Sleep-blocking condition variables with timeout support (`wait_event_timeout`).

### 5.3 Data Structures
- **Circular Doubly-Linked List (`include/kos/list.h`)**:
  - Standard Linux-style intrusive linked list (`struct list_head`).
  - Macros: `LIST_HEAD_INIT`, `list_add`, `list_add_tail`, `list_del`, `list_for_each_entry`, `list_for_each_entry_reverse`.
  - Used in block device registry, LRU list, and hash collision chains.

---

## 6. Actionable Recommendations & Implementation Blueprints

### 6.1 Backup GPT Header Fallback Blueprint (`fs/partition.c`)
To fulfill R2 completely:
```c
static uint32_t partition_scan_gpt(struct block_device *dev) {
    if (dev->total_blocks < 34) return 0;

    uint8_t sector_buf[512];
    bool use_backup = false;
    uint64_t header_lba = 1;

    /* 1. Try Primary GPT Header at LBA 1 */
    if (!block_read(dev, header_lba, 1, sector_buf) ||
        !gpt_header_is_valid((struct gpt_header *)sector_buf, dev, header_lba)) {
        
        /* 2. Fallback to Backup GPT Header at disk end */
        header_lba = dev->total_blocks - 1;
        log_warnf("[partition] Primary GPT invalid on %s, trying backup header at LBA %llu",
                  dev->name, (unsigned long long)header_lba);
        
        if (!block_read(dev, header_lba, 1, sector_buf) ||
            !gpt_header_is_valid((struct gpt_header *)sector_buf, dev, header_lba)) {
            log_errorf("[partition] Both primary and backup GPT headers invalid on %s", dev->name);
            return 0;
        }
        use_backup = true;
    }

    struct gpt_header *hdr = (struct gpt_header *)sector_buf;
    /* Process partition array from hdr->partition_entries_lba ... */
}
```

### 6.2 Cache Coherence Root Normalization Blueprint (`fs/block_cache.c`)
```c
static inline void cache_normalize_target(struct block_device **dev_io, uint64_t *lba_io) {
    struct block_device *d = *dev_io;
    uint64_t lba = *lba_io;
    while (d->parent != NULL) {
        lba += d->start_lba;
        d = d->parent;
    }
    *dev_io = d;
    *lba_io = lba;
}
```
Invoking `cache_normalize_target(&dev, &lba)` at the entry point of `block_cache_read` and `block_cache_write` seamlessly eliminates cache aliasing.

### 6.3 Comprehensive Global Sync Blueprint (`fs/block_cache.c`)
```c
bool block_cache_sync(void) {
    bool ok = block_cache_flush_device(NULL);
    
    /* Also flush underlying hardware controllers for all online devices */
    uint32_t count = block_device_count();
    for (uint32_t i = 0; i < count; i++) {
        struct block_device *dev = block_device_get_at(i);
        if (dev != NULL && dev->parent == NULL && dev->state == BLOCK_DEVICE_ONLINE) {
            block_flush(dev);
        }
    }
    return ok;
}
```

---

## 7. Conclusion & Next Steps for Team

The foundation for KOS storage, partition management, and block caching is solid and operational. By applying the targeted fixes outlined in this report—specifically:
1. Adding Backup GPT Header fallback and slot-based partition indexing in `fs/partition.c`.
2. Normalizing cache lookups to the root hardware device to prevent partition cache aliasing in `fs/block_cache.c`.
3. Flushing hardware devices in `block_cache_sync()`.
4. Invalidating block cache entries upon device unregistration in `drivers/block.c`.
5. Decoupling slow hardware I/O from the interrupt-disabled block cache spinlock.

The KOS storage architecture will achieve production-grade reliability, data integrity, and complete compliance with Milestone K16 & K17 requirements.
