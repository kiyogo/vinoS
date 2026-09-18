# Handoff Report: Explorer Survey 3 (Modular VFS, FAT32 Engine, Volume Manager & Test Suite)

**Agent**: Explorer Survey 3  
**Target Milestone**: K16 & K17 (R4 & R5)  
**Parent Orchestrator**: `orchestrator_k16_1` (Conversation ID: `a0cfd602-3f1b-4ffa-9469-2c19d542b4b0`)  
**Working Directory**: `D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_3`  
**Date**: 2026-09-18T08:05:00Z  

---

## 1. Observation

Direct observations from inspecting the KOS codebase at `D:\DATA\source\OS\KOS\OS2`:

### 1.1 VFS and Filesystem Abstraction
1. `include/kos/vfs.h` lines 7-19:
   ```c
   struct vfs_file {
       const char *path;
       const uint8_t *data;
       uint64_t size;
   };

   struct vfs_backend {
       bool (*read_file)(const char *path, const uint8_t **data, uint64_t *size);
       bool (*write_file)(const char *path, const uint8_t *data, uint64_t size);
       uint64_t (*file_count)(void);
       const struct vfs_file *(*file_at)(uint64_t index);
       bool (*unlink_file)(const char *path);
   };
   ```
   No `vnode` or `inode` structure exists in `include/kos/vfs.h`. File data is represented solely as contiguous buffers (`const uint8_t *data`).
2. `fs/vfs.c` lines 96-100:
   ```c
   if (*character == ':' || (character[0] == '.' && character[1] == '.'
           && (character == path || character[-1] == '/' || character[-1] == '\\')
           && (character[2] == '\0' || character[2] == '/' || character[2] == '\\'))) {
       return false;
   }
   ```
   `path_is_safe` unconditionally rejects any path component containing `..`.
3. `include/kos/object.h` line 22 & `kernel/object.c` line 53:
   `KOS_OBJ_FILE` is defined in `enum kos_object_type`, but no `struct file` or file descriptor object exists in `fs/` or `include/kos/`.
4. `kernel/process.c` & `include/kos/process.h` line 40:
   `struct process` contains `struct handle_table *handles;`, but handle table functions (`handle_create`, `handle_lookup`, `handle_close`) are not used by the VFS.

