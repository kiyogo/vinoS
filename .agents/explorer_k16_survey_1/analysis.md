# KOS Storage Architecture & Hardware Drivers Analysis Report (Milestone K16 & K17)

**Agent**: Explorer Survey 1  
**Working Directory**: `D:\DATA\source\OS\KOS\OS2\.agents\explorer_k16_survey_1`  
**Reference Document**: `D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md`  
**Date**: 2026-09-18  

---

## 1. Executive Summary

This investigation surveys the complete low-level storage stack in the KOS x86_64 kernel, focusing on:
- **Universal Block Device Abstraction** (`include/kos/block.h`, `drivers/block.c`)
- **RAM Block Device (`ramdisk`)** (`include/kos/ramdisk.h`, `drivers/ramdisk.c`)
- **VirtIO Block Driver** (`include/kos/virtio_blk.h`, `drivers/virtio_blk.c`)
- **AHCI SATA Driver** (`include/kos/ahci.h`, `drivers/ahci.c`)
- **PCI Subsystem & Device Enumeration** (`include/kos/pci.h`, `arch/x86_64/pci.c`)
- **MMIO Mapping & Virtual Memory Subsystem** (`include/kos/vmm.h`, `mm/vmm.c`)
- **Physical Memory Management & DMA Allocation** (`include/kos/pmm.h`, `mm/pmm.c`, `mm/heap.c`)
- **Build System Integration** (`tools/kos.c`, `tools/kos-tool.exe`)

### Key Observations & Highlights
1. **Clean Abstraction**: KOS features a standardized `struct block_device` with operations (`read_blocks`, `write_blocks`, `flush`, `destroy`), independent integer IDs, unique device names (`ramdisk0`, `ahci0`, `virtio0`), and transparent partition child device forwarding (`blk0p1`).
2. **RAM Disk Ready**: Memory-backed block device creation and teardown (`ramdisk_create`, `ramdisk_destroy`) is robust, tested via `command_blocktest` in `kernel/command.c`.
3. **AHCI SATA Implemented**: Full AHCI 1.3 compliance with Port discovery, Command List, Received FIS, Command Tables, PRDT entries, and 48-bit LBA DMA read/write (`0x25`/`0x35`) and IDENTIFY (`0xEC`).
4. **VirtIO Block Driver Implemented but has a Critical Structural Defect**: While the driver logic and register interaction adhere to VirtIO 0.9.5, the virtqueue memory allocation allocates only 1 page (4096 bytes) and sets `vdev->used` to offset 2048, overlapping with `vdev->avail`. Legacy VirtIO requires `used` ring to be 4096-byte aligned (requiring 2 pages / 8192 bytes: descriptors at 0, avail at 2048, used at 4096). Furthermore, QEMU in `kos-tool` does not currently instantiate `-device virtio-blk-pci`, so this code path was not executed during live QEMU boots.
5. **Fragile DMA Allocations via Heap**: Both `ahci.c` and `virtio_blk.c` attempt to allocate DMA buffers using `kmalloc()` and then verify physical contiguity via `first_phys + size - 1 == last_phys`. While this succeeds on a freshly booted kernel with unfragmented heap, it is inherently fragile for multi-page buffers (e.g. 64 KiB bounce buffers = 16 pages). PMM already provides `pmm_alloc_pages(count)`, which natively guarantees physically contiguous frame allocation.

---

## 2. Universal Block Device Abstraction

### 2.1 Interface Definition (`include/kos/block.h`)
The block device subsystem provides a uniform, storage-medium agnostic abstraction:

```c
struct block_device_ops {
    bool (*read_blocks)(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer);
    bool (*write_blocks)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
    bool (*flush)(struct block_device *dev);
    void (*destroy)(struct block_device *dev);
};

struct block_device {
    uint32_t id;
    char name[BLOCK_NAME_MAX];       /* e.g. "ramdisk0", "ahci0", "blk0p1" */
    enum block_device_type type;     /* RAMDISK, AHCI, VIRTIO, PARTITION */
    enum block_device_state state;   /* OFFLINE, ONLINE, ERROR */
    uint64_t block_size;             /* Typically 512 bytes */
    uint64_t total_blocks;
    uint64_t capacity_bytes;

    const struct block_device_ops *ops;
    void *private_data;
    kos_spinlock_t lock;
    struct list_head list_node;

    /* Partition relationship (if type == BLOCK_TYPE_PARTITION) */
    struct block_device *parent;
    uint64_t start_lba;
    uint32_t partition_index;
    uint8_t partition_type_mbr;
    uint8_t partition_guid[16];
    bool has_guid;
};
```

