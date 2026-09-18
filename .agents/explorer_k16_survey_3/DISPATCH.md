# Dispatch: Explorer Survey 3 (Modular VFS, FAT32 Engine, Volume Manager & CLI Tools)

## Mission
Investigate Modular VFS, FAT32 file system engine, Dynamic Volume Manager, and CLI diagnostic tools & automated tests for Milestone K16 & K17 (R4 & R5).

## Authoritative Reference
Read `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` before starting.

## Key Focus Areas
1. **Existing VFS & File System Layout**:
   - Inspect `fs/`, `include/kos/vfs.h`, `include/kos/ramfs.h` or similar. How are files, directories, and mounts currently represented?
   - How does process file descriptor management work (`process_t`, handle table, `vfs_open`, `vfs_read`, etc.)?
2. **Modular VFS Architecture (R4)**:
   - Abstractions: `vnode_t` (type: file, dir; ops: lookup, create, mkdir, unlink, rmdir, read, write, truncate, readdir, getattr, setattr; refcount, flags).
   - `vfs_mount_t` / mount table: mount point path, underlying device, filesystem type ops (`mount`, `unmount`, `sync`, `root_vnode`).
   - Path resolver: parsing absolute paths, relative paths, Windows-style drive letters (`C:/`, `D:/`, `E:/`) or standard unix `/`, handling `.` and `..`.
3. **FAT32 Engine Specification (R4)**:
   - On-disk layout: Boot Sector & BPB, FSInfo sector, FAT1, FAT2, Data Clusters.
   - Cluster size (sectors per cluster * 512), first data sector calculation.
   - FAT table management: reading/writing FAT entries (mask `0x0FFFFFFF`), cluster allocation (finding free cluster 0x00000000, setting EOF marker `0x0FFFFFFF`), cluster chain traversal, cluster deallocation/truncation.
   - Directory entries: SFN (Short File Name 8.3 format), LFN (Long File Name up to 255 chars in UTF-16, multiple LFN entries, checksum validation).
   - CRUD operations: `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `readdir`, `seek`, `truncate`.
   - Data integrity & safe ordering: write data clusters first -> update FAT chain -> update directory entry -> sync.
4. **Dynamic Volume Manager & Drive Letters (R5)**:
   - Volume discovery on block devices and partitions.
   - Drive letter assignment (`E:`, `F:`, etc., or `C:`, `D:`).
   - Stable volume identification by Partition GUID or Volume Serial Number / ID.
5. **CLI Tools & Automated Test Suite (R5)**:
   - Inspect `kernel/command.c`, `kernel/shell.c` or CLI registry.
   - CLI commands: `fdisk`, `blkinfo`, `mount`, `umount`, `sync`, `touch`, `mkdir`, `rm`, `mv`.
   - Automated test suite commands:
     - `blocktest`: Read/write/flush on RAM disk and block devices.
     - `mbrtest`: Create MBR, write partition table, parse and detect partitions.
     - `gpttest`: Create GPT, calculate CRC32 header/table, parse GUID and detect partitions.
     - `cachetest`: Cache hit/miss, LRU eviction, dirty writeback, `bcache_sync`.
     - `fattest`: Format/mount FAT32, create directories, multi-cluster file write/read, seek, truncate, rename, unlink, data integrity check.
6. **Build System & Tooling**:
   - Inspect `tools/kos-tool.exe build` commands and source directories in Makefile / build scripts.

## Output
Write comprehensive analysis and architecture report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_3\analysis.md`
and write a self-contained handoff report to:
`D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_3\handoff.md`
Report back when complete.
