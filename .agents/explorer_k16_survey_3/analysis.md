# Architectural Survey & Analysis Report: Modular VFS, FAT32 Engine, Dynamic Volume Manager & Test Suite

**Target Milestones**: K16 & K17 (Storage Stack, Modular VFS & FAT32)  
**Investigator**: Explorer Survey 3  
**Working Directory**: `D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_3`  
**Date**: 2026-09-18T08:00:00Z  
**Integrity Mode**: Freestanding C17, Clang/LLD/NASM x86_64, UEFI Limine, QEMU q35 cpu max  

---

## 1. Executive Summary

This report delivers an exhaustive architectural survey of the filesystem and storage management layers of the KOS operating system, specifically addressing **Requirement R4 (Modular VFS & FAT32 File System Engine)** and **Requirement R5 (Dynamic Volume Manager, CLI Diagnostic Tools & Automated Test Suite)** for Milestones K16 and K17.

### Current State
A preliminary implementation of the storage stack was merged in commit `282668d`:
- Core block device abstraction (`drivers/block.c`, `include/kos/block.h`).
- Drivers for RAM disk (`drivers/ramdisk.c`), VirtIO-Block (`drivers/virtio_blk.c`), and AHCI SATA (`drivers/ahci.c`).
- Partition table parsers for MBR and GPT (`fs/partition.c`, `include/kos/partition.h`).
- Block cache with 512 sectors / 256 KiB and LRU replacement (`fs/block_cache.c`, `include/kos/block_cache.h`).
- Basic FAT32 implementation (`fs/fat32.c`, `include/kos/fat32.h`).
- Dynamic volume manager (`fs/volume.c`, `include/kos/volume.h`).
- Shell commands and diagnostic tests in `kernel/command.c` (`fdisk`, `blkinfo`, `mount`, `umount`, `sync`, `touch`, `mkdir`, `rm`, `mv`, `blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`).
- Build system (`tools/kos.c`) compiles cleanly with `-Wall -Wextra -Werror` (0 warnings, 0 errors).

### Critical Gaps and Architectural Deficiencies
While the preliminary implementation satisfies the basic smoke tests, a rigorous forensic inspection reveals critical gaps that prevent full compliance with Milestones K16 & K17:
1. **Lack of a True Modular VFS**:
   - The current VFS (`fs/vfs.c`, `include/kos/vfs.h`) is a flat buffer wrapper (`struct vfs_file` holding `const uint8_t *data`), reading and writing entire files into kernel memory at once.
   - There are no `vnode_t` abstractions, no `vnode_ops`, no streaming file descriptors (`struct file`), no `seek`, and no `truncate`.
   - File descriptors in `process_t` and `handle_table` (`KOS_OBJ_FILE`) are completely disconnected from the VFS.
2. **FAT32 Engine Limitations & Defects**:
   - **LFN Creation is Missing**: `fs/fat32.c` only generates and writes standard 8.3 SFN directory entries (line 517). It **never writes LFN entries** when creating files or directories, violating the 255-character LFN requirement.
   - **Orphaned LFN on Unlink**: Unlinking only marks the SFN entry deleted (`0xE5`); preceding LFN entries are orphaned on disk.
   - **Directory Rename Broken**: `fat32_rename` reads the entire file into RAM and writes it to a new file. This fails 100% on directories because directories cannot be read as flat files.
   - **Missing CRUD Primitives**: `seek` and `truncate` are not implemented. `rmdir` does not check for empty directories.
   - **Multi-Volume Corruption Bug**: `fs/fat32.c` relies on a single global pointer `static struct fat32_volume *g_active_vfs_vol;` (line 1053). Mounting multiple FAT32 volumes (e.g. `E:` and `F:`) causes all VFS operations to route to the last mounted volume.
   - **FSInfo Sector Neglect**: `struct fat32_fsinfo` is defined but never updated on cluster allocation/deallocation, resulting in slow $O(N)$ linear scans on every write.
   - **Cached File Array Bottleneck**: The filesystem caches at most 128 files in a static array (`FAT32_MAX_CACHED_FILES`). Any files beyond 128 are invisible to VFS enumeration.
3. **Volume Manager Deficiencies**:
   - Lacks stable volume identification: `struct volume_entry` does not record Partition GUID or FAT32 Volume ID / Serial Number.
   - Lacks stable lookup methods by GUID or Volume ID.