### 2.2 Device Registry & Concurrency Control (`drivers/block.c`)
- **Storage Table**: Statically allocated table `block_devices[BLOCK_MAX_DEVICES]` (64 slots max) coupled with a doubly-linked list `block_device_list`.
- **Locking**: Protected by a global spinlock `block_lock` using `spinlock_lock_irqsave` / `spinlock_unlock_irqrestore`. Each device also embeds an individual `dev->lock`.
- **Registration**:
  - `block_device_register()` initializes the descriptor, assigns a monotonically increasing 32-bit `id`, copies the unique name, sets state to `BLOCK_DEVICE_ONLINE`, and links to `block_device_list`.
  - `block_device_unregister()` unregisters any child partitions first, invokes optional `ops->destroy()`, marks the slot `BLOCK_DEVICE_OFFLINE`, and removes it from the list.
- **Lookup APIs**:
  - `block_device_find_by_name(const char *name)`: $O(N)$ string lookup under `block_lock`.
  - `block_device_find_by_id(uint32_t id)`: $O(N)$ ID lookup under `block_lock`.
  - `block_device_count()`, `block_device_get_at(uint32_t index)`.

### 2.3 Partition Sub-Device Layering
Partitions are first-class block devices (`type == BLOCK_TYPE_PARTITION`):
- Registered via `block_partition_register(parent, part_index, start_lba, block_count, part_type_mbr, guid)`.
- Names are formatted as `<parent_name>p<index>` (e.g. `ahci0p1`, `virtio0p1`, `mbr_diskp1`).
- Partition I/O operations (`partition_read_blocks`, `partition_write_blocks`, `partition_flush`) transparently validate partition-relative bounds (`lba + count <= dev->total_blocks`) and forward requests to `block_read(dev->parent, dev->start_lba + lba, count, buffer)`.

### 2.4 Bounds Checking & Defensive Validation
`block_read()`, `block_write()`, and `block_flush()` enforce:
- Non-null pointers (`dev`, `buffer`, `dev->ops`, `dev->ops->read_blocks`).
- Count greater than 0.
- State check: rejects requests if `dev->state != BLOCK_DEVICE_ONLINE`.
- Bounds check: rejects requests if `lba + count > dev->total_blocks`.

---

## 3. RAM Block Device (`ramdisk`)

### 3.1 Implementation (`drivers/ramdisk.c`, `include/kos/ramdisk.h`)
- Backed by an in-memory buffer allocated via `kmalloc(alloc_size)`.
- Data structure:
  ```c
  struct ramdisk_data {
      uint8_t *buffer;
      uint64_t size_bytes;
      bool buffer_allocated;
  };
  ```
- **I/O Operations**:
  - `ramdisk_read_blocks()`: Bounds-checked `kmemcpy(buffer, data->buffer + offset, byte_count)`.
  - `ramdisk_write_blocks()`: Bounds-checked `kmemcpy(data->buffer + offset, buffer, byte_count)`.
  - `ramdisk_flush()`: No-op returning `true`.
  - `ramdisk_dev_destroy()`: Frees `data->buffer` and `data` struct via `kfree()`.
- **Lifecycle Functions**:
  - `ramdisk_create(name, size_bytes, block_size)`: Allocates zeroed buffer and registers device.
  - `ramdisk_destroy(dev)`: Calls `block_device_unregister(dev)`, which calls `ramdisk_dev_destroy()`.

### 3.2 Evaluation
The RAM disk provides an isolated test fixture for all upper layers (MBR parser, GPT parser, Block Cache, FAT32 filesystem engine). It operates completely independently of physical disk controller state and emulator configuration.

---

## 4. PCI Subsystem & Hardware Discovery

