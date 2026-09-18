# Handoff Report: Explorer Survey 2 (Milestone K14)

**Agent**: `teamwork_preview_explorer_survey_2`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_2`  
**Mission**: Investigate KOS x86_64 VMM paging, address space management, ELF64 binary parsing/loading, and user stack configuration for Milestone K14.  
**Report Type**: Hard Handoff (Investigation Complete)

---

## 1. Observation

### 1.1 VMM & Page Table Management
- **File**: `include/kos/vmm.h`, lines 7–10:
  ```c
  enum {
      VMM_PAGE_WRITABLE = 1ull << 1,
      VMM_PAGE_NO_EXECUTE = 1ull << 63,
  };
  ```
  `VMM_PAGE_USER` (bit 2, `1ull << 2`) is not defined.
- **File**: `mm/vmm.c`, lines 115–119:
  ```c
  bool vmm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
      if (virtual_address % PAGE_SIZE != 0 || physical_address % PAGE_SIZE != 0
          || (flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) != 0) {
          return false;
      }
  ```
  Any flag other than `VMM_PAGE_WRITABLE` and `VMM_PAGE_NO_EXECUTE` is rejected with `return false`.
- **File**: `mm/vmm.c`, line 97 (`next_table`):
  ```c
  *entry = frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE;
  ```
  Intermediate paging directory entries (PML4E, PDPTE, PDE) are allocated without `PAGE_USER` (bit 2).
- **File**: `mm/vmm.c`, lines 21–25:
  ```c
  struct virtual_memory_manager {
      uint64_t hhdm_offset;
      uint64_t *pml4;
      bool initialized;
  };
  static struct virtual_memory_manager vmm;
  ```
  `vmm.pml4` is a single static pointer initialized from the boot CR3 in `vmm_initialize()`. No per-process address space functions (`vmm_create_address_space`, `vmm_destroy_address_space`, `vmm_map_page_in`) exist.
- **File**: `kernel/process.c`, line 139 (`process_create`):
  ```c
  proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;
  ```
  Every new process points to the bootloader's single page table. In `process_destroy()`, `proc->pml4_physical` is ignored and never reclaimed.
- **File**: `kernel/task.c`, lines 298–301 (`schedule`):
  ```c
  current_task = next;
  task_switch(&previous->context, &next->context);
  ```
  `schedule()` does not update CR3 on context switch and does not call `tss_set_rsp0()`.
- **File**: `include/kos/cpu.h` & `arch/x86_64/cpu_control.c`:
  `cpu_read_cr3()` exists (line 70 of `cpu_control.c`), but there is no `cpu_write_cr3()`.

### 1.2 Virtual Address Space Layout
- **File**: `boot/linker.ld`, lines 16, 28, 45, 52:
  Kernel text, rodata, data, and bss are linked at `0xffffffff80000000` (PML4 index 511).
- **File**: `mm/heap.c`, line 12:
  `HEAP_BASE = 0xffffffffc0000000ull` (PML4 index 511).
- **File**: `boot/limine_requests.c`, line 39:
  `kos_hhdm_request` maps physical memory in higher-half virtual space (PML4 index $\ge 256$).
- Lower half (PML4 indices 0..255, addresses `0x0000000000000000` to `0x00007fffffffffff`) is unused by the kernel.

### 1.3 ELF Support
- **File**: `include/kos/elf.h`, lines 7–13:
  ```c
  struct elf_image_info {
      uint64_t entry_point;
      uint16_t program_header_count;
      uint16_t loadable_segment_count;
  };
  bool elf_inspect_image(const uint8_t *image, uint64_t image_size, struct elf_image_info *info);
  ```
- **File**: `fs/elf.c`, lines 42–82:
  `elf_inspect_image()` checks magic, 64-bit, little-endian, machine x86_64, and counts `PT_LOAD` segments. It does not allocate memory, map pages, check segment flags (`PF_X`, `PF_W`, `PF_R`), handle BSS zeroing, or support binary loading into an address space.
- Standard `Elf64_Ehdr` and `Elf64_Phdr` structures are absent from `include/kos/elf.h`.

### 1.4 Binary Storage
- **File**: `boot/limine.conf`, lines 11–12:
  ```
  module_path: boot():/boot/initramfs.cpio
  module_string: kos-initramfs
  ```
- **File**: `kernel/main.c`, lines 337–344:
  `initramfs_initialize()` parses the CPIO newc archive from Limine; `vfs_mount_root(&initramfs_vfs_backend)` mounts it as root volume `C:`. `ramfs_initialize()` mounts `D:` as read-write RAM volume.
- **File**: `include/kos/vfs.h`, line 32:
  `bool vfs_read_file(const char *path, const uint8_t **data, uint64_t *size);` provides direct memory pointer access to any file on `C:` or `D:`.
- **File**: `tools/kos.c`, lines 224–254:
  `make_initramfs()` packages files into `build/initramfs.cpio` using CPIO newc format.

### 1.5 Fault Handling in User Mode
- **File**: `arch/x86_64/interrupts.c`, lines 259–269 (`page_fault_handler`):
  ```c
  } else {
      /* User mode page fault (Ring 3) */
      log_enter_emergency_mode();
      log_emergency_puts("\r\n==================== USER PROCESS PAGE FAULT ====================\r\n");
      log_error("PAGE FAULT: Access violation in User Mode (Ring 3)");
      log_info_hex("Faulting Address (CR2): ", cr2);
      ...
      log_error("User space process termination not yet available in K12 -> System halted.");
      cpu_halt();
  }
  ```
  User mode page faults unconditionally halt the CPU (`cpu_halt()`).

### 1.6 Automated Build Status
- **Command**: `.\tools\kos-tool.exe build`
- **Result**: Exited with code `0`. "Kernel ELF built with 651 embedded symbols: build/kos.elf. UEFI ESP staged: build/esp." Zero warnings, zero errors.

---

## 2. Logic Chain

1. **Address Space Isolation (from 1.1 & 1.2)**:
   Because the kernel and HHDM reside strictly in PML4 indices 256..511 ($va \ge \text{0xffff800000000000}$), allocating a new PML4 frame, zeroing entries 0..255 (user space), and copying entries 256..511 from `vmm.pml4` instantly gives every user process a private 128 TiB user space while cleanly sharing all kernel page tables without duplicating PDPT/PD/PT tables.
2. **Ring 3 Hardware Protection (from 1.1)**:
   In AMD64 paging, user mode access requires the $U/S$ bit (bit 2) set on **every** level of the translation hierarchy (PML4E, PDPTE, PDE, PTE). Because `next_table()` in `mm/vmm.c:97` leaves bit 2 clear, and `vmm_map_page()` strictly filters out bit 2, Ring 3 execution cannot access user memory until:
   - `VMM_PAGE_USER = 1ull << 2` is added to `include/kos/vmm.h`.
   - `vmm_map_page` and intermediate directory allocations (`next_table`) propagate `VMM_PAGE_USER` for user mappings ($va < \text{0x0000800000000000}$).
3. **Context Switching & TSS (from 1.1)**:
   When multitasking between user processes, switching threads requires:
   - Loading `CR3` with `next->process->pml4_physical` via `cpu_write_cr3()`.
   - Updating `TSS.rsp0` with `next->stack + next->stack_size` via `tss_set_rsp0()` so that asynchronous interrupts (e.g. timer IRQ) or exceptions occurring while in Ring 3 safely use the thread's own kernel stack.
4. **ELF64 Loader Pipeline (from 1.3 & 1.4)**:
   `vfs_read_file()` provides zero-copy memory buffers of ELF files from `C:` (initramfs) or `D:` (ramfs). An ELF loader (`elf_load_binary`) can parse `Elf64_Ehdr`, validate static `ET_EXEC` headers and segment alignment, iterate over `PT_LOAD` segments, allocate physical frames from `pmm_allocate_frame()`, zero BSS, and map pages into the process's private PML4 with exact permissions (`VMM_PAGE_USER`, `VMM_PAGE_WRITABLE`, `VMM_PAGE_NO_EXECUTE`).
5. **User Stack & Guard Page (from 1.1 & 1.5)**:
   By defining `USER_STACK_TOP = 0x00007fffffffe000ull` and allocating 64 KiB (`USER_STACK_SIZE`), mapping pages downwards, and leaving `guard_page = stack_bottom - PAGE_SIZE` unmapped (`PAGE_PRESENT = 0`), any stack overflow triggers vector 14 (#PF). Updating `page_fault_handler()` to call `process_exit(proc, -1)` instead of `cpu_halt()` satisfies the acceptance criterion that stack overflows cleanly terminate the faulting process without crashing the kernel.
6. **System V AMD64 ABI Initial Stack (from 1.1)**:
   Structuring initial stack memory so that `argc`, `argv[]`, `envp[]`, auxiliary vector `AT_NULL`, and strings are pushed before entry, with `(%rsp & 0xf) == 0`, guarantees standard C user programs can execute and access arguments correctly.

---

## 3. Caveats

1. **Dynamic Shared Objects**: Dynamic linking (`PT_INTERP` / `ld.so`) is out of scope for K14 R1; all user binaries must be statically linked (`-static`).
2. **On-Demand Demand Paging**: K14 R1 specifies eager allocation of all `PT_LOAD` frames and user stack frames during `process_exec`/`elf_load_binary`. Demand-paging / lazy page allocation can be deferred to a later milestone.
3. **Huge Pages**: `mm/vmm.c` supports 2 MiB and 1 GiB huge pages for kernel mappings; user pages should strictly use standard 4 KiB pages for granular protection.
4. **No other caveats**.

---

## 4. Conclusion

The existing KOS architecture possesses all physical memory management (`pmm.c`), VFS/initramfs file loading (`initramfs.c`, `vfs.c`), IDT/GDT user segment selectors (`KOS_USER_CODE_SELECTOR = 0x2b`, `KOS_USER_DATA_SELECTOR = 0x33`), and TSS infrastructure (`tss_set_rsp0`) needed to support Milestone K14.

To complete Requirement R1 (ELF64 Binary Loader & Address Space Mapping), the implementer must execute the following scoped changes:
1. **`include/kos/vmm.h` & `mm/vmm.c`**:
   - Define `VMM_PAGE_USER = 1ull << 2`.
   - Implement `vmm_create_address_space()`, `vmm_destroy_address_space()`, `vmm_map_page_in()`.
   - Propagate `VMM_PAGE_USER` in `next_table()` for user mappings.
2. **`include/kos/cpu.h` & `arch/x86_64/cpu_control.c`**:
   - Add `cpu_write_cr3(uint64_t cr3)`.
3. **`include/kos/elf.h` & `fs/elf.c`**:
   - Define `Elf64_Ehdr`, `Elf64_Phdr`, `PT_*`, `PF_*`.
   - Implement `elf_load_binary()` with strict validation (magic, 64-bit, static `ET_EXEC`, canonical range `0x00200000` to `0x00007fffffffffff`, non-overlapping segments, BSS zeroing, graceful error unwinding).
4. **`kernel/process.c` & `kernel/task.c`**:
   - Use `vmm_create_address_space()` in `process_create()` and `vmm_destroy_address_space()` in `process_destroy()`.
   - Implement `process_setup_user_stack()` (64 KiB stack, unmapped guard page, System V AMD64 ABI layout with 16-byte alignment).
   - In `schedule()`, update CR3 if the process address space changes, and update `tss_set_rsp0()`.
5. **`arch/x86_64/interrupts.c`**:
   - In `page_fault_handler()`, replace `cpu_halt()` on user-mode faults (`PF_ERROR_USER`) with `process_exit(proc, -1)` and `schedule()`.

Detailed architectural designs, layouts, and algorithms are recorded in:
`d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_2\analysis.md`.

---

## 5. Verification Method

### 5.1 Independent Code Inspections
1. Inspect `include/kos/vmm.h` and `mm/vmm.c:117` to confirm `VMM_PAGE_USER` definition and flag validation mask.
2. Inspect `include/kos/cpu.h` and `arch/x86_64/cpu_control.c` to confirm `cpu_write_cr3()` exists and executes `mov %0, %%cr3`.
3. Inspect `fs/elf.c` to verify that `elf_load_binary()` validates `e_entry`, `p_vaddr`, `p_memsz`, and rejects `PT_INTERP` or overlapping segments.
4. Inspect `arch/x86_64/interrupts.c:255-269` to verify that user-mode page faults invoke `process_exit()` instead of `cpu_halt()`.

### 5.2 Build Verification
Run the KOS host build orchestrator:
```powershell
.\tools\kos-tool.exe build
```
*Expected Result*: Exit code 0, all C and NASM compilation units compile without warnings (`-Wall -Wextra -Werror`), intermediate symbol pass passes, and `build/kos.elf` + `build/esp` are generated.

### 5.3 Invalidation Conditions
- Any change that permits user mappings in the higher half ($va \ge \text{0x0000800000000000}$).
- Any implementation where intermediate page tables (PML4E, PDPTE, PDE) omit `VMM_PAGE_USER` for user addresses, causing premature supervisor faults.
- Any implementation of `schedule()` that fails to update `tss_set_rsp0()`, causing Ring 3 interrupt stack corruption.