4. **CLI Tools & Test Suite Gaps**:
   - `rmdir` is missing from the shell command table.
   - `ls` does not support path navigation (e.g., `ls E:/DOCS`).
   - `fattest` does not test LFN file creation (>11 chars or lowercase), seek, truncate, directory rename, or multi-volume concurrency.

---

## 2. Existing VFS & Filesystem Layout Analysis

### 2.1 Codebase Layout
- `include/kos/vfs.h`: VFS data types and volume operations.
- `fs/vfs.c`: VFS implementation with 8-volume array.
- `include/kos/ramfs.h` & `fs/ramfs.c`: In-memory static/dynamic filesystem mounted on `D:`.
- `fs/initramfs.c`: CPIO initramfs boot volume mounted on `C:`.
- `include/kos/process.h` & `kernel/process.c`: Process structures.
- `include/kos/handle.h` & `kernel/handle.c`: Handle table and generation-based handles.
- `include/kos/object.h` & `kernel/object.c`: Kernel object model with `KOS_OBJ_FILE`.

### 2.2 Current VFS Structure (`include/kos/vfs.h`, `fs/vfs.c`)
Currently, `fs/vfs.c` represents volumes as:
```c
enum {
    VFS_MAX_VOLUMES = 8,
    VFS_MAX_PATH = 256,
};

struct vfs_volume {
    char letter;
    const char *label;
    const struct vfs_backend *backend;
};

static struct vfs_volume volumes[VFS_MAX_VOLUMES];
static uint64_t mounted_volume_count;
static struct kos_spinlock vfs_lock = KOS_SPINLOCK_INITIALIZER;
```
And `struct vfs_backend` is defined as:
```c
struct vfs_backend {
    bool (*read_file)(const char *path, const uint8_t **data, uint64_t *size);
    bool (*write_file)(const char *path, const uint8_t *data, uint64_t size);
    uint64_t (*file_count)(void);
    const struct vfs_file *(*file_at)(uint64_t index);
    bool (*unlink_file)(const char *path);
};
```
#### Structural Problems:
1. **Flat Memory Paradigm**:
   - `read_file` passes `const uint8_t **data` and `uint64_t *size`. The backend is expected to provide a contiguous pointer in kernel heap or static memory.
   - For large files (e.g. a 100 MiB ISO or database), this requires allocating 100 MiB of contiguous kernel heap!
   - Streaming read/write (e.g. 4 KiB chunks) is impossible with this interface.
2. **No Context Pointer in `vfs_backend`**:
   - The function pointers in `vfs_backend` (`read_file`, `write_file`, etc.) take no `void *instance` or `struct vfs_mount *` argument.
   - As a result, filesystems that support multiple mounted instances (like FAT32) are forced to use global variables (`g_active_vfs_vol`), breaking multi-volume support!
3. **No Directory Traversal or Vnodes**:
   - There are no vnodes representing directory nodes or file nodes.
   - Paths are passed as raw strings to the backend.
   - Directory operations (`mkdir`, `rmdir`, `readdir`, `opendir`) are completely omitted from `vfs_backend`.
4. **Path Sanitizer Limitations**:
   - In `fs/vfs.c` line 96, `path_is_safe` rejects any path containing `..`:
     ```c
     if (*character == ':' || (character[0] == '.' && character[1] == '.'
             && (character == path || character[-1] == '/' || character[-1] == '\\')
             && (character[2] == '\0' || character[2] == '/' || character[2] == '\\'))) {
         return false;
     }
     ```
   - Normal path navigation using `../` is therefore completely prohibited.

### 2.3 Process Handle Table Integration (`kernel/handle.c`, `kernel/object.c`)
- Every process (`struct process` in `include/kos/process.h`) contains:
  ```c
  struct handle_table *handles;
  ```
- `struct handle_table` (`include/kos/handle.h`) manages up to 64 handles with 32-bit generation IDs to prevent ABA reuse.
- `enum kos_object_type` (`include/kos/object.h`) already declares:
  ```c
  KOS_OBJ_FILE,
  ```
- However, there is currently **no struct representing an open file object** in the kernel, and `KOS_OBJ_FILE` is not used anywhere in the VFS.

---

## 3. Modular VFS Architecture Specification (R4 Target)

To fulfill Requirement R4, KOS must introduce a modular VFS based on the `vnode` abstraction, while maintaining full backward compatibility for existing callers.