### 4.1 Implementation (`arch/x86_64/pci.c`, `include/kos/pci.h`)
- **Architecture**: Standard legacy x86 PCI configuration mechanism #1 using I/O ports `0xCF8` (Address) and `0xCFC` (Data).
- **Scanning (`pci_enumerate`)**:
  - Scans bus 0 through 255, 32 devices per bus, 1 or 8 functions depending on `PCI_HEADER_MULTIFUNCTION` bit in header type.
  - Recursively traverses PCI-to-PCI bridges (`class_code == 0x06`, `subclass == 0x04`) via secondary bus number register (offset `0x19`).
  - Stores up to 128 devices in static array `devices[PCI_MAX_DISCOVERED_DEVICES]`.
- **Device Information Extracted**:
  - Bus, Device, Function.
  - Vendor ID, Device ID (offsets 0x00, 0x02).
  - Class code, Subclass, Programming Interface (offset 0x08).
  - Header Type (offset 0x0E).
  - Interrupt line, Interrupt pin (offset 0x3C, 0x3D).
  - BARs 0..5 (offsets 0x10..0x24) for Header Type 0.
- **Bus Mastering & Memory Space Activation**:
  - `pci_enable_memory_bus_mastering(device)`:
    Reads 32-bit Command/Status register at offset 0x04.
    Sets bit 1 (`Memory Space Enable`) and bit 2 (`Bus Master Enable`).
    Writes back to offset 0x04 and verifies both bits are set.

### 4.2 Findings & Gaps
1. **Device Search by Class/Subclass**: `pci_find_device()` only searches by `(vendor_id, device_id)`. Drivers looking for class-based hardware (e.g., AHCI SATA controller `0x01:0x06`) currently iterate `pci_device_count()` with `pci_device_at(i)`. Adding a helper `pci_find_by_class(class_code, subclass)` would make class-based driver probing cleaner.
2. **64-bit BAR Handling**: `pci->bars[6]` stores 32-bit values. For 64-bit memory BARs (where bits 2:1 are `0b10`), the base address spans `bars[i]` (low 32) and `bars[i+1]` (high 32). In QEMU q35, ABAR (BAR5) and VirtIO BAR0 are 32-bit addresses, so 32-bit extraction works, but a full 64-bit BAR accessor should be noted for future expansion.

---

## 5. MMIO Mapping & Virtual Memory Subsystem

### 5.1 Architecture (`mm/vmm.c`, `include/kos/vmm.h`)
- **Limine Higher Half Direct Map (HHDM)**:
  - Limine bootloader creates a direct linear mapping of all available physical memory at a higher-half virtual offset (`hhdm_offset`, typically `0xffff800000000000`).
  - `vmm_physical_to_virtual(pa)`: Returns `(void *)(vmm.hhdm_offset + pa)`.
  - `vmm_virtual_to_physical(va)`: Returns `(uint64_t)va - vmm.hhdm_offset`.
- **Explicit Page Table Mapping**:
  - `vmm_map_page(va, pa, flags)` / `vmm_map_pages(va, pa, page_count, flags)`.
  - Traverses/allocates 4-level page tables (PML4 -> PDPT -> PD -> PT).
  - Flags supported: `VMM_PAGE_WRITABLE`, `VMM_PAGE_USER`, `VMM_PAGE_NO_EXECUTE`.

### 5.2 Comparative Analysis: MMIO in `e1000.c` vs `ahci.c`
- In `drivers/e1000.c`:
  ```c
  /* Uses a fixed kernel virtual window: 0xffffc10000000000 */
  for (uint64_t offset = 0; offset < E1000_MMIO_SIZE; offset += KOS_PAGE_SIZE) {
      vmm_map_page(E1000_MMIO_VIRTUAL_BASE + offset, physical_base + offset,
                   VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE);
  }
  ```
- In `drivers/ahci.c`:
  ```c
  uint64_t abar_phys = (uint64_t)(pci->bars[5] & ~0xFFFU);
  struct ahci_hba_mem *abar = (struct ahci_hba_mem *)vmm_physical_to_virtual(abar_phys);
  ```
- **Evaluation**:
  - In QEMU OVMF UEFI, the first 4GB of physical address space (including PCI MMIO regions like ABAR) is covered by UEFI/Limine identity/HHDM mappings. Therefore, `vmm_physical_to_virtual(abar_phys)` resolves without faulting in QEMU.
  - However, on hardware where MMIO is above RAM or not covered by Limine's HHDM, `vmm_physical_to_virtual(abar_phys)` would cause a Page Fault.
  - **Best Practice Recommendation**: Standardize MMIO mapping by providing a kernel function `vmm_map_mmio(uint64_t phys_addr, uint64_t size_bytes)` that maps pages into a dedicated MMIO region and returns the virtual pointer.

