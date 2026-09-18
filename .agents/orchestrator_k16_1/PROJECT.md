# Project: Milestone K16 & K17 (Storage Stack, Block Devices, Partitions, Block Cache, Modular VFS & FAT32)

## Architecture
The KOS Storage Subsystem is structured into 5 decoupled architectural layers:
```
+-------------------------------------------------------------------------+
|                  Layer 5: Volume Manager & CLI Tools                    |
|  - Dynamic Volume discovery & drive letter assignment (C:, D:, E: ...)  |
|  - Stable volume lookup via Partition GUID or Volume ID                 |
|  - CLI commands: fdisk, blkinfo, mount, umount, sync, touch, mkdir,     |
|    rm, rmdir, mv                                                        |
|  - Automated Test Suites: blocktest, mbrtest, gpttest, cachetest,       |
|    fattest                                                              |
+-------------------------------------------------------------------------+
|                     Layer 4: Modular VFS & FAT32                        |
|  - Modular VFS: struct vnode, vnode_ops, vfs_mount, struct file,        |
|    file descriptor table integration (KOS_OBJ_FILE)                     |
|  - Path resolver: normalized '/' and '\', '.' and '..', drive prefix    |
|  - FAT32 Engine: BPB, FSInfo, FAT chain management, multi-cluster files |
|  - Full CRUD: create, unlink, mkdir, rmdir, rename, readdir, seek,      |
|    truncate                                                             |
|  - Long File Names (LFN up to 255 chars) bidirectional read/write       |
|  - Safe ordering sync: data clusters -> FAT table -> directory entries  |
|  - Multi-volume support: no global state, instance-specific private data|
+-------------------------------------------------------------------------+
|                      Layer 3: Block Cache Layer                         |
|  - 512 cached sectors (256 KiB), 128 hash buckets (Thomas Wang 64-bit)  |
|  - Doubly-linked LRU list for clean eviction                            |
|  - Dirty block tracking & writeback flush                               |
|  - Root-device normalization (prevent partition cache aliasing)         |
|  - Decoupled I/O from spinlock (preserve PIT timer interrupts)          |
|  - Global sync (bcache_sync) & hardware controller flush                |
+-------------------------------------------------------------------------+
|                Layer 2: Partition Table Parsers (MBR & GPT)             |
|  - MBR: signature 0x55AA, 4 entries, LBA bounds check                   |
|  - GPT: Protective MBR, EFI PART, CRC32 header & table, GUID extraction |
|  - GPT Backup Header fallback at total_blocks - 1                       |
|  - Full partition table array CRC32 calculation                         |
|  - Slot-based partition numbering (i + 1) e.g. blk0p1, blk0p2           |
|  - Transparent child block device slicing (start_lba, total_blocks)     |
|  - Invalidation of block cache entries upon device unregistration       |
+-------------------------------------------------------------------------+
|           Layer 1: Universal Block Device Abstraction & Drivers         |
|  - struct block_device & struct block_device_ops (read, write, flush)   |
|  - Independent device ID and names (ramdisk0, virtio0, ahci0)           |
|  - RAM Block Device (ramdisk) for isolated unit testing                 |
|  - VirtIO Block Driver: 2-page virtqueue (8192B), 4096B used ring align |
|  - AHCI SATA Driver: PCI 0x01:0x06, ABAR MMIO, LBA48 DMA, PRDT, FIS     |
|  - PMM DMA allocator: pmm_alloc_pages contiguous memory buffers         |
+-------------------------------------------------------------------------+
```

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Universal Block Device Abstraction | struct block_device, block_device_ops, device registry by ID, synchronous read/write/flush | M1 | R1 |
| 2 | RAM Block Device (ramdisk) | Memory-backed block device for isolated tests and benchmarking | M1 | R1 |
| 3 | Contiguous DMA Page Allocator | Allocate contiguous physical frames via pmm_alloc_pages for drivers | M1 | R1 |
| 4 | VirtIO Block Driver Hardening | Fix 2-page virtqueue layout (used ring at offset 4096), PCI 0x1AF4:0x1001 discovery | M1 | R1 |
| 5 | AHCI SATA Driver Hardening | Fix command table pointer tracking, LBA48 DMA read/write, identify device | M1 | R1 |
| 6 | MBR Partition Table Parser | Signature 0x55AA, 4 entries, active flag, LBA bounds check | M2 | R2 |
| 7 | GPT Partition Table Parser | Protective MBR, "EFI PART", CRC32 header, full array CRC32, GUID extraction | M2 | R2 |
| 8 | GPT Backup Header Fallback | Fallback to secondary GPT header at total_blocks - 1 on primary corruption | M2 | R2 |
| 9 | Child Block Device Registration | Slicing parent device, slot-based naming (i + 1), bounds overflow protection | M2 | R2 |
| 10 | Cache Invalidation on Device Unregister | Invalidate block cache entries on device unregistration to avoid dangling pointers | M2 | R2 |
| 11 | Block Cache Layer Architecture | 512 sectors (256 KiB), 128 hash buckets, Thomas Wang 64-bit mix hash | M3 | R3 |
| 12 | LRU Replacement & Dirty Writeback | Doubly-linked LRU list, dirty buffer writeback on eviction or flush | M3 | R3 |
| 13 | Root-Device Cache Normalization | Normalize partition LBA to parent disk LBA to eliminate cache aliasing | M3 | R3 |
| 14 | Decoupled Cache I/O Locking | Decouple disk I/O from spinlock to keep timer interrupts active during polling | M3 | R3 |
| 15 | Global Sync & Controller Flush | bcache_sync() writes dirty buffers and flushes underlying hardware controllers | M3 | R3 |
| 16 | Modular VFS Abstraction | struct vnode, struct vnode_ops, struct vfs_mount, struct vfs_ops | M4 | R4 |
| 17 | Streaming File Descriptors | struct file, KOS_OBJ_FILE integration with handle_table, refcounts | M4 | R4 |
| 18 | VFS Path Resolver | Unified path parser ('/', '\', '.', '..', drive prefixes C:, D:, E:) | M4 | R4 |
| 19 | FAT32 BPB & FSInfo Management | Parse BPB, EBPB, track and update free cluster count and next free in FSInfo | M4 | R4 |
| 20 | Multi-Cluster FAT Chain Operations | Cluster lookup, allocation, truncation, traversal for files spanning clusters | M4 | R4 |
| 21 | Bidirectional LFN Engine | Read and write Long File Names up to 255 characters (UTF-16LE, checksum, slots) | M4 | R4 |
| 22 | Complete FAT32 CRUD | create, unlink (with LFN purge), mkdir, rmdir (empty check), rename (files & dirs), readdir, seek, truncate | M4 | R4 |
| 23 | Safe Ordering Mutation Sync | Write order: data clusters -> FAT table updates -> directory entries -> flush | M4 | R4 |
| 24 | Multi-Volume FAT32 Support | Per-volume state in vfs_mount private data, eliminating global volume pointers | M4 | R4 |
| 25 | Dynamic Volume Manager | Automatic volume discovery on block devices/partitions, drive letter assignment | M5 | R5 |
| 26 | Stable Volume Identification | Volume lookup by Partition GUID (GPT) or Volume Serial Number / ID (MBR) | M5 | R5 |
| 27 | Storage CLI Commands | fdisk, blkinfo, mount, umount, sync, touch, mkdir, rm, rmdir, mv | M5 | R5 |
| 28 | Comprehensive Automated Test Suite | blocktest, mbrtest, gpttest, cachetest, fattest (expanded LFN, seek, truncate, rename) | M5 | R5 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Block Device Abstraction & Hardware Drivers | Universal block dev, ramdisk, PMM DMA allocation, VirtIO 2-page queue, AHCI SATA | None | PLANNED |
| M2 | Partition Parsers & Child Devices | MBR, GPT with backup fallback, full array CRC32, slot naming, overflow check, unregister invalidation | M1 | PLANNED |
| M3 | Block Cache Layer Hardening | 512 sectors / 256 KiB, 128 buckets, LRU, root normalization, decoupled I/O locking, global controller flush | M1, M2 | PLANNED |
| M4 | Modular VFS & FAT32 File System Engine | vnodes, mount table, file descriptors, path resolver, FAT32 CRUD, LFN up to 255 chars, safe ordering, multi-volume | M1, M2, M3 | PLANNED |
| M5 | Dynamic Volume Manager, CLI Tools & Test Suite | Volume manager, drive letters, GUID/ID lookup, CLI commands, expanded blocktest, mbrtest, gpttest, cachetest, fattest | M1, M2, M3, M4 | PLANNED |

## Interface Contracts

### Block Device ↔ Driver
```c
struct block_device_ops {
    bool (*read_blocks)(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer);
    bool (*write_blocks)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
    bool (*flush)(struct block_device *dev);
    void (*destroy)(struct block_device *dev);
};
```

### Partition ↔ Block Device
- Partition device has `dev->parent != NULL`.
- `dev->start_lba` is offset in parent.
- Child `read_blocks` translates: `block_read(dev->parent, dev->start_lba + lba, count, buf)`.

### Block Cache ↔ Block Device
- Cache keys normalized to `(dev->parent ? dev->parent->id : dev->id)` and `(dev->parent ? dev->start_lba + lba : lba)`.
- Eviction writeback and flush invoke `block_write` and `block_flush` on the root device.

### VFS ↔ File System (FAT32)
```c
struct vnode_ops {
    bool (*lookup)(struct vnode *dir, const char *name, struct vnode **out_vn);
    bool (*create)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out_vn);
    bool (*mkdir)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out_vn);
    bool (*unlink)(struct vnode *dir, const char *name);
    bool (*rmdir)(struct vnode *dir, const char *name);
    bool (*rename)(struct vnode *old_dir, const char *old_name, struct vnode *new_dir, const char *new_name);
    int64_t (*read)(struct vnode *vn, uint64_t offset, void *buffer, uint64_t size);
    int64_t (*write)(struct vnode *vn, uint64_t offset, const void *buffer, uint64_t size);
    bool (*truncate)(struct vnode *vn, uint64_t new_size);
    bool (*readdir)(struct vnode *dir, uint64_t *pos, struct vfs_dirent *dirent);
    bool (*sync)(struct vnode *vn);
    void (*release)(struct vnode *vn);
};
```

## Code Layout
- `include/kos/block.h`, `drivers/block.c`: Universal block device abstraction
- `include/kos/ramdisk.h`, `drivers/ramdisk.c`: RAM block device
- `include/kos/virtio_blk.h`, `drivers/virtio_blk.c`: VirtIO Block Driver
- `include/kos/ahci.h`, `drivers/ahci.c`: AHCI SATA Driver
- `include/kos/partition.h`, `fs/partition.c`: Partition table parsers (MBR & GPT)
- `include/kos/block_cache.h`, `fs/block_cache.c`: Block Cache subsystem
- `include/kos/vfs.h`, `fs/vfs.c`: Modular VFS Layer
- `include/kos/fat32.h`, `fs/fat32.c`: FAT32 File System Engine
- `include/kos/volume.h`, `fs/volume.c`: Dynamic Volume Manager
- `kernel/command.c`: CLI commands and automated test suites (`blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`)