### 3.1 Architecture Overview
```
+--------------------------------------------------------------------------+
|                            Process Userspace / Shell                     |
|          open() / read() / write() / lseek() / close() / fstat()         |
+--------------------------------------------------------------------------+
                                    |
                                    v
+--------------------------------------------------------------------------+
|                      Handle Table (kernel/handle.c)                      |
|             Maps integer fd -> struct vfs_file_object (KOS_OBJ_FILE)     |
+--------------------------------------------------------------------------+
                                    |
                                    v
+--------------------------------------------------------------------------+
|                         VFS Core (fs/vfs_core.c)                         |
|   - Path Resolver (resolves "C:/...", "E:/dir/file", ".", "..")          |
|   - Mount Table (mount points: 'C', 'D', 'E', ..., 'Z')                  |
|   - Vnode Lifecycle & Reference Counting (vref, vrele)                   |
+--------------------------------------------------------------------------+
                                    |
            +-----------------------+-----------------------+
            |                       |                       |
            v                       v                       v
+-----------------------+ +-------------------+ +-----------------------+
|   FAT32 Vnode Ops     | |   Ramfs Vnode Ops | |   Initramfs Vnode Ops |
|  - fat32_lookup       | |  - ramfs_lookup   | |  - initramfs_lookup   |
|  - fat32_read         | |  - ramfs_read     | |  - initramfs_read     |
|  - fat32_write        | |  - ramfs_write    | |  (read-only)          |
|  - fat32_truncate     | |  - ramfs_truncate |                         |
|  - fat32_readdir      | |  - ramfs_readdir  |                         |
+-----------------------+ +-------------------+ +-----------------------+
            |
            v
+--------------------------------------------------------------------------+
|                       Block Cache (fs/block_cache.c)                     |
+--------------------------------------------------------------------------+
            |
            v
+--------------------------------------------------------------------------+
|                    Block Device Layer (drivers/block.c)                  |
+--------------------------------------------------------------------------+
```

### 3.2 Core VFS Data Structures

#### 1. Vnode Type & Operations
```c
enum vnode_type {
    VNODE_TYPE_NONE = 0,
    VNODE_TYPE_REG,   /* Regular file */
    VNODE_TYPE_DIR,   /* Directory */
    VNODE_TYPE_BLK,   /* Block device */
    VNODE_TYPE_CHR,   /* Character device */
};

struct vnode_stat {
    enum vnode_type type;
    uint64_t size;
    uint32_t attributes;  /* Read-only, hidden, archive, etc. */
    uint32_t block_size;
    uint64_t blocks;
    uint64_t creation_time;
    uint64_t modification_time;
};

struct vfs_dirent {
    char name[256];
    enum vnode_type type;
    uint64_t size;
    uint32_t attributes;
};

struct vnode;
struct vfs_mount;

struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out_vn);
    int (*create)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out_vn);
    int (*mkdir)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out_vn);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*rmdir)(struct vnode *dir, const char *name);
    int (*rename)(struct vnode *old_dir, const char *old_name, struct vnode *new_dir, const char *new_name);
    int (*read)(struct vnode *vn, uint64_t offset, void *buffer, uint64_t count, uint64_t *bytes_read);
    int (*write)(struct vnode *vn, uint64_t offset, const void *buffer, uint64_t count, uint64_t *bytes_written);
    int (*truncate)(struct vnode *vn, uint64_t new_size);
    int (*readdir)(struct vnode *dir, uint64_t *offset, struct vfs_dirent *dirent);
    int (*getattr)(struct vnode *vn, struct vnode_stat *stat);
    int (*sync)(struct vnode *vn);
    void (*release)(struct vnode *vn);
};

struct vnode {
    enum vnode_type type;
    uint32_t flags;
    atomic_t refcount;
    struct vfs_mount *mount;
    const struct vnode_ops *ops;
    void *fs_data;          /* Filesystem-specific node (e.g. struct fat32_node) */
    kos_spinlock_t lock;
    struct list_head list_node;
};
```