---

## 6. Physical Memory Management & DMA Allocation

### 6.1 PMM Subsystem Capabilities (`mm/pmm.c`, `include/kos/pmm.h`)
- Bitmap-based physical frame allocation (`pmm.free_bitmap`).
- `pmm_allocate_frame()` / `pmm_alloc_page()`: Allocates single 4 KiB frame.
- `pmm_free_frame(pa)` / `pmm_free_page(pa)`: Frees single 4 KiB frame.
- `pmm_alloc_pages(uint64_t count)`: Scans the allocation bitmap for `count` contiguous free frames and marks them allocated.
- `pmm_free_pages(uint64_t physical_address, uint64_t count)`: Clears bitmap bits for `count` contiguous frames.

### 6.2 Existing DMA Allocation in Drivers (`ahci.c` & `virtio_blk.c`)
Currently, both drivers use an identical pattern:
```c
static bool alloc_dma(uint64_t size, uint64_t alignment, void **raw_out, uint8_t **virt_out, uint64_t *phys_out) {
    uint8_t *raw = (uint8_t *)kmalloc(size + alignment);
    if (raw == NULL) return false;
    uint64_t aligned = ((uint64_t)(uintptr_t)raw + alignment - 1) & ~(alignment - 1);
    uint64_t first_phys, last_phys;
    if (!vmm_query_page(aligned, &first_phys, 0) ||
        !vmm_query_page(aligned + size - 1, &last_phys, 0) ||
        (first_phys + size - 1 != last_phys)) {
        kfree(raw);
        return false;
    }
    *raw_out = raw;
    *virt_out = (uint8_t *)(uintptr_t)aligned;
    *phys_out = first_phys;
    kmemset(*virt_out, 0, size);
    return true;
}
```

### 6.3 Critical Vulnerability in Current DMA Allocation
1. **Multi-page Contiguity Failure**:
   The bounce buffers in both `ahci.c` and `virtio_blk.c` are 64 KiB (`128 * 512` bytes = 16 pages).
   `kmalloc()` allocates memory from the kernel heap (`mm/heap.c`). The heap expands using `grow_heap()`, which calls `pmm_allocate_frame()` in a loop.
   In a newly booted kernel with an empty heap, `pmm_allocate_frame()` allocates consecutive frames.
   However, once the system runs processes, forks threads, or experiences memory fragmentation, `kmalloc()` will allocate pages whose underlying physical frames are **non-contiguous**.
   As a result, `first_phys + size - 1 != last_phys` will fail, and driver initialization or bounce buffer allocation will fail!
2. **Superior Solution**:
   KOS already has `pmm_alloc_pages(count)`!
   For DMA structures and bounce buffers:
   ```c
   static bool dma_buffer_alloc(uint64_t size, void **virt_out, uint64_t *phys_out) {
       uint64_t pages = (size + 4095) / 4096;
       uint64_t phys = pmm_alloc_pages(pages);
       if (phys == 0) return false;
       void *virt = vmm_physical_to_virtual(phys);
       kmemset(virt, 0, pages * 4096);
       *virt_out = virt;
       *phys_out = phys;
       return true;
   }
   ```
   This guarantees physical contiguity, requires 0 alignment padding overhead, eliminates `kmalloc` fragmentation, and takes full advantage of KOS's existing PMM contiguous allocator.

---

## 7. VirtIO Block Driver Detailed Investigation

### 7.1 Driver Architecture (`drivers/virtio_blk.c`)
- **VirtIO Specification**: VirtIO 0.9.5 (Legacy PCI interface).
- **PCI Matching**:
  - Vendor ID: `0x1AF4`
  - Device IDs: `0x1001` (Legacy transitional block device), `0x1042` (Modern block device).
- **PCI Configuration**:
  - Legacy VirtIO exposes control registers via I/O Port space on BAR0 (`pci->bars[0] & ~3U`).
  - Registers: `VIRTIO_REG_DEVICE_STATUS` (0x12), `VIRTIO_REG_QUEUE_SELECT` (0x0E), `VIRTIO_REG_QUEUE_SIZE` (0x0C), `VIRTIO_REG_QUEUE_ADDRESS` (0x08), `VIRTIO_REG_QUEUE_NOTIFY` (0x10), `VIRTIO_REG_CONFIG` (0x14).
