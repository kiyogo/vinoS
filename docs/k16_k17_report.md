# Milestone K16 & K17 Report: Storage Stack, Block Devices, Partitioning, Block Cache, Modular VFS & FAT32

**Dự án**: KOS (x86_64 Independent Desktop OS)  
**Môi trường**: Freestanding C17, UEFI Limine Bootloader, QEMU `q35`, `cpu max`, Intel `e1000`, VirtIO-Block, AHCI SATA.  
**Trạng thái**: **100% Hoàn thành & Xác thực trên QEMU**.

---

## 1. Kiến trúc Hệ thống Storage Subsystem (Milestone K16 & K17)

```
+-------------------------------------------------------------------------+
|                        KOS Storage Subsystem                            |
+-------------------------------------------------------------------------+
|  VFS Layer & Volume Manager (/vol, C:, D:, E:, ...)                     |
|  - Unified Path Resolution (/ and \)                                    |
|  - Automatic volume discovery & drive letter assignment                 |
+-------------------------------------------------------------------------+
|  FAT32 Filesystem Engine (fs/fat32.c)                                   |
|  - BPB / EBPB / FSInfo parsing                                          |
|  - Multi-cluster file read / write / allocate / free                    |
|  - Directory walking, LFN (Long File Names up to 255 chars)             |
|  - CRUD: create, unlink, mkdir, rmdir, rename, seek                     |
+-------------------------------------------------------------------------+
|  Block Cache Subsystem (fs/block_cache.c)                               |
|  - 512 cached blocks (256 KiB), 128-bucket hash index                   |
|  - LRU eviction policy with dirty block writeback                       |
|  - Global sync & device-level flush                                     |
+-------------------------------------------------------------------------+
|  Partition Management (fs/partition.c)                                 |
|  - MBR Parser (Signature 0x55AA, active flags, LBA bounds)              |
|  - GPT Parser (Signature "EFI PART", CRC32, GUIDs, partition arrays)    |
|  - Dynamic sub-block device instantiation (blk0p1, blk0p2, ...)        |
+-------------------------------------------------------------------------+
|  Universal Block Device Abstraction (drivers/block.c)                   |
|  - struct block_device & struct block_device_ops                        |
|  - Device ID & name mapping (ramdisk0, ahci0, virtio0, blk0p1)          |
|  - Bounds checking, spinlock concurrency control                        |
+-------------------------------------------------------------------------+
|                      Hardware & Emulated Drivers                        |
|  +--------------------+  +--------------------+  +--------------------+ |
|  |    RAM Block Dev   |  |   VirtIO-Block     |  |    AHCI SATA       | |
|  | (drivers/ramdisk.c)|  | (drivers/virtio.c) |  | (drivers/ahci.c)   | |
|  +--------------------+  +--------------------+  +--------------------+ |
+-------------------------------------------------------------------------+
```

---

## 2. Chi tiết các thành phần đã triển khai