#### 2. VFS Mount & Filesystem Type
```c
struct vfs_filesystem_type {
    const char *name;
    int (*mount)(struct vfs_mount *mount, struct block_device *dev, const void *data);
    int (*unmount)(struct vfs_mount *mount);
    int (*sync)(struct vfs_mount *mount);
};

struct vfs_mount {
    char letter;                    /* 'C' through 'Z' */
    char label[32];
    const struct vfs_filesystem_type *fs_type;
    struct block_device *dev;
    struct vnode *root_vnode;
    void *fs_private;               /* Filesystem instance (e.g. struct fat32_volume *) */
    kos_spinlock_t lock;
    struct list_head mount_node;
};
```

#### 3. Open File Object (Handle Table Integration)
```c
#define VFS_O_RDONLY  0x0001
#define VFS_O_WRONLY  0x0002
#define VFS_O_RDWR    (VFS_O_RDONLY | VFS_O_WRONLY)
#define VFS_O_CREAT   0x0004
#define VFS_O_TRUNC   0x0008
#define VFS_O_APPEND  0x0010

enum vfs_seek_whence {
    VFS_SEEK_SET = 0,
    VFS_SEEK_CUR = 1,
    VFS_SEEK_END = 2,
};

struct vfs_file_object {
    struct kos_object_header header; /* type = KOS_OBJ_FILE */
    struct vnode *vnode;
    uint64_t offset;
    uint32_t flags;
    kos_spinlock_t lock;
};
```

### 3.3 Path Resolver Specification
The path resolver must normalize input paths into a canonical volume and relative path:
1. **Drive Letter Prefix**: If the path starts with `[A-Za-z]:`, extract the drive letter and advance the pointer past the colon. If omitted, default to `'C'`.
2. **Slash Normalization**: Treat `/` and `\` identically. Strip duplicate consecutive slashes (`//` -> `/`).
3. **Component Tokenization & Dot Processing**:
   - Parse each path component delimited by slashes.
   - Ignore `.` components.
   - On `..` components: backtrack one directory level in the path component stack. Do not allow backtracking beyond the mount root.
4. **Vnode Traversal**:
   - Obtain the `root_vnode` of the target volume mount.
   - For each path component, invoke `dir_vn->ops->lookup(dir_vn, component, &next_vn)`.
   - Release the parent vnode (`vnode_unref(dir_vn)`).
   - If a non-directory component is encountered before the final token, return `ENOTDIR`.
   - If a component does not exist, return `ENOENT`.

### 3.4 Backward Compatibility Interface
All existing VFS functions in `include/kos/vfs.h` remain fully supported:
- `vfs_read_file(path, &data, &size)`: Resolves path, opens file, reads `size` bytes into `kmalloc` buffer, returns pointer.
- `vfs_write_file(path, data, size)`: Resolves path, opens with `O_CREAT | O_TRUNC | O_WRONLY`, writes `data`, closes.
- `vfs_unlink_file(path)`: Resolves parent directory and calls `dir_vn->ops->unlink`.
- `vfs_file_count_on_volume(letter)` & `vfs_file_at_on_volume(letter, index)`: Enumerates root vnode.

---

## 4. FAT32 Filesystem Engine Deep Dive & Gap Analysis (R4)

### 4.1 On-Disk Data Structures & Calculations

#### BPB and Volume Geometry (`include/kos/fat32.h`)
```
+--------------------------------------------------------------------------+
| LBA 0: Boot Sector (BPB)                                                 |
| Bytes per sector: 512 | Sectors per cluster: Sc                          |
| Reserved sector count: R (typically 32) | Num FATs: N (typically 2)      |
| FAT size 32: F (sectors per FAT)                                         |
+--------------------------------------------------------------------------+
| LBA 1: FSInfo Sector                                                     |
| Signatures: 0x41615252, 0x61417272, 0xAA550000                           |
| Free cluster count: free_count | Next free cluster hint: next_free       |
+--------------------------------------------------------------------------+
| LBA R .. R + F - 1: FAT 1 Table                                          |
| Entries 0, 1: Reserved (0x0FFFFFF8, 0x0FFFFFFF)                          |
| Entries 2 .. Total Clusters + 1: 32-bit Cluster Links (mask 0x0FFFFFFF)   |
+--------------------------------------------------------------------------+
| LBA R + F .. R + 2*F - 1: FAT 2 Table (Mirror of FAT 1)                  |
+--------------------------------------------------------------------------+
| LBA R + N*F: Data Clusters Begin (Cluster 2)                             |
| LBA(cluster) = (R + N*F) + (cluster - 2) * Sc                            |
+--------------------------------------------------------------------------+
```

