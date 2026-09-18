# Changes Summary — Milestone M1 (Hardware & Paging Primitives)

## Files Modified

### 1. `include/kos/vmm.h`
- Added `VMM_PAGE_USER = 1ull << 2` to enum and defined `#define VMM_PAGE_USER (1ull << 2)`.
- Exported user page permission constant for virtual memory mapping.

### 2. `mm/vmm.c`
- Added `PAGE_USER = 1ull << 2` to the internal page table flag enum.
- Updated `combine_effective_flags()` to handle `VMM_PAGE_USER` across parent page table entries.
- Updated `next_table()` signature to `static uint64_t *next_table(uint64_t *entry, bool *created, uint64_t virtual_address, uint64_t flags)`.
  - When allocating a new intermediate table (PML4E, PDPTE, PDE), sets `PAGE_USER` (`1ull << 2`) if `virtual_address < 0x0000800000000000ull` or `(flags & VMM_PAGE_USER) != 0`.
  - If the intermediate table entry is already present, asserts/sets `PAGE_USER` if mapping user space so user translation succeeds through the hierarchy.
- Updated `vmm_map_page()` to allow `VMM_PAGE_USER` in flag validation: `flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE)`.
- Updated call sites of `next_table()` in `vmm_map_page()` to pass `virtual_address` and `flags`.
- Updated `set_leaf_permissions()` and `vmm_set_page_permissions_unlocked()` to allow `VMM_PAGE_USER`.

### 3. `include/kos/cpu.h`
- Declared `void cpu_write_cr3(uint64_t cr3);`.

### 4. `arch/x86_64/cpu_control.c`
- Implemented `void cpu_write_cr3(uint64_t cr3)` using inline assembly:
  ```c
  void cpu_write_cr3(uint64_t cr3) {
      __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
  }
  ```

### 5. `include/kos/descriptors.h`
- Added selector aliases:
  ```c
  #define GDT_KERNEL_CS 0x08
  #define GDT_KERNEL_DS 0x10
  #define GDT_USER_CS   0x2b
  #define GDT_USER_DS   0x33
  ```

### 6. `kernel/task.c`
- Added `#include <kos/descriptors.h>`.
- In `schedule()`, on context switch before `task_switch(&previous->context, &next->context)`, added:
  ```c
  if (next->stack != NULL) {
      tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
  } else {
      extern uint8_t kos_boot_stack_top[];
      tss_set_rsp0((uint64_t)kos_boot_stack_top);
  }
  ```
  Guarantees that hardware interrupts and exceptions from Ring 3 land on the thread's own kernel stack, falling back to `kos_boot_stack_top` for the bootstrap task.

### 7. `net/socket.c`
- In `net_udp_open()`, after `sockets[index] = (struct udp_socket){ .open = true };`, called:
  ```c
  wait_queue_init(&sockets[index].wait_queue);
  ```
  Fixes uninitialized wait queue list head and lock corruption on socket allocation.

## Verification
- Built cleanly with `tools\kos-tool.exe build` (exit code 0, 0 warnings, 0 errors).