### 1.2 FAT32 Implementation Defects
1. `fs/fat32.c` lines 506-541:
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
       fat32_generate_83_name(name, sfn);
   ...
       kmemcpy(de->name, sfn, 11);
       de->attr = attr;
       de->first_cluster_high = (uint16_t)(first_clus >> 16);
       de->first_cluster_low = (uint16_t)(first_clus & 0xFFFF);
       de->file_size = file_size;
       return block_cache_write(vol->dev, clus_lba + s, 1, sec_buf);
   ```
   When creating files, `fat32_add_dir_entry` only generates 8.3 SFN names. **No LFN entries are ever generated or written to disk.**
2. `fs/fat32.c` lines 707-715:
   ```c
   /* Mark entry deleted (0xE5) */
   uint64_t dir_lba = cluster_to_lba(vol, res.dir_cluster) + res.dir_sector_offset;
   uint8_t sec_buf[512];
   if (block_cache_read(vol->dev, dir_lba, 1, sec_buf)) {
       struct fat32_dir_entry *entries = (struct fat32_dir_entry *)sec_buf;
       entries[res.entry_index_in_sector].name[0] = 0xE5;
       block_cache_write(vol->dev, dir_lba, 1, sec_buf);
   }
   ```
   On `unlink`, only the single SFN entry at `res.entry_index_in_sector` is marked `0xE5`. Preceding LFN entries are left orphaned.
3. `fs/fat32.c` lines 825-833:
   ```c
   bool fat32_rename(struct fat32_volume *vol, const char *old_path, const char *new_path) {
       if (vol == NULL || !vol->mounted || old_path == NULL || new_path == NULL) return false;

       const uint8_t *data = NULL;
       uint64_t size = 0;
       if (!fat32_read_file(vol, old_path, &data, &size)) {
           return false;
       }
   ```
   `fat32_rename` reads the old file through `fat32_read_file`, which explicitly returns `false` if `res.entry.attr & FAT_ATTR_DIRECTORY` (line 448). Therefore, renaming a directory fails 100% of the time.
4. `fs/fat32.c` lines 1053-1098:
   ```c
   static struct fat32_volume *g_active_vfs_vol = NULL;

   static bool fat32_vfs_read_file(const char *path, const uint8_t **data, uint64_t *size) {
       if (g_active_vfs_vol == NULL) return false;
       return fat32_read_file(g_active_vfs_vol, path, data, size);
   }
   ```
   A single global pointer `g_active_vfs_vol` is shared across all FAT32 mounts.
5. `fs/fat32.c` line 16:
   ```c
   #define FAT32_MAX_CACHED_FILES 128
   ```
   The cached file array is capped at 128 entries. Files beyond 128 are omitted from enumeration.
6. `include/kos/fat32.h` lines 82-90 & `fs/fat32.c`:
   `struct fat32_fsinfo` is defined, but sector 1 (`fs_info_sector`) is never read or updated during cluster allocation in `fat32_allocate_cluster` (lines 105-129).

### 1.3 Volume Manager & CLI Tools
1. `include/kos/volume.h` lines 10-17:
   ```c
   struct volume_entry {
       char letter;
       char label[32];
       char fs_type[16];
       struct block_device *bdev;
       void *fs_handle;
       bool is_mounted;
   };
   ```
   There is no Partition GUID field or FAT32 Volume ID field in `struct volume_entry`.
2. `kernel/command.c` lines 4506-4567:
   `command_registry` contains `fdisk`, `blkinfo`, `mount`, `umount`, `sync`, `touch`, `mkdir`, `rm`, `mv`, but lacks `rmdir`.
3. `kernel/command.c` lines 4333-4504:
   `command_fattest` tests basic 8.3 file creation, multi-cluster writes, and subdirectory creation, but does not test LFN up to 255 characters, seek, truncate, directory rename, rmdir, or multi-volume concurrency.

### 1.4 Build System Verification
Execution of `.\tools\kos-tool.exe build` exited with code 0:
```
Kernel ELF built with 791 embedded symbols: build/kos.elf
UEFI ESP staged: build/esp
```
The codebase compiles with `-Wall -Wextra -Werror` without warnings or errors.

---

## 2. Logic Chain

1. **VFS Modernization Need**:
   - Observation 1.1.1 shows that `vfs_backend` requires whole files to be loaded into contiguous memory.
   - Observation 1.1.2 shows that `..` path traversal is rejected.
   - Observation 1.1.3 and 1.1.4 show that `process_t->handles` and `KOS_OBJ_FILE` are disconnected from the VFS.
   - *Inference*: To satisfy Requirement R4 ("Chuẩn hóa VFS với vnode, mount entry, file object, file descriptor table và path resolver"), KOS must introduce `struct vnode`, `struct vnode_ops`, `struct vfs_mount`, and `struct vfs_file_object`, integrating them with `handle_table`.

2. **FAT32 Defect Resolution**:
   - Observation 1.2.1 shows that `fat32_add_dir_entry` only writes 8.3 SFN entries.
   - *Inference*: To satisfy Requirement R4 ("Hỗ trợ Long File Names lên tới 255 ký tự"), an LFN write engine must be implemented: generating UTF-16LE chunks, computing SFN checksums, finding contiguous slots in directory clusters, and writing reverse-order entries with sequence flags.
   - Observation 1.2.2 shows orphaned LFN entries on `unlink`.
   - *Inference*: Directory unlink must track and erase all preceding LFN slots with `0xE5`.
   - Observation 1.2.3 shows that directory rename fails.
   - *Inference*: Rename must be rewritten to manipulate directory entries rather than reading data buffers into memory.
   - Observation 1.2.4 shows a global variable bottleneck.
   - *Inference*: Eliminating `g_active_vfs_vol` and storing volume references in `vfs_mount->fs_private` is required to allow multiple FAT32 volumes (`E:`, `F:`) without cross-talk.

3. **Volume Manager Stable Identification**:
   - Observation 1.3.1 shows `volume_entry` lacks GUID and Volume ID.
   - *Inference*: To satisfy Requirement R5 ("Nhận diện ổ đĩa ổn định bằng Partition GUID hoặc Volume ID"), `struct volume_entry` must store `partition_guid` (for GPT) and `volume_id` (for MBR/FAT32), with corresponding lookup functions.

4. **Test Suite Completeness**:
   - Observation 1.3.3 shows `fattest` lacks coverage for LFN up to 255 chars, seek, truncate, directory rename, and multi-volume concurrency.
   - *Inference*: `command_fattest` in `kernel/command.c` must be expanded to validate these operations to ensure no regressions and 100% specification compliance.

---

## 3. Caveats

1. **Legacy Compatibility**: The legacy VFS API (`vfs_read_file`, `vfs_write_file`, etc.) must be preserved as wrapper functions over the new vnode layer to avoid breaking `fs/initramfs.c`, `kernel/main.c`, and existing diagnostics.
2. **Cluster Allocation Boundary**: LFN contiguous slot allocation can span across cluster boundaries or require allocating an additional cluster for the directory. The allocator must handle directory expansion safely.
3. **Hardware Storage in QEMU**: Physical AHCI and VirtIO drives require appropriate QEMU device arguments. For standalone unit tests (`blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`), RAM block devices (`ramdisk_create`) ensure 100% deterministic test execution in standard QEMU environments.

---

## 4. Conclusion

1. The current codebase has a functioning block device, block cache, and MBR/GPT foundation, but the VFS and FAT32 layers are incomplete prototypes that fail key aspects of Milestone K16 & K17 requirements (specifically: lack of vnodes, missing LFN write support, broken directory rename, missing seek/truncate, and single-instance FAT32 limitation).
2. The recommended division of remaining work into milestones is:
   - **Milestone M4**: Modular VFS & FAT32 File System Engine (vnodes, mount table, file descriptors, path resolver, bidirectional LFN up to 255 chars, CRUD, seek, truncate, safe mutation ordering).
   - **Milestone M5**: Dynamic Volume Manager, CLI Tools & Test Suite (stable volume ID/GUID, `rmdir`, `ls` path support, expanded `fattest` covering all new operations).
3. Detailed architectural designs, data structures, and implementation algorithms are fully documented in `D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_3\analysis.md`.

---

## 5. Verification Method

To independently verify the observations and conclusions in this report:

1. **Verify Current Build Status**:
   ```pwsh
   cd D:\DATA\source\OS\KOS\OS2
   .\tools\kos-tool.exe build
   ```
   Expected: 0 warnings, 0 errors.

2. **Inspect SFN-only Write in FAT32**:
   Open `fs/fat32.c` and inspect line 517 (`fat32_generate_83_name`) and line 534 (`kmemcpy(de->name, sfn, 11)`). Notice the absence of `fat32_lfn_entry` creation.

3. **Inspect Directory Rename Failure**:
   Open `fs/fat32.c` and inspect line 830 (`fat32_read_file(vol, old_path, &data, &size)`). Compare with line 448 (`if (res.entry.attr & FAT_ATTR_DIRECTORY) return false;`).

4. **Inspect Global Variable in FAT32 Backend**:
   Open `fs/fat32.c` and inspect line 1053 (`static struct fat32_volume *g_active_vfs_vol = NULL;`).

5. **Verify Full Test Suite in QEMU**:
   ```pwsh
   cd D:\DATA\source\OS\KOS\OS2
   .\tools\kos-tool.exe run --display none
   ```
   Observe boot log on serial output confirming all existing test commands execute.