### 4.2 Forensic Analysis of Gaps in Existing `fs/fat32.c`

#### Gap 1: Missing LFN Writing (CRITICAL)
In `fs/fat32.c` lines 506-557:
```c
static bool fat32_add_dir_entry(
    struct fat32_volume *vol,
    uint32_t dir_cluster,
    const char *name,
    uint8_t attr,
    uint32_t first_clus,
    uint32_t file_size
) {
    uint32_t cur_clus = dir_cluster;
    uint8_t sec_buf[512];
    uint8_t sfn[11];
    fat32_generate_83_name(name, sfn);  // <--- TRUNCATES TO 8.3!
...
    // Only finds 1 single 32-byte slot and writes SFN!
    kmemcpy(de->name, sfn, 11);
...
```
**Impact**:
- Any file created with a name longer than 8 characters, or with lowercase letters (e.g. `MyReport.txt`, `k16_k17_report.md`), is irreversibly stripped to an 8.3 uppercase SFN (e.g. `MYREPORT.TXT`, `K16_K17_.MD`).
- The user request explicitly demands: **"Hỗ trợ Long File Names (LFN lên tới 255 ký tự)."**
- The current implementation only **parses** LFN when reading (`fat32_lookup_entry_in_dir`), but **never writes** LFN entries!

#### Required LFN Implementation:
1. **LFN Detection**: If `name` contains lowercase characters, spaces, special allowed characters (`+`, `,`, `;`, `=`, `[`, `]`), or length > 8.3, LFN is required.
2. **SFN Basis & Numeric Tail Generation**:
   - Strip illegal characters, convert to uppercase.
   - Format: `BASENA~1.EXT`. If `~1` collides with an existing file in the directory, increment to `~2`, `~3`, etc.
3. **SFN Checksum**:
   ```c
   static uint8_t fat32_calc_checksum(const uint8_t *sfn) {
       uint8_t sum = 0;
       for (int i = 0; i < 11; i++) {
           sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
       }
       return sum;
   }
   ```
4. **LFN Chunking**:
   - Compute number of LFN slots: $K = (length + 12) / 13$. For 255 chars, $K \le 20$.
   - Total contiguous directory slots needed: $K + 1$ (K LFN entries + 1 SFN entry).
5. **Contiguous Slot Allocation**:
   - Scan directory sectors for a run of $K + 1$ consecutive slots with `de->name[0] == 0x00 || de->name[0] == 0xE5`.
   - If crossing cluster boundaries or reaching end of cluster chain, allocate a new cluster via `fat32_allocate_cluster`.
6. **Reverse LFN Emission**:
   - Slot 1 to $K$: write in reverse order (entry $K$ down to 1).
   - Sequence number for entry $K$: `K | 0x40` (`0x40` = `LAST_LONG_ENTRY`).
   - Sequence number for entries $K-1$ down to 1: `(K - 1)` down to `1`.
   - Encode name in UTF-16LE: `name0_4` (5 chars), `name5_10` (6 chars), `name11_12` (2 chars).
   - Pad characters after null terminator with `0xFFFF`.
   - Set `attr = 0x0F`, `type = 0x00`, `first_cluster_low = 0x0000`, `checksum = fat32_calc_checksum(sfn)`.
   - Immediately follow with the 32-byte SFN entry.

#### Gap 2: Orphaned LFN Entries on Unlink (CRITICAL)
In `fs/fat32.c` lines 707-715:
```c
    /* Mark entry deleted (0xE5) */
    uint64_t dir_lba = cluster_to_lba(vol, res.dir_cluster) + res.dir_sector_offset;
    uint8_t sec_buf[512];
    if (block_cache_read(vol->dev, dir_lba, 1, sec_buf)) {
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
        entries[res.entry_index_in_sector].name[0] = 0xE5; // <--- ONLY SFN MARKED!
        block_cache_write(vol->dev, dir_lba, 1, sec_buf);
    }
```
**Impact**: When a file with LFN is deleted, its LFN entries remain untouched in the directory. A standard FAT32 chkdsk/fsck will flag this as corrupt orphaned LFN chains.
**Fix**: `fat32_lookup_entry_in_dir` must track the starting LBA/slot of the LFN sequence. On `unlink`, mark **all $K$ LFN slots and the SFN slot** with `0xE5`.

