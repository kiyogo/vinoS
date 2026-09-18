# Project: Milestone K13 & K14: Address Space Isolation, Ring 3 Userspace & ELF Loader in KOS x86_64

## Architecture
- **Paging & Address Spaces**: 4-level x86_64 paging (PML4). User space mapped in PML4 entries 0..255 (`0x00000000_00000000 .. 0x00007FFF_FFFFFFFF`). Kernel space mapped in PML4 entries 256..511 (`0xFFFF8000_00000000 .. 0xFFFFFFFF_FFFFFFFF`), synchronized with the kernel master PML4. Lowest 2 MiB unmapped for null pointer safety.
- **Address Space Abstraction**: `struct address_space` and `struct vm_area` embedded in `struct process`. Safe user-space copy routines (`copy_from_user`, `copy_to_user`, `strndup_from_user`) validating addresses against canonical boundaries and using HHDM physical-to-virtual translation to prevent kernel-mode page fault panics.
- **CPU Privilege Transition**: GDT User Code at selector `0x2B` (DPL 3, 64-bit), User Data at selector `0x33` (DPL 3). Static TSS with `tss.rsp0` updated on context switch. Transition via `enter_user_mode` using `iretq` (`SS=0x33, RSP, RFLAGS=0x202, CS=0x2B, RIP`), zeroing all non-argument GPRs.
- **Syscall ABI**: Hardware `syscall`/`sysret` initialized via MSRs (`IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_EFER.SCE`), with `iretq` return path ensuring atomic stack/flags restore and GDT compatibility. Software interrupt `int 0x80` with DPL 3 gate. Core syscalls: `SYS_EXIT=1, SYS_WRITE=2, SYS_READ=3, SYS_GETTIME=4, SYS_SYSINFO=5, SYS_YIELD=6, SYS_SLEEP=7`.
- **ELF64 Loader & Stack Layout**: Upgrade `fs/elf.c` with strict validation, `PT_LOAD` segment mapping with W^X page flags, .bss zeroing. 64 KiB user stack terminating at `0x00007FFFFFFFE000` with 4 KiB guard page at `0x00007FFFFFFED000`. Initial stack frame formatted per System V AMD64 ABI (`argc`, `argv[]`, `NULL`, `envp[]`, `AT_NULL`, argument strings, 16-byte aligned RSP).
- **User Environment & Build**: Freestanding user binary with `user/crt0.asm`, `user/user.ld`, `user/hello.c` compiled and packed into CPIO initramfs (`C:\HELLO.ELF`). Shell `exec <path> [args]` command.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | MSR Read/Write Primitives | Add `cpu_read_msr` and `cpu_write_msr` to `cpu_control.c` and `cpu.h` | M1 | Survey 1 & 2 |
| 2 | `struct address_space` & VMAs | Define `address_space` and `vm_area` structs in `include/kos/address_space.h` and `mm/address_space.c` | M1 | Survey 1 |
| 3 | Address Space Lifecycle | Implement `as_create`, `as_ref`, `as_unref`, `as_destroy` with 100% PMM frame reclamation | M1 | Survey 1 |
| 4 | Safe User Copy Functions | Implement `copy_from_user`, `copy_to_user`, `strndup_from_user` via page-walk HHDM copying | M1 | Survey 1 |
| 5 | Process Address Space Integration | Embed `struct address_space *as` in `struct process`, maintaining `pml4_physical` fast accessor | M1 | Survey 1 |
| 6 | Ring 3 Entry via `iretq` | Implement `enter_user_mode(entry, rsp, argc, argv)` in `arch/x86_64/cpu.asm` | M2 | Survey 2 |
| 7 | Syscall MSR Configuration | Configure `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK` in `syscall_subsystem_init` | M2 | Survey 2 |
| 8 | Syscall Assembly Stub | Implement `syscall_entry` in `cpu.asm` with `TSS.rsp0` switch and atomic `iretq` return | M2 | Survey 2 |
| 9 | Context Switch & TSS Hardening | Ensure `TSS.rsp0` and `CR3` are updated properly on thread and process switches | M2 | Survey 2 |
| 10 | Ring 3 Exception Safety | Verify IDT fault handlers terminate user process on `#PF` / `#GP` with 0 kernel panic | M2 | Survey 2 |
| 11 | Syscall Table Realignment | Update `include/kos/syscall.h` to R4 numbering (`EXIT=1, WRITE=2, READ=3, GETTIME=4, SYSINFO=5, YIELD=6, SLEEP=7`) | M3 | Survey 2 & 3 |
| 12 | Core Syscalls Implementation | Implement `sys_exit`, `sys_write`, `sys_read`, `sys_gettime`, `sys_sysinfo`, `sys_yield`, `sys_sleep` | M3 | Survey 2 |
| 13 | Syscall Test Synchronization | Synchronize `usertest` hardcoded exit opcodes from `mov rax, 4` to `mov rax, 1` in `kernel/command.c` | M3 | Survey 2 & 3 |
| 14 | Host Toolchain Rebuild | Recompile `tools/kos.c` into `tools/kos-tool.exe` with hosted Clang for clean build | M4 | Survey 3 |
| 15 | Standalone User C Runtime | Create `user/crt0.asm` and `user/user.ld` per System V AMD64 ABI | M4 | Survey 3 |
| 16 | Sample User Binary | Create `user/hello.c` invoking `sys_write` and exiting with 0 | M4 | Survey 3 |
| 17 | User Binary Build & Packaging | Integrate user ELF compilation in `tools/kos.c` and package into `build/initramfs.cpio` as `C:\HELLO.ELF` | M4 | Survey 3 |
| 18 | Interactive Shell `exec` Command | Implement `exec <path> [args]` command in `kernel/command.c` | M5 | Survey 3 |
| 19 | E2E Test Suite & usertest Validation | Run and verify 6-part test suite in `command_usertest` + standalone `exec` in QEMU | M5 | Survey 3 |
| 20 | Adversarial Hardening & Leak Audit | Verify zero PMM/heap leaks after 50 process cycles, robust guard page, and exception trapping | M5 | Survey 1, 2, 3 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Memory & Address Space Isolation | MSR primitives, `struct address_space`, VMA tracking, safe user copy functions (`copy_from_user`, etc.), process integration | none | PLANNED |
| M2 | Ring 3 Transition & Syscall Boundary | `enter_user_mode`, MSR syscall setup, `syscall_entry` stub with `iretq`, `TSS.rsp0` context switch, exception safety | M1 | PLANNED |
| M3 | Core Syscalls & ABI Implementation | Syscall table realignment (1..7), core syscall handlers (`sys_write` via safe copy, `sys_exit`, etc.), test opcode synchronization | M1, M2 | PLANNED |
| M4 | User Runtime, Toolchain & Sample Binary | Host toolchain recompile, `user/user.ld`, `user/crt0.asm`, `user/hello.c`, initramfs integration | M3 | PLANNED |
| M5 | Shell Exec, E2E Validation & Hardening | Interactive `exec` shell command, comprehensive `usertest` pass, zero leak audit, final QEMU verification | M1, M2, M3, M4 | PLANNED |

