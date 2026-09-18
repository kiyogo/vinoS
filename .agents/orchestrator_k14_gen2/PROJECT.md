# Project: KOS Milestone K14 — Process and Ring 3 Userspace

## Architecture
Milestone K14 implements the user-mode execution and process lifecycle layer for KOS x86_64:
- **Paging & Address Space Isolation**: Lower-half virtual address space (0x0000000000000000 to 0x00007FFFFFFFFFFF) is private to each process. Higher-half (0xFFFF800000000000 to 0xFFFFFFFFFFFFFFFF) contains the shared kernel and direct physical mappings. `vmm_create_address_space()` creates a new PML4 cloning higher-half entries 256..511 and zeroing 0..255. `vmm_destroy_address_space()` recursively frees user page tables and mapped frames.
- **Hardware Privilege Transition**: `enter_userspace(entry_point, user_rsp)` sets up the user data segments (`0x33`), scrubs general-purpose registers, formats an `iretq` frame (SS=0x33, RSP=user_rsp, RFLAGS=0x202 with IF=1, CS=0x2b, RIP=entry_point), and executes `iretq`.
- **TSS & RSP0 Kernel Stack Switching**: On every thread context switch in `schedule()`, `tss_set_rsp0(next->stack + next->stack_size)` is called so any hardware interrupt (PIT timer, keyboard) or exception occurring while CPL=3 safely switches to that thread's kernel stack.
- **Fault Containment**: `page_fault_handler()` and exception dispatchers check if the fault originated in user mode (`(context->cs & 3) == 3` or `error_code & PF_ERROR_USER`). If so, they log the violation and cleanly terminate the faulting process via `process_exit(proc, -11)` rather than halting or panicking the kernel.
- **ELF64 Loader**: Statically linked x86_64 ELF binaries (`ET_EXEC`) are validated for magic, machine, 64-bit, endianness, entry point, alignment, and bounds. All `PT_LOAD` segments are allocated via PMM and mapped with permissions (`VMM_PAGE_USER`, `VMM_PAGE_WRITABLE`, `VMM_PAGE_NO_EXECUTE`).
- **User Stack & Guard Page**: Dedicated 64 KiB user stack allocated at `USER_STACK_TOP = 0x00007FFFFFFFE000ull`, with an unmapped bottom guard page (`0x00007FFFFFFEE000ull`). Initial stack formatted according to System V AMD64 ABI (`argc`, `argv`, `envp`, auxiliary vector `AT_NULL`).
- **Process & Thread Lifecycle**: `process_t` tracks PID, PPID, state (READY, RUNNING, BLOCKED, ZOMBIE, DEAD), `children` list, `sibling_node`, `threads` list, `pml4_physical`, and `exit_wq`. Parent processes wait via `process_wait()`, retrieve child exit status, and reap zombie processes.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Paging User Flag & CR3 Control | Define `VMM_PAGE_USER (1ull << 2)`, propagate user bit in `next_table()`, implement `cpu_write_cr3()`, fix UDP socket wait_queue boot bug | M1 | Survey 2 & 3 |
| 2 | TSS RSP0 Context Switch Update | Update `tss_set_rsp0(next_rsp0)` on every thread context switch in `schedule()` | M1 | Survey 1 & 3 |
| 3 | Per-Process Address Spaces | Implement `vmm_create_address_space()` and recursive `vmm_destroy_address_space()` freeing user frames | M2 | Survey 2 |
| 4 | ELF64 Binary Loader | Implement `elf_load_binary()` in `fs/elf.c` with strict validation, `PT_LOAD` segment mapping, BSS zeroing, error unwinding | M2 | Survey 2 |
| 5 | User Stack & Guard Page | Allocate 64 KiB stack at `USER_STACK_TOP`, unmapped guard page at bottom, format System V AMD64 ABI layout | M2 | Survey 2 |
| 6 | Ring 3 Transition Primitive | Implement `enter_userspace(entry, rsp)` in `arch/x86_64/cpu.asm` with `iretq` and register scrubbing | M3 | Survey 1 |
| 7 | User Fault Containment | In `arch/x86_64/interrupts.c`, handle user faults (Page Fault, GPF, UD) by terminating process via `process_exit(cur, -11)` instead of `cpu_halt()` | M3 | Survey 1 & 2 |
| 8 | Process Hierarchy & Reaping | Add `children` / `sibling_node` to `struct process`; ensure zombie processes remain until reaped by `process_wait()` | M4 | Survey 3 |
| 9 | `process_exec` Primitive | Implement `process_exec(path, argc, argv)` creating address space, loading ELF, setting up user stack, and entering Ring 3 | M4 | Survey 3 |
| 10 | Resource Reclamation | Ensure full teardown of page tables, user stacks, handle tables, and open objects on process termination without memory leaks | M4 | Survey 3 |
| 11 | Shell Test Suite (`usertest`) | Shell command running 6 subtests: valid ELF, corrupted ELF, kernel memory access, stack overflow, timer preemption, 50-iteration leak test | M5 | Survey 3 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Hardware & Paging Primitives | VMM_PAGE_USER, cpu_write_cr3, TSS RSP0 update in schedule(), net_udp wait_queue init fix | none | DONE |
| M2 | VMM Address Space & ELF64 Loader | vmm_create/destroy_address_space, elf_load_binary, user stack & guard page, System V ABI | M1 | DONE |
| M3 | Ring 3 Transition & Fault Recovery | enter_userspace (iretq), user page fault & exception routing to process_exit | M1, M2 | DONE |
| M4 | Process Lifecycle & Reclamation | process_exec, parent/child hierarchy, zombie reaping, full resource cleanup | M2, M3 | DONE |
| M5 | Automated Build & QEMU E2E Verification | usertest command, acceptance criteria validation, QEMU test runs | M1, M2, M3, M4 | IN_PROGRESS |