#### Gap 3: Directory Rename Failure & Excessive I/O (CRITICAL)
In `fs/fat32.c` lines 825-844:
```c
bool fat32_rename(struct fat32_volume *vol, const char *old_path, const char *new_path) {
    const uint8_t *data = NULL;
    uint64_t size = 0;
    if (!fat32_read_file(vol, old_path, &data, &size)) {
        return false;  // FAILS IF old_path IS A DIRECTORY!
    }
    if (!fat32_write_file(vol, new_path, data, size)) { ... }
    fat32_unlink_file(vol, old_path);
    return true;
}
```
**Impact**:
1. Renaming any directory fails immediately because `fat32_read_file` explicitly rejects directories.
2. For files, renaming a 100 MiB file reads 100 MiB into RAM, allocates 100 MiB of new clusters, writes 100 MiB of blocks, and deletes the old clusters!
**Fix**: Rename must operate purely at the directory entry level:
- Read existing entry (`first_cluster`, `file_size`, attributes, timestamps).
- Add new directory entry in destination directory pointing to the **same** `first_cluster`.
- If moving a directory to a different parent, update its internal `..` entry to point to the new parent cluster.
- Delete the old directory entry (and its LFN slots).
- Zero data cluster copies required!

#### Gap 4: Missing `seek` and `truncate` Operations
- File truncation is needed when opening files with `O_TRUNC`, or calling `ftruncate()`:
  - **Shrinking**: Follow FAT chain to target cluster, write `0x0FFFFFFF` (EOF), free subsequent clusters via `fat32_free_cluster_chain`, update directory entry `file_size`.
  - **Expanding**: Follow FAT chain to EOF, allocate additional clusters, link them, zero new clusters, update directory entry `file_size`.
- Streaming read/write at an arbitrary byte offset:
  - Compute `start_cluster_index = offset / cluster_size`.
  - Traverse FAT chain $N$ steps to reach start cluster.
  - Read/write partial cluster at `offset % cluster_size`.

#### Gap 5: Multi-Volume Global Variable Collision
In `fs/fat32.c` lines 1053-1098:
```c
static struct fat32_volume *g_active_vfs_vol = NULL;

static bool fat32_vfs_read_file(const char *path, const uint8_t **data, uint64_t *size) {
    if (g_active_vfs_vol == NULL) return false;
    return fat32_read_file(g_active_vfs_vol, path, data, size);
}
```
**Impact**: If volume `E:` is mounted, and then volume `F:` is mounted: `g_active_vfs_vol` is overwritten to point to `F:`. Any operation on `E:` will read or write to `F:`!
**Fix**: In the modular VFS, each `struct vnode` has `vn->mount->fs_private`, which points directly to its specific `struct fat32_volume`. No global variables are used.

#### Gap 6: FSInfo Sector Maintenance & Allocation Performance
- `include/kos/fat32.h` defines `struct fat32_fsinfo`.
- `fat32.c` never reads or writes sector 1 (`fs_info_sector`).
- In `fat32_allocate_cluster`:
  ```c
  for (uint32_t clus = 2; clus < vol->total_clusters + 2; clus++)
  ```
  Every cluster allocation always starts from cluster 2, resulting in an $O(N)$ linear scan over the entire FAT table.
- **Fix**: Load FSInfo on mount. Start searching from `fsinfo->next_free`. When allocating, update `next_free` and decrement `free_count`. Write FSInfo back to disk through block cache.

#### Gap 7: Safe Mutation Ordering for Crash Consistency
R4 specifies: **"Đồng bộ FAT table, directory entry và data cluster đúng thứ tự để chống filesystem corruption."**
The required safe ordering protocol:
1. **File Extension**:
   1. Allocate new clusters from free pool.
   2. Zero and write payload data into new data clusters in block cache.
   3. Update FAT table chain to link new clusters.
   4. Update directory entry with new `file_size`.
   5. Flush block cache (`block_cache_flush_device`).
2. **File Deletion**:
   1. Mark directory entry deleted (`0xE5`) first.
   2. Mark FAT chain entries free (`0x00000000`).
   3. Update FSInfo free cluster count.
   4. Flush block cache.
This prevents dangling FAT pointers or lost clusters in case of power failure or crash.

---

## 5. Dynamic Volume Manager Specification (R5 Target)