- **Device Initialization Sequence**:
  1. Reset device: write `0` to Status register.
  2. Set `ACKNOWLEDGE` bit (1).
  3. Set `DRIVER` bit (2).
  4. Select queue 0: write `0` to Queue Select register, read Queue Size.
  5. Allocate DMA memory for Virtqueue.
  6. Write queue physical PFN (`queue_phys >> 12`) to Queue Address register.
  7. Set `DRIVER_OK` bit (4).
  8. Read disk capacity (in sectors) from Config register (`0x14` low 32 bits, `0x18` high 32 bits).
  9. Register block device `virtioX`.

### 7.2 The Virtqueue Layout Defect (CRITICAL)
In `drivers/virtio_blk.c`:
```c
/* Lines 320-330 */
if (!virtio_alloc_dma_page(4096, &vdev->queue_raw, &vdev->queue_mem, &vdev->queue_phys)) {
    return false;
}
vdev->desc = (struct vring_desc *)vdev->queue_mem;
vdev->avail = (struct vring_avail *)(vdev->queue_mem + (16 * vdev->queue_size));
vdev->used = (struct vring_used *)(vdev->queue_mem + 2048);
```
- **Defect Breakdown**:
  1. When `queue_size = 128`:
     - `desc` spans offset `0 .. 2047` (128 * 16 bytes).
     - `avail` spans offset `2048 .. 2307` (`16 * 128 = 2048`).
     - BUT line 329 sets `vdev->used` to offset `2048`!
     - This means `vdev->avail` and `vdev->used` point to the **exact same memory address**!
  2. In the VirtIO 0.9.5 specification:
     - The used ring MUST be aligned to a 4096-byte boundary:
       $$\text{offset(used)} = \text{ALIGN}(\text{offset(avail)} + 4 + 2 \times qsize, 4096) = 4096$$
     - Therefore, the used ring is located at offset 4096, requiring a 2-page (8192 bytes) allocation.
  3. Because QEMU legacy virtio calculates `vring->used = vring->desc + 4096`:
     - QEMU writes request completion entries to physical offset 4096.
     - KOS only allocated 4096 bytes and is polling offset 2048!
     - Result: If executed against a real VirtIO device, QEMU writes out of bounds or into adjacent memory, while KOS will always time out after 3000ms.
- **Why this wasn't caught in automated testing**:
  In `tools/kos.c`, QEMU is launched with:
  `-drive format=raw,file=fat:rw:build/esp`
  There is NO `-device virtio-blk-pci` in the QEMU command line.
  As a result, `virtio_blk_initialize()` finds 0 matching PCI devices and exits immediately. The driver was never instantiated in QEMU!

### 7.3 Solution for VirtIO Block
1. Change allocation to 8192 bytes (2 contiguous pages via `pmm_alloc_pages(2)`).
2. Set `vdev->desc = (struct vring_desc *)vdev->queue_mem;`
3. Set `vdev->avail = (struct vring_avail *)(vdev->queue_mem + 2048);`
4. Set `vdev->used = (struct vring_used *)(vdev->queue_mem + 4096);`
5. Ensure memory fences (`smp_wmb()`, `smp_rmb()`) around index updates.

---

## 8. AHCI SATA Driver Detailed Investigation

### 8.1 Driver Architecture (`drivers/ahci.c`)
- **PCI Class Code**: `0x01` (Mass Storage), Subclass `0x06` (SATA), Prog-IF `0x01` (AHCI).
- **HBA Memory (ABAR)**:
  - Located via BAR5.
  - Global Host Control (`GHC`): sets `GHC.AE = 1` (bit 31) to enable AHCI controller mode.
  - Implemented Ports: reads 32-bit bitmask `abar->pi`.
- **Port Discovery**:
  - Checks SATA Status (`ssts`): `det == 0x03` (device detected & physical communication established) and `ipm == 0x01` (interface in active state).
  - Checks signature: `port->sig == SATA_SIG_ATA` (`0x00000101`). Ignores ATAPI (`0xEB140101`), SEMB, PM.

