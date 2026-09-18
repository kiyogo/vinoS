# Dispatch: Worker M1 (Hardware & Paging Primitives)

## Mission
Implement Milestone M1: Hardware & Paging Primitives for KOS x86_64:
1. `include/kos/vmm.h` & `mm/vmm.c`:
   - Define `VMM_PAGE_USER = (1ull << 2)` in `include/kos/vmm.h`.
   - Update `vmm_map_page()` validation mask to allow `VMM_PAGE_USER`.
   - Update `next_table()` in `mm/vmm.c` so intermediate tables (PML4E, PDPTE, PDE) include `VMM_PAGE_USER` if mapping user addresses (`virtual_address < 0x0000800000000000ull` or if `flags & VMM_PAGE_USER`).
2. `include/kos/cpu.h` & `arch/x86_64/cpu_control.c`:
   - Declare and implement `void cpu_write_cr3(uint64_t cr3);` with inline assembly:
     `__asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");`
3. `include/kos/descriptors.h`:
   - Add `#define GDT_USER_CS 0x2b` and `#define GDT_USER_DS 0x33` (or enum aliases to match `KOS_USER_CODE_SELECTOR` and `KOS_USER_DATA_SELECTOR`).
4. `kernel/task.c`:
   - In `schedule()`, update TSS RSP0 on every context switch:
     `tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);`
5. `net/socket.c`:
   - In `net_udp_open()`, fix the bug identified by Explorer 3: line ~186 `sockets[index] = (struct udp_socket){ .open = true };` overwrites the struct without initializing the wait queue. Call `wait_queue_init(&sockets[index].wait_queue);` so loopback self-test does not crash dereferencing NULL.
6. Build verification:
   - Run `tools\kos-tool.exe build` and verify 0 warnings and 0 errors.

## Mandatory Integrity Warning
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

## Working Directory
`d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_worker_m1`
## 2026-09-18T00:29:49Z
Mission: Milestone M1 (Hardware & Paging Primitives)
Files you own exclusively:
- include/kos/vmm.h & mm/vmm.c
- include/kos/cpu.h & arch/x86_64/cpu_control.c
- include/kos/descriptors.h
- kernel/task.c
- net/socket.c