### 5.1 Analysis of Current `fs/volume.c`
- Manages an array of up to 16 `struct volume_entry`.
- Drive letter assignment: `C:` is reserved for Initramfs, `D:` for Ramfs. `E:` through `Z:` are dynamically assigned to detected block devices.
- Scan function: `volume_manager_scan_and_mount()` probes all block devices from `block_device_get_at(i)` and mounts those with valid FAT32 headers.

### 5.2 Required Enhancements for R5

#### 1. Stable Volume Identification
Requirement R5 states: **"Nhận diện ổ đĩa ổn định bằng Partition GUID hoặc Volume ID."**
Currently, `struct volume_entry` lacks fields for GUID or Volume ID.
`struct volume_entry` should be extended:
```c
struct volume_entry {
    char letter;                    /* e.g. 'E' */
    char label[32];                 /* e.g. "STORAGE_01" */
    char fs_type[16];               /* "FAT32", "RAMFS", etc. */
    struct block_device *bdev;      /* Pointer to backing block device */
    void *fs_handle;                /* Pointer to fat32_volume */
    bool is_mounted;

    /* Stable Volume Identifiers */
    uint32_t volume_id;             /* FAT32 32-bit volume serial number */
    uint8_t partition_guid[16];     /* GPT 128-bit Partition GUID */
    bool has_guid;                  /* True if backing device is GPT partition */
};
```

#### 2. Stable Lookup APIs
```c
const struct volume_entry *volume_manager_find_by_guid(const uint8_t guid[16]);
const struct volume_entry *volume_manager_find_by_volume_id(uint32_t volume_id);
```
These APIs ensure that storage volumes can be re-identified regardless of changes in PCI discovery order or insertion port.

---

## 6. CLI Tools & Automated Test Suites Analysis (R5)

### 6.1 Shell Commands in `kernel/command.c`
The following CLI commands are currently registered:
| Command | Handler Function | Current Functionality | Issues / Missing Features |
|---|---|---|---|
| `fdisk` | `command_fdisk` (line 3776) | Lists block devices, types, sizes, states, MBR info. | Does not display GPT GUIDs. |
| `blkinfo` | `command_fdisk` (line 4538) | Alias for `fdisk`. | Clean. |
| `mount` | `command_mount` (line 3808) | Lists mounted volumes or mounts a device to a drive letter. | Clean. |
| `umount` | `command_umount` (line 3857) | Unmounts volume by drive letter (protects `C:`). | Clean. |
| `sync` | `command_sync` (line 3879) | Flushes block cache to hardware. | Clean. |
| `touch` | `command_touch` (line 3888) | Writes 0 bytes to file path. | Fails if parent dir doesn't exist. |
| `mkdir` | `command_mkdir` (line 3901) | Calls `fat32_mkdir`. | Only works for FAT32 volumes. |
| `rm` | `command_rm` (line 3926) | Calls `vfs_unlink_file`. | Clean. |
| `mv` | `command_mv` (line 3939) | Copies file data to new path, unlinks old path. | Fails on directories, memory heavy. |
| `ls` | `command_ls` (line 835) | Lists files on a volume (e.g. `ls C:`). | Cannot list subdirectories (`ls E:/DOCS`). |
| `rmdir` | **MISSING** | Not registered in `command_registry`. | Must be added. |

### 6.2 Automated Test Suites Analysis
The existing test suite commands in `kernel/command.c` provide strong baseline verification:
1. `blocktest` (line 3976): Verifies RAM block device creation, multi-sector write/read, bounds checking, zero-length write rejection, flush, and destruction.
2. `mbrtest` (line 4072): Constructs an MBR sector with 2 partitions, scans, registers child block devices (`mbr_diskp1`, `mbr_diskp2`), and validates offset translation.
3. `gpttest` (line 4158): Constructs a protective MBR, GPT header, and 128-byte partition array with CRC32 calculation, scans, registers child block device, and verifies GUID.
4. `cachetest` (line 4254): Writes 64 sectors through cache, reads back, checks stats (hits/misses), tests dirty block flush to backing device, and invalidates cache.
5. `fattest` (line 4333): Formats a 16 MiB ramdisk as FAT32, mounts it, writes/reads a small file, writes/reads a 12 KiB multi-cluster file, creates a directory, writes/reads in a subdirectory, renames a file, unlinks, and unmounts.

