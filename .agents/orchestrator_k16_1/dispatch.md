# Dispatch to Project Orchestrator (Milestone K16 & K17)

- **Target Workspace**: D:\DATA\source\OS\KOS\OS2
- **Agent Working Directory**: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1
- **User Request Source**: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md

## 2026-09-18T07:31:28Z
<USER_REQUEST>
You are the Project Orchestrator for Milestone K16 & K17 (Storage, Block Device, Partition, VFS & FAT32) of the KOS x86_64 operating system kernel.

Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k16_1
The project workspace root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is recorded in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md

Your mission is to orchestrate and complete Milestone K16 & K17:
1. R1: Universal Block Device Abstraction & Hardware Drivers (ramdisk, VirtIO Block, AHCI SATA).
2. R2: Partition Table Parsers (MBR & GPT).
3. R3: Block Cache Layer (256 KiB / 512 sectors, 128 hash buckets, LRU replacement, dirty writeback, bcache_sync).
4. R4: Modular VFS & FAT32 File System Engine (vnode, mount, file descriptor table, path resolver, FAT32 CRUD, LFN up to 255 chars, cluster chain management, safe ordering sync).
5. R5: Dynamic Volume Manager & CLI Tools (Volume Manager, drive letters, fdisk, blkinfo, mount, umount, sync, touch, mkdir, rm, mv; test commands: blocktest, mbrtest, gpttest, cachetest, fattest).

Acceptance Criteria:
- `tools\kos-tool.exe build` succeeds with 0 warnings and 0 errors (-Wall -Wextra -Werror).
- All automated tests (`blocktest`, `mbrtest`, `gpttest`, `cachetest`, `fattest`) pass 100% in QEMU without panic, hang, or memory leak.

Maintain your BRIEFING.md, PROJECT.md, and progress.md in your working directory.
Dispatch tasks to specialist subagents (explorer, worker/implementer, reviewer, challenger) per Teamwork protocols.
When the entire milestone is complete and validated, send a victory claim back to Sentinel.
</USER_REQUEST>