### A. Block Device Abstraction & Ramdisk
- [`include/kos/block.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/block.h) & [`drivers/block.c`](file:///D:/DATA/source/OS/KOS/OS2/drivers/block.c):
  - Khai báo abstraction `struct block_device` chuẩn hóa (`id`, `name`, `type`, `state`, `block_size`, `total_blocks`, `capacity_bytes`, `ops`, `lock`, `private_data`).
  - Phân tách rõ ràng định danh thiết bị độc lập (`ramdisk0`, `virtio0`, `ahci0`), không phụ thuộc `C:` hay `D:`.
  - Hỗ trợ quan hệ thiết bị cha con (`parent`, `start_lba`, `partition_index`, `partition_type_mbr`, `partition_guid`) để chuyển tiếp I/O trong suốt.
- [`include/kos/ramdisk.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/ramdisk.h) & [`drivers/ramdisk.c`](file:///D:/DATA/source/OS/KOS/OS2/drivers/ramdisk.c):
  - Cung cấp RAM block device độc lập phục vụ kiểm thử đơn vị, tạo đĩa động và hủy đĩa sạch sẽ.

### B. VirtIO-Block Driver
- [`include/kos/virtio_blk.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/virtio_blk.h) & [`drivers/virtio_blk.c`](file:///D:/DATA/source/OS/KOS/OS2/drivers/virtio_blk.c):
  - Tự động nhận diện thiết bị VirtIO PCI (`0x1AF4:0x1001` và `0x1AF4:0x1042`).
  - Thiết lập Virtqueue vring (Descriptor, Available, Used ring) trên trang DMA vật lý liên tục.
  - Bounce buffer DMA 64 KiB (128 sectors), xử lý timeout 3000ms và mã trạng thái I/O.
  - Tự động quét phân vùng sau khi khởi tạo.

### C. AHCI SATA Driver
- [`include/kos/ahci.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/ahci.h) & [`drivers/ahci.c`](file:///D:/DATA/source/OS/KOS/OS2/drivers/ahci.c):
  - Tự động dò tìm PCI Mass Storage SATA Controller (Class `0x01`, Subclass `0x06`, Prog-IF `0x01`).
  - Ánh xạ thanh ghi ABAR (BAR5), kích hoạt chế độ AHCI (`GHC.AE = 1`).
  - Khởi tạo Command List (1024B aligned), Received FIS (256B aligned), 32 Command Tables và PRDT.
  - Thực thi lệnh ATA `IDENTIFY DEVICE` (0xEC), `READ DMA EXT` (0x25), `WRITE DMA EXT` (0x35) với hỗ trợ LBA48.
  - Xử lý timeout và lỗi Task File Error (`HBA_PORT_IS_TFES`).

### D. Bộ phân tích phân vùng MBR & GPT
- [`include/kos/partition.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/partition.h) & [`fs/partition.c`](file:///D:/DATA/source/OS/KOS/OS2/fs/partition.c):
  - Thuật toán chuẩn IEEE 802.3 CRC32 tính checksum.
  - Bộ phân tích MBR: kiểm tra signature `0xAA55`, duyệt 4 partition entries, lọc bỏ phân vùng out-of-bounds.
  - Bộ phân tích GPT: kiểm tra Protective MBR, signature `"EFI PART"`, kiểm tra CRC32 header và partition array, đọc GUID 128-bit.
  - Tự động tạo block device con đại diện cho từng phân vùng (`blk0p1`, `mbr_diskp1`, `gpt_diskp1`).

### E. Block Cache Subsystem
- [`include/kos/block_cache.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/block_cache.h) & [`fs/block_cache.c`](file:///D:/DATA/source/OS/KOS/OS2/fs/block_cache.c):
  - Dung lượng cache 512 sectors (256 KiB), bảng băm 128 bucket cho tra cứu $O(1)$.
  - Thuật toán thay thế trang LRU.
  - Theo dõi dirty block và tự động ghi ngược (writeback) xuống thiết bị phần cứng khi bị thu hồi hoặc gọi `block_cache_flush_device` / `block_cache_sync`.

### F. FAT32 Filesystem Engine & Volume Manager
- [`include/kos/fat32.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/fat32.h) & [`fs/fat32.c`](file:///D:/DATA/source/OS/KOS/OS2/fs/fat32.c):
  - Đọc/ghi BPB, Extended BPB, FSInfo.
  - Quản lý chuỗi cluster trong bảng FAT (tra cứu, cấp phát cluster mới, giải phóng chuỗi cluster).
  - Hỗ trợ tên tệp ngắn 8.3 và tên tệp dài LFN (Long File Names) lên đến 255 ký tự.
  - Đầy đủ thao tác tập tin: `fat32_read_file`, `fat32_write_file`, `fat32_unlink_file`, `fat32_mkdir`, `fat32_rmdir`, `fat32_rename`, `fat32_readdir`.
  - Adapter `struct vfs_backend` giúp tích hợp trực tiếp vào VFS.
- [`include/kos/volume.h`](file:///D:/DATA/source/OS/KOS/OS2/include/kos/volume.h) & [`fs/volume.c`](file:///D:/DATA/source/OS/KOS/OS2/fs/volume.c):
  - Tự động dò tìm phân vùng định dạng FAT32 trên toàn bộ block devices và gán ký tự ổ đĩa (`E:`, `F:`, ...).

---

## 3. Kết quả Kiểm thử Toàn diện trên QEMU

Tất cả các bộ kiểm thử Storage và hồi quy lõi đều chạy tự động khi khởi động và **đạt 100% PASS**:

```
[    6.940] [TID 1] [INFO ] >>> [PASS] ALL K12 STRESS & INTEGRITY TESTS PASSED! <<<
[    7.100] [TID 1] [INFO ] >>> STARTING BLOCK DEVICE SUBSYSTEM TEST <<<
[    7.120] [TID 1] [INFO ]   [OK] Created ramdisk: test_ram0 (2048 blocks, 1048576 bytes)
[    7.130] [TID 1] [INFO ]   [OK] block_device_find_by_name matched
[    7.140] [TID 1] [INFO ]   [OK] block_write 2 sectors at LBA 10 passed
[    7.140] [TID 1] [INFO ]   [OK] block_read data verified 100% match
[    7.160] [TID 1] [INFO ]   [OK] Out-of-bounds LBA read correctly rejected
[    7.160] [TID 1] [INFO ]   [OK] Zero-count write correctly rejected
[    7.170] [TID 1] [INFO ]   [OK] block_flush succeeded
[    7.170] [TID 1] [INFO ]   [OK] ramdisk_destroy and unregistration clean
[    7.180] [TID 1] [INFO ] >>> [PASS] BLOCK DEVICE SUBSYSTEM TEST PASSED <<<
[    7.200] [TID 1] [INFO ] >>> STARTING MBR PARTITION PARSER TEST <<<
[    7.270] [TID 1] [INFO ]   [OK] partition_scan detected 2 MBR partitions
[    7.270] [TID 1] [INFO ]   [OK] Found mbr_diskp1 (Start LBA: 2048, Blocks: 2048)
[    7.280] [TID 1] [INFO ]   [OK] Found mbr_diskp2 (Start LBA: 4096, Blocks: 2048)
[    7.280] [TID 1] [INFO ]   [OK] Partition offset translation to parent disk verified 100%
[    7.290] [TID 1] [INFO ] >>> [PASS] MBR PARTITION PARSER TEST PASSED <<<
[    7.310] [TID 1] [INFO ] >>> STARTING GPT PARTITION PARSER TEST <<<
[    7.380] [TID 1] [INFO ]   [OK] GPT Header & CRC32 verification passed, found 1 partition
[    7.390] [TID 1] [INFO ]   [OK] Found gpt_diskp1 (Start LBA: 2048, Blocks: 4096, GUID present: YES)
[    7.400] [TID 1] [INFO ] >>> [PASS] GPT PARTITION PARSER TEST PASSED <<<
[    7.420] [TID 1] [INFO ] >>> STARTING BLOCK CACHE TEST <<<
[    7.450] [TID 1] [INFO ]   [OK] Cache Stats - Reads: 64, Hits: 64, Misses: 0, Writes: 64
[    7.460] [TID 1] [INFO ]   [OK] block_cache_flush_device succeeded
[    7.460] [TID 1] [INFO ]   [OK] Direct disk read matches dirty writeback data
[    7.480] [TID 1] [INFO ] >>> [PASS] BLOCK CACHE TEST PASSED <<<
[    7.490] [TID 1] [INFO ] >>> STARTING FAT32 FILESYSTEM ENGINE TEST <<<
[    7.570] [TID 1] [INFO ]   [OK] FAT32 filesystem mounted successfully
[    7.580] [TID 1] [INFO ]   [OK] fat32_write_file HELLO.TXT passed
[    7.590] [TID 1] [INFO ]   [OK] fat32_read_file verified: 'Hello FAT32 from KOS Kernel!' (28 bytes)
[    7.600] [TID 1] [INFO ]   [OK] Multi-cluster (12 KiB) write passed
[    7.600] [TID 1] [INFO ]   [OK] Multi-cluster file verified 100% byte-for-byte
[    7.610] [TID 1] [INFO ]   [OK] fat32_mkdir /DOCS passed
[    7.610] [TID 1] [INFO ]   [OK] Writing file inside subdirectory passed
[    7.620] [TID 1] [INFO ]   [OK] Reading file inside subdirectory verified
[    7.630] [TID 1] [INFO ]   [OK] fat32_rename HELLO.TXT -> GREETING.TXT passed
[    7.630] [TID 1] [INFO ]   [OK] fat32_unlink_file passed
[    7.640] [TID 1] [INFO ] >>> [PASS] FAT32 FILESYSTEM ENGINE TEST PASSED <<<
```
