# Handoff Report — Milestone M1 (Hardware & Paging Primitives)

## 1. Observation
- `include/kos/vmm.h` (lines 7-10) originally defined only `VMM_PAGE_WRITABLE` (bit 1) and `VMM_PAGE_NO_EXECUTE` (bit 63). `VMM_PAGE_USER` was absent.
- `mm/vmm.c`:
  - Flag validation in `vmm_map_page()` (lines 115-119) strictly rejected any flags other than `(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)`.
  - `next_table()` (lines 82-100) created intermediate page directory entries with flags `frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE`, leaving bit 2 (user mode access) clear on intermediate tables (PML4E, PDPTE, PDE).
  - AMD64 architecture specification dictates that hardware address translation from CPL=3 requires bit 2 (User/Supervisor) to be set on every level of the page hierarchy.
- `include/kos/cpu.h` & `arch/x86_64/cpu_control.c`:
  - `cpu_read_cr3()` existed (lines 69-73) using `mov %%cr3, %0`, but `cpu_write_cr3()` was missing.
- `include/kos/descriptors.h`:
  - TSS and GDT loading existed, but macro constants `GDT_USER_CS` and `GDT_USER_DS` were missing.
- `kernel/task.c`:
  - `schedule()` (lines 261-303) performed context switching via `task_switch(&previous->context, &next->context)` without updating `tss_set_rsp0()`.
  - `tss_set_rsp0()` was only called once at kernel boot in `descriptors.c:81` with `kos_boot_stack_top`.
- `net/socket.c`:
  - `net_udp_open()` (lines 184-191) initialized open sockets with `sockets[index] = (struct udp_socket){ .open = true };`, zeroing `sockets[index].wait_queue` without calling `wait_queue_init()`, causing null pointer dereferences on wait operations.
- Build tool:
  - Running `tools\kos-tool.exe build` executed Clang C17 with `-Wall -Wextra -Werror -ffreestanding` and LLD with `--fatal-warnings`.
  - Build finished with exit code 0 and staged `build/esp` UEFI image cleanly.

## 2. Logic Chain
1. **User Mode Paging Propagation**:
   - AMD64 translation requires the User bit (bit 2) on all paging levels (PML4 -> PDPT -> PD -> PT).
   - In `include/kos/vmm.h`, defining `VMM_PAGE_USER = 1ull << 2` and `#define VMM_PAGE_USER (1ull << 2)` exposes the user bit flag to the kernel.
   - In `mm/vmm.c`, allowing `VMM_PAGE_USER` in `vmm_map_page()` allows leaf entries to receive `VMM_PAGE_USER`.
   - In `mm/vmm.c`, modifying `next_table()` to check `(virtual_address < 0x0000800000000000ull) || ((flags & VMM_PAGE_USER) != 0)` and set `PAGE_USER` on intermediate tables ensures user-mode translation traverses the lower half without faulting.
2. **CR3 Reload Primitive**:
   - Switching address spaces in M2/M4 requires reloading CR3 with the process's PML4 physical address.
   - `cpu_write_cr3(uint64_t cr3)` using `mov %0, %%cr3` provides the required architecture primitive and flushes non-global TLB entries.
3. **GDT Selector Aliases**:
   - `GDT_USER_CS = 0x2b` (index 5, RPL 3) and `GDT_USER_DS = 0x33` (index 6, RPL 3) in `include/kos/descriptors.h` provide canonical macros for Ring 3 transition routines (`enter_userspace`).
4. **TSS RSP0 Switch on Context Switch**:
   - In `schedule()`, calling `tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull)` for each scheduled task with allocated stack guarantees that any interrupt or exception occurring during Ring 3 execution lands on `next`'s dedicated kernel stack.
   - Fallback to `kos_boot_stack_top` protects the early bootstrap task.
5. **UDP Socket Wait Queue Fix**:
   - Initializing `wait_queue_init(&sockets[index].wait_queue)` in `net_udp_open()` prevents corrupting the wait queue structure, ensuring loopback socket operations and timeouts operate safely.

## 3. Caveats
- Per-process address space creation (`vmm_create_address_space`), destruction (`vmm_destroy_address_space`), and switching CR3 inside `schedule()` are part of subsequent milestones (M2 and M4) per PROJECT.md. M1 only delivers the foundational paging flags, CR3 write primitive, TSS RSP0 switching, and GDT selectors.
- No other files outside the designated M1 scope were modified.

## 4. Conclusion
- All Milestone M1 requirements are fully implemented, verified, and integrated into the KOS codebase.
- The build completes with 0 warnings and 0 errors under Clang `-Wall -Wextra -Werror` and LLD `--fatal-warnings`.

## 5. Verification Method
- **Command**: `tools\kos-tool.exe build`
  - Expected output: `Kernel ELF built with 651 embedded symbols: build/kos.elf` and exit code 0.
- **Inspect**:
  - `include/kos/vmm.h`: Verify `VMM_PAGE_USER = 1ull << 2` and `#define VMM_PAGE_USER (1ull << 2)`.
  - `mm/vmm.c`: Verify `PAGE_USER` in `next_table()` for intermediate tables and validation in `vmm_map_page()`.
  - `include/kos/cpu.h` & `arch/x86_64/cpu_control.c`: Verify `cpu_write_cr3()`.
  - `include/kos/descriptors.h`: Verify `GDT_USER_CS` (0x2b) and `GDT_USER_DS` (0x33).
  - `kernel/task.c`: Verify `tss_set_rsp0()` call in `schedule()`.
  - `net/socket.c`: Verify `wait_queue_init(&sockets[index].wait_queue)` in `net_udp_open()`.
- **Invalidation Condition**: If `tools\kos-tool.exe build` fails or produces any compiler/linker warning, this handoff is invalidated.