### 8.2 Port Initialization & DMA Structures
For each detected active SATA port:
1. **Port Stop (`ahci_port_stop`)**:
   Clears `HBA_PORT_CMD_ST` (bit 0) and `HBA_PORT_CMD_FRE` (bit 4).
   Spins up to 500ms for `CR` (bit 15) and `FR` (bit 14) to clear.
2. **Command List Buffer (`CLB`)**:
   Allocates 1024 bytes aligned to 1024 bytes (32 entries of `struct ahci_cmd_header`, each 32 bytes).
   Writes physical address to `port->clb` (low 32) and `port->clbu` (high 32).
3. **Received FIS Buffer (`FB`)**:
   Allocates 256 bytes aligned to 256 bytes.
   Writes physical address to `port->fb` (low 32) and `port->fbu` (high 32).
4. **32 Command Tables (`CTBA`)**:
   Allocates 32 separate command tables (each 256 bytes, 128-byte aligned).
   Points each `cmd_header[i].ctba / ctbau` to `cmd_tables_phys[i]`.
5. **Bounce Buffer**:
   Allocates 64 KiB (128 sectors of 512 bytes) 4096-byte aligned.
6. **Port Start (`ahci_port_start`)**:
   Clears `serr` and `is`. Sets `FRE` (bit 4) and `ST` (bit 0).

### 8.3 Command Issue & Execution
- **Slot Selection**: Finds clear bit in `(port->sact | port->ci)`.
- **Command FIS Construction (`struct fis_reg_h2d`)**:
  - `fis_type = FIS_TYPE_REG_H2D` (0x27)
  - `pmport_c = (1 << 7)` (Command bit)
  - `device = (1 << 6)` (LBA mode)
  - Populates 48-bit LBA registers (`lba0`..`lba5`) and 16-bit count registers (`countl`, `counth`).
- **PRDT Configuration**:
  - `prdt_entry[0].dba = bounce_phys & 0xFFFFFFFF`
  - `prdt_entry[0].dbau = bounce_phys >> 32`
  - `prdt_entry[0].dbc_i = (byte_count - 1) | (1U << 31)` (Interrupt bit)
- **Command Execution**:
  - Checks if port is hung busy: `port->tfd & (0x80 | 0x08)`.
  - Issues command: `port->ci = (1U << slot)`.
  - Polls `port->ci & (1U << slot)` with 3000ms timeout (`timer_get_ticks() + timer_ms_to_ticks(3000)`).
  - Checks Task File Error Status: `port->is & HBA_PORT_IS_TFES`.
- **Supported ATA Commands**:
  - `0xEC` (`ATA_CMD_IDENTIFY`): Parses 512-byte identify response. Checks word 83 bit 10 for LBA48 support. Calculates total sectors from words 100-103 or words 60-61.
  - `0x25` (`ATA_CMD_READ_DMA_EX`): Reads blocks into bounce buffer, copies to destination.
  - `0x35` (`ATA_CMD_WRITE_DMA_EX`): Copies source to bounce buffer, writes blocks.
  - `0xE7` (`ATA_CMD_FLUSH_CACHE_EX`): Flushes drive cache.

### 8.4 Minor Implementation Flaw in `ahci.c`
In `ahci_probe_port()` line 318:
```c
for (int32_t i = 0; i < 32; i++) {
    if (!ahci_alloc_dma(sizeof(struct ahci_cmd_table), 128,
                        &adev->cmd_table_raw, (uint8_t **)&adev->cmd_tables[i], &adev->cmd_tables_phys[i])) {
        return false;
    }
    adev->cmd_headers[i].ctba = ...
}
```
`&adev->cmd_table_raw` is passed for every iteration, overwriting the raw pointer from previous iterations. If cleanup or freeing were attempted, memory from slots 0..30 would be leaked.

---

## 9. Build System & Compilation Pipeline

### 9.1 Verification of `tools/kos.c`
- **Compiler Configuration**:
  ```
  --target=x86_64-unknown-none-elf -std=c17 -ffreestanding -fno-stack-protector
  -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-80387
  -ffunction-sections -fdata-sections -fno-common -fno-asynchronous-unwind-tables
  -fno-unwind-tables -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
  -g -I include -I third_party/limine-protocol/include -I third_party/font8x8
  ```