#### Required Test Expansions for `fattest`:
To ensure complete test coverage of R4 & R5 requirements, `fattest` must be enhanced with:
1. **LFN Test**: Create a file with a 64-character and 255-character filename containing mixed case and spaces (e.g., `Test_Very_Long_Filename_KOS_Fat32_2026_Validation.txt`). Read it back by its full long name, verify exact data match.
2. **Seek & Partial Write Test**: Open a multi-cluster file, seek to offset 5000, overwrite 200 bytes, read back the entire file, and verify surrounding bytes are unmodified.
3. **Truncate Test**: Shrink a 12 KiB file to 2 KiB, verify clusters are freed in FAT table. Then grow it to 8 KiB, verify zero-padding.
4. **Directory Rename Test**: Create `/FOLDER_A/FILE.TXT`, rename `/FOLDER_A` to `/FOLDER_B`, verify `/FOLDER_B/FILE.TXT` is accessible.
5. **Rmdir Test**: Attempt `rmdir` on a non-empty directory (must fail). Delete all files inside, then `rmdir` (must succeed).
6. **Multi-Volume Concurrency Test**: Create two independent FAT32 ramdisks, mount them simultaneously on `E:` and `F:`, write unique files to both, verify that operations on `E:` do not contaminate `F:`.

---

## 7. Build System & Tooling Survey

- Build Tool: `tools/kos-tool.exe` compiled from `tools/kos.c`.
- Compiler & Linker:
  - Clang: `clang -target x86_64-unknown-none-elf -std=c17 -ffreestanding -Wall -Wextra -Werror -O2 -g ...`
  - NASM: `nasm -f elf64`
  - LLD: `ld.lld -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T boot/linker.ld`
- Source Manifest: `c_sources[]` in `tools/kos.c` lines 23-36 includes all current storage and filesystem source files (`fs/block_cache.c`, `fs/fat32.c`, `fs/partition.c`, `fs/ramfs.c`, `fs/vfs.c`, `fs/volume.c`).
- Build Verification: Executed `.\tools\kos-tool.exe build` during this survey:
  - Exit code: 0 (clean build).
  - Generated: `build/kos.elf` with 791 embedded symbols.
  - Staged: UEFI ESP at `build/esp`.

---

## 8. Implementation Roadmap & Recommendations

To bring Milestones K16 & K17 to 100% completion, the work should be structured into two sequential milestones:

### Milestone M4: Modular VFS & Full FAT32 Engine
1. **Refactor VFS Core**:
   - Define `vnode_t`, `vnode_ops`, `struct vfs_mount`, and `struct vfs_file_object`.
   - Implement unified path resolver (`.` and `..` support, `/` and `\` normalization).
   - Integrate `vfs_open`, `vfs_read`, `vfs_write`, `vfs_seek`, `vfs_close` with `process_t->handles` using `KOS_OBJ_FILE`.
   - Retain legacy VFS APIs as thin wrappers for zero breakage.
2. **Upgrade FAT32 Engine**:
   - Implement bidirectional LFN: generate UTF-16LE LFN entries (up to 255 chars), compute 8.3 checksum, find contiguous slot runs, write reverse sequence entries.
   - Fix unlinking: mark all preceding LFN slots deleted (`0xE5`).
   - Fix rename: implement directory-entry-only rename for files and subdirectories.
   - Implement `fat32_seek` and `fat32_truncate`.
   - Implement `fat32_rmdir` with empty directory checking.
   - Eliminate `g_active_vfs_vol` global variable by storing volume context in mount/vnode.
   - Maintain FSInfo sector (`free_count`, `next_free`).
   - Enforce safe mutation ordering: data blocks -> FAT chain -> directory entry -> flush.

### Milestone M5: Dynamic Volume Manager, CLI Tools & Test Suite
1. **Enhance Volume Manager**:
   - Add Partition GUID and Volume ID to `struct volume_entry`.
   - Add `volume_manager_find_by_guid` and `volume_manager_find_by_volume_id`.
2. **Enhance CLI Shell Tools**:
   - Add `rmdir` command to `command_registry`.
   - Upgrade `ls` to accept paths (e.g. `ls E:/DOCS`).
   - Upgrade `mv` to use vnode rename for instant zero-copy moves.
3. **Comprehensive Test Suite**:
   - Expand `fattest` with tests for LFN up to 255 chars, seek, truncate, directory rename, rmdir, and multi-volume concurrency.
   - Verify 0 leaks, 0 crashes in QEMU.