## Interface Contracts

### M1 ↔ M2: Address Space & Hardware Switch
- `struct address_space` allocated via `as_create(void)` provides `uint64_t pml4_phys`.
- `struct process` contains `struct address_space *as` and cached `pml4_physical`.
- `schedule()` and `task_switch()` load `CR3` from `next->process->pml4_physical` (or `kernel_process.pml4_physical` if kernel thread).

### M1 ↔ M3: Safe User Memory Primitives
- `bool user_memory_validate(const void *user_ptr, size_t len, bool write_access)`
- `int copy_from_user(void *dst, const void *src_user, size_t n)` returns 0 on success, `-EFAULT` (-14) on invalid address.
- `int copy_to_user(void *dst_user, const void *src, size_t n)` returns 0 on success, `-EFAULT` (-14) on invalid address.
- `char *strndup_from_user(const char *src_user, size_t max_len)` returns allocated null-terminated string or NULL.

### M2 ↔ M3: Syscall ABI Convention
- Hardware `syscall` enters `syscall_entry` in `arch/x86_64/cpu.asm`.
- Software `int 0x80` enters IDT gate 0x80 in `arch/x86_64/interrupts.c`.
- Both dispatch via `kos_syscall_dispatch(rax, rdi, rsi, rdx, r10, r8, r9)` with return value in `rax`.
- Syscall table:
  1 = `SYS_EXIT(int code)`
  2 = `SYS_WRITE(int fd, const void *buf, size_t count)`
  3 = `SYS_READ(int fd, void *buf, size_t count)`
  4 = `SYS_GETTIME(void)`
  5 = `SYS_SYSINFO(void *info)`
  6 = `SYS_YIELD(void)`
  7 = `SYS_SLEEP(uint64_t ms)`

### M2/M4 ↔ M5: Process Launch & System V Stack Layout
- User entry point invoked via `enter_user_mode(entry_point, user_rsp, argc, argv)`.
- Initial stack at `user_rsp` formatted per System V AMD64 ABI: `argc` at `(%rsp)`, `argv` pointers at `8(%rsp)`, followed by NULL terminator, `envp` pointers (NULL terminated), `AT_NULL` auxiliary vector, and string storage below `USER_STACK_TOP` (`0x00007FFFFFFFE000`).
- Unmapped guard page at `0x00007FFFFFFED000` traps stack overflows.

## Code Layout
- `include/kos/cpu.h`, `arch/x86_64/cpu_control.c`: MSR read/write primitives
- `include/kos/address_space.h`, `mm/address_space.c`: `struct address_space`, VMAs, safe user copy functions
- `include/kos/process.h`, `kernel/process.c`: Process address space lifecycle integration
- `arch/x86_64/cpu.asm`: `enter_user_mode`, `syscall_entry`, `user_rsp_scratch`
- `include/kos/descriptors.h`, `arch/x86_64/descriptors.c`: GDT, TSS, `TSS.rsp0`
- `include/kos/syscall.h`, `kernel/syscall.c`: Syscall table and implementations
- `tools/kos.c`, `tools/kos-tool.exe`: Host build toolchain
- `user/user.ld`, `user/crt0.asm`, `user/hello.c`: User C runtime and sample binary
- `kernel/command.c`: `exec` shell command, `usertest` test harness