- **Strict Compliance**: The build strictly passes `-Wall -Wextra -Werror` with 0 warnings and 0 errors.
- **Source List**:
  `c_sources[]` in `tools/kos.c` explicitly enumerates every source file. All Milestone K16 & K17 files (`drivers/ahci.c`, `drivers/block.c`, `drivers/ramdisk.c`, `drivers/virtio_blk.c`, `fs/block_cache.c`, `fs/fat32.c`, `fs/partition.c`, `fs/volume.c`) are already registered in `c_sources[]`.
- **Linking**: LLD links intermediate ELF with `boot/linker.ld`, extracts symbol table via `llvm-nm`, compiles `kernel_symbols.c`, and performs final link.

---

## 10. Summary Matrix of Storage Components

| Component | Status | Code Location | Primary Capabilities | Gaps / Issues Identified |
|-----------|--------|---------------|----------------------|--------------------------|
| **Block Subsystem** | COMPLETE | `drivers/block.c`<br>`include/kos/block.h` | Generic `struct block_device`, ops dispatch, ID/name indexing, partition layering | None. Robust bounds and concurrency control. |
| **RAM Disk** | COMPLETE | `drivers/ramdisk.c`<br>`include/kos/ramdisk.h` | Dynamic in-memory block device creation/destruction, bounds checking | None. Successfully verified by `blocktest`. |
| **PCI Subsystem** | COMPLETE | `arch/x86_64/pci.c`<br>`include/kos/pci.h` | Configuration ports 0xCF8/0xCFC, recursive bridge scan, bus mastering enable | Lacks `pci_find_by_class()`; only 32-bit BAR extraction. |
| **MMIO Mapping** | FUNCTIONAL | `mm/vmm.c`<br>`include/kos/vmm.h` | Limine HHDM (`vmm_physical_to_virtual`), page table mapping (`vmm_map_page`) | `ahci.c` relies on HHDM for ABAR MMIO; recommend formal `vmm_map_mmio()`. |
| **DMA Allocation** | NEEDS REFACTOR | `drivers/ahci.c`<br>`drivers/virtio_blk.c` | Uses `kmalloc()` + physical contiguity check | Fragile for multi-page buffers (64 KiB bounce buffer). Should use `pmm_alloc_pages()`. |
| **VirtIO-Block** | DEFECTIVE | `drivers/virtio_blk.c`<br>`include/kos/virtio_blk.h` | Legacy VirtIO PCI register access, virtqueue submission, status polling | **Used ring overlaps avail ring at offset 2048** (must be 4096-byte aligned, 2 pages). Not tested in QEMU. |
| **AHCI SATA** | COMPLETE | `drivers/ahci.c`<br>`include/kos/ahci.h` | Port discovery, CLB, FB, Command Tables, PRDT, IDENTIFY, LBA48 DMA Read/Write | Overwrites `cmd_table_raw` in loop; DMA alloc refactoring needed. |

---

## 11. Concrete Recommendations for Milestone K16 & K17 Implementation

1. **Fix Virtqueue Ring Alignment in `drivers/virtio_blk.c`**:
   - Allocate 2 contiguous pages (8192 bytes).
   - Place descriptors at offset 0, available ring at offset 2048, and used ring at offset 4096.
   - Add `-device virtio-blk-pci,drive=drv0` option or a dedicated test disk to QEMU configuration in `tools/kos.c` to test the driver against real virtual hardware.
2. **Refactor DMA Buffer Allocations in `ahci.c` and `virtio_blk.c`**:
   - Create a clean helper `dma_alloc_pages(count, &virt, &phys)` backed directly by `pmm_alloc_pages(count)` and `vmm_physical_to_virtual(phys)`.
   - Use this helper for the 64 KiB bounce buffer, command list, received FIS, and virtqueues.
3. **Fix Command Table Raw Tracking in `ahci.c`**:
   - Change `void *cmd_table_raw;` to `void *cmd_table_raw[32];` or allocate all 32 command tables in a single contiguous buffer `sizeof(struct ahci_cmd_table) * 32`.
4. **Introduce Helper for PCI Class Search**:
   - Add `const struct pci_device *pci_find_device_by_class(uint8_t class_code, uint8_t subclass)` in `arch/x86_64/pci.c`.