## Interface Contracts

### VMM ↔ Process / ELF Loader
```c
/* include/kos/vmm.h */
#define VMM_PAGE_USER (1ull << 2)

uint64_t vmm_create_address_space(void);
void vmm_destroy_address_space(uint64_t pml4_phys);
bool vmm_map_page_in(uint64_t pml4_phys, uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
```

### CPU Control ↔ Scheduler
```c
/* include/kos/cpu.h */
void cpu_write_cr3(uint64_t cr3);
void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn));
```

### ELF Loader ↔ VFS / Process
```c
/* include/kos/elf.h */
struct elf_binary_image {
    uint64_t entry_point;
    uint64_t user_stack_top;
    uint64_t user_stack_bottom;
};

bool elf_load_binary(const char *path, uint64_t pml4_phys, struct elf_binary_image *out_image);
```

### Process Lifecycle
```c
/* include/kos/process.h */
int process_exec(const char *path, int argc, char *const argv[]);
bool process_wait(struct process *proc, int *exit_code, uint64_t timeout_ms);
void process_exit(struct process *proc, int exit_code) __attribute__((noreturn));
```

## Code Layout
- `include/kos/vmm.h`, `mm/vmm.c`: Paging flags (`VMM_PAGE_USER`), address space creation/destruction, page table mapping
- `include/kos/cpu.h`, `arch/x86_64/cpu_control.c`, `arch/x86_64/cpu.asm`: `cpu_write_cr3()`, `enter_userspace()`
- `include/kos/descriptors.h`, `arch/x86_64/descriptors.c`: TSS RSP0 setting, user segment selectors
- `include/kos/elf.h`, `fs/elf.c`: ELF64 header structures, validator, and loader
- `include/kos/process.h`, `kernel/process.c`: Process structure, hierarchy, `process_exec()`, `process_wait()`, zombie reaping, cleanup
- `include/kos/task.h`, `kernel/task.c`: Thread context switch, `schedule()`, TSS RSP0 update, CR3 switch
- `arch/x86_64/interrupts.c`: User page fault and exception handling (containment to process_exit)
- `kernel/command.c`: `usertest` command implementation
- `net/socket.c`: UDP socket wait queue initialization fix
