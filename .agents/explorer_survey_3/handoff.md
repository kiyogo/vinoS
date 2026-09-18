# Technical Survey & Handoff Report: ELF64 Binary Loader, User Memory Layout, Build Toolchain & Userspace Test Harness (Milestones K13 & K14)

**Agent**: `explorer_survey_3`  
**Role**: ELF64 Loader & User Binary Environment Explorer  
**Milestone**: K13 & K14 (Address Space Isolation, Ring 3 Userspace & ELF Loader)  
**Date**: 2026-09-18T10:15:00Z  
**Project Root**: `D:\DATA\source\OS\KOS\OS2`  
**Working Directory**: `D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_3`

---

## Executive Summary

KOS x86_64 has already established a functional foundation for ELF64 loading, Ring 3 execution, user stack configuration, and process isolation. Specifically:
1. **ELF64 Loader (`fs/elf.c`, `include/kos/elf.h`)**: Fully parses 64-bit ELF headers and `PT_LOAD` program headers, enforces validation (magic, architecture, machine, boundary ranges, alignment, non-overlapping segments, executable entry points), maps segments into private process address spaces with page-level permissions (`VMM_PAGE_USER`, `VMM_PAGE_WRITABLE`, `VMM_PAGE_NO_EXECUTE`), and zeroes `.bss` memory.
2. **User Process Memory Layout & Stack (`fs/elf.c`, `include/kos/elf.h`)**: Sets up a 64 KiB user stack terminating at `0x00007FFFFFFFE000`, protected by an unmapped 4 KiB guard page at `0x00007FFFFFFED000`. Formats the initial user stack per the System V AMD64 ABI with 16-byte alignment, command-line argument strings (`argv`), pointers array (`argv[]`), NULL terminators, and auxiliary vectors (`AT_NULL`).
3. **Ring 3 Hardware Context & Privilege Transition (`arch/x86_64/cpu.asm`, `kernel/process.c`)**: Implements `enter_userspace` via `iretq` with User Code segment `0x2B` (DPL 3), User Data segment `0x33` (DPL 3), `RFLAGS = 0x202` (IF=1), and sets `TSS.RSP0` to the current thread's kernel stack on every context switch and before entering userspace.
4. **Build System & Toolchain (`tools/kos.c`, `tools/kos-tool.exe`)**: An autonomous hosted C build tool compiles freestanding kernel sources with LLVM Clang, NASM, and LLD, generates two-pass symbol tables, and stages an EFI System Partition (ESP) with Limine and CPIO initramfs. *(Root-cause discovery: a recent uncompiled edit in `tools/kos.c` temporarily prevented `tools\kos-tool.exe build` from succeeding; rebuilding `kos-tool.exe` achieves 100% clean builds).*
5. **Shell & Test Integration (`kernel/command.c`)**: `command_usertest` already executes six comprehensive tests covering valid ELF execution in Ring 3, six corrupted ELF rejection cases, kernel memory isolation trapping, guard page boundary fault containment, timer preemption in Ring 3, and 50 iterations of process creation/destruction with zero memory leaks.

This report details the architectural mechanisms, exact data structures, build procedures, and step-by-step implementation tasks required to deliver Requirements R2 and R5.

---

## 1. Observation

Direct inspection of the KOS repository revealed the following concrete implementations, line numbers, structures, and behaviors:

### 1.1 Existing ELF64 Loader (`include/kos/elf.h`, `fs/elf.c`)

#### Data Structures (`include/kos/elf.h:7-123`)
The standard ELF64 header and program header structures are defined and verified via static assertions:
```c
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* 16 bytes: \x7fELF, class, data, version, osabi, abiversion, pad */
    Elf64_Half    e_type;            /* 2 = ET_EXEC, 3 = ET_DYN */
    Elf64_Half    e_machine;         /* 62 = EM_X86_64 */
    Elf64_Word    e_version;         /* 1 = EV_CURRENT */
    Elf64_Addr    e_entry;           /* Virtual entry point */
    Elf64_Off     e_phoff;           /* Program header table file offset */
    Elf64_Off     e_shoff;           /* Section header table file offset */
    Elf64_Word    e_flags;           /* Processor-specific flags */
    Elf64_Half    e_ehsize;          /* 64 bytes */
    Elf64_Half    e_phentsize;       /* 56 bytes */
    Elf64_Half    e_phnum;           /* Number of program header entries */
    Elf64_Half    e_shentsize;       /* Section header entry size */
    Elf64_Half    e_shnum;           /* Number of section headers */
    Elf64_Half    e_shstrndx;        /* Section header string table index */
} Elf64_Ehdr;

typedef struct {
    Elf64_Word    p_type;            /* 1 = PT_LOAD, 3 = PT_INTERP, etc. */
    Elf64_Word    p_flags;           /* 1 = PF_X, 2 = PF_W, 4 = PF_R */
    Elf64_Off     p_offset;          /* Segment file offset */
    Elf64_Addr    p_vaddr;           /* Segment virtual address */
    Elf64_Addr    p_paddr;           /* Physical address (ignored) */
    Elf64_Xword   p_filesz;          /* Segment size in file */
    Elf64_Xword   p_memsz;           /* Segment size in memory */
    Elf64_Xword   p_align;           /* Segment alignment (power of 2) */
} Elf64_Phdr;

_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr size must be 64 bytes");
_Static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr size must be 56 bytes");
```

Associated runtime tracking structures (`include/kos/elf.h:111-123`):
```c
struct elf_binary_image {
    uint64_t entry_point;
    uint64_t user_stack_top;
    uint64_t user_stack_bottom;
    uint64_t user_rsp;
};

struct elf_image_info {
    uint64_t entry_point;
    uint16_t program_header_count;
    uint16_t loadable_segment_count;
};
```

#### Validation Performed (`fs/elf.c:202-297`)
In `elf_load_binary(const char *path, uint64_t pml4_phys, struct elf_binary_image *out_image)`:
1. **ELF Header Verification**:
   - `e_ident[0..3]` must equal `0x7f, 'E', 'L', 'F'`.
   - `e_ident[EI_CLASS] == ELFCLASS64` (2).
   - `e_ident[EI_DATA] == ELFDATA2LSB` (1, little-endian).
   - `e_ident[EI_VERSION] == EV_CURRENT` (1).
   - `e_type == ET_EXEC` (2). *(Rejects ET_REL, ET_DYN, ET_CORE)*.
   - `e_machine == EM_X86_64` (62).
   - `e_ehsize == 64` and `e_phentsize == 56`.
   - `0 < e_phnum <= 64`.
   - Program header table offset and byte length must reside within file bounds:
     `range_is_valid(ehdr->e_phoff, (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr), file_size)`.
2. **Entry Point Boundaries**:
   - Entry point must be in canonical user space and >= 2 MiB:
     `ehdr->e_entry >= 0x00200000ull && ehdr->e_entry < 0x0000800000000000ull`.
3. **Program Header Verification**:
   - Rejects dynamic loader interpreter: `if (phdr->p_type == PT_INTERP) return false;`.
   - For all `PT_LOAD` segments:
     - `phdr->p_memsz >= phdr->p_filesz`.
     - `range_is_valid(phdr->p_offset, phdr->p_filesz, file_size)`.
     - `phdr->p_vaddr >= 0x00200000ull`.
     - `phdr->p_vaddr + phdr->p_memsz < 0x0000800000000000ull` (with 64-bit integer overflow check: `end_va >= phdr->p_vaddr`).
     - Alignment check: If `p_align > 1`, `p_align` must be a power of two, and `(phdr->p_vaddr % phdr->p_align) == (phdr->p_offset % phdr->p_align)`.
4. **Segment Non-Overlap Check**:
   - Compares every pair `(i, j)` of loadable segments:
     `max_start = max(start_i, start_j); min_end = min(end_i, end_j); if (max_start < min_end) return false;`.
5. **Entry Point Segment Executability**:
   - Traverses loadable segments to ensure `ehdr->e_entry` falls strictly within a segment marked with `PF_X`:
     `if ((loadable[i]->p_flags & PF_X) != 0 && entry >= p_vaddr && entry < p_vaddr + p_memsz)`.

#### Segment Mapping & Permission Translation (`fs/elf.c:299-349`)
For each valid `PT_LOAD` segment:
1. **Virtual Address Range**:
   - Start VA is aligned down to page boundary: `seg_start_va = phdr->p_vaddr & ~(KOS_PAGE_SIZE - 1)`.
   - End VA is aligned up to page boundary: `seg_end_va = (phdr->p_vaddr + phdr->p_memsz + KOS_PAGE_SIZE - 1) & ~(KOS_PAGE_SIZE - 1)`.
2. **Page Table Flag Translation**:
   ```c
   uint64_t vmm_flags = VMM_PAGE_USER;
   if ((phdr->p_flags & PF_W) != 0) {
       vmm_flags |= VMM_PAGE_WRITABLE;
   }
   if ((phdr->p_flags & PF_X) == 0) {
       vmm_flags |= VMM_PAGE_NO_EXECUTE;
   }
   ```
   *(Enforces W ^ X: data and bss segments are non-executable; code segments are non-writable)*.
3. **Allocation & BSS Zeroing**:
   - For each 4 KiB page in `[seg_start_va, seg_end_va)`:
     - If not already mapped: allocates a physical frame via `pmm_allocate_frame()`.
     - Maps into target PML4 via `vmm_map_page_in(pml4_phys, va, frame, vmm_flags)`.
     - Zeroes the physical frame via `memset(vmm_physical_to_virtual(frame), 0, KOS_PAGE_SIZE)`.
   - Intersection copy:
     - Calculates overlap between current page `[va, va + 4096)` and segment file range `[phdr->p_vaddr, phdr->p_vaddr + phdr->p_filesz)`.
     - Copies `copy_len` bytes from file buffer into `vmm_physical_to_virtual(frame) + dest_off`.
     - Bytes from `p_filesz` to `p_memsz` (the `.bss` segment) are guaranteed zero because the frame was pre-cleared with `memset`.
4. **Error Rollback**:
   - If any frame allocation or page mapping fails, jumps to `load_error`, which calls `vmm_clear_user_address_space(pml4_phys)` and returns `false`, preventing frame leakage.

---

### 1.2 User Process Memory Layout & User Stack (`include/kos/elf.h`, `fs/elf.c`, `mm/vmm.c`)

#### Address Space Layout
The 64-bit virtual memory space is strictly partitioned at the PML4 level:
- **Canonical User Space**: `0x00000000_00000000` .. `0x00007FFF_FFFFFFFF` (128 TiB, PML4 entries 0..255).
- **Reserved Low Memory**: `0x00000000_00000000` .. `0x00000000_001FFFFF` (First 2 MiB) is unmapped. Any dereference of `NULL` or low address triggers a page fault.
- **User Image & Heap**: `0x00000000_00200000` .. `0x00007FFF_FFFF0000`.
- **User Stack**:
  ```c
  #define USER_STACK_TOP        0x00007FFFFFFFE000ull
  #define USER_STACK_SIZE       (64 * 1024ull) /* 64 KiB = 16 pages */
  #define USER_STACK_BOTTOM     (USER_STACK_TOP - USER_STACK_SIZE) /* 0x00007FFFFFFEE000ull */
  #define USER_STACK_GUARD_PAGE (USER_STACK_BOTTOM - 4096ull)      /* 0x00007FFFFFFED000ull */
  ```
- **Guard Page**: `0x00007FFFFFFED000` is explicitly unmapped in `process_setup_user_stack()`:
  ```c
  if (vmm_query_page_in(pml4_phys, USER_STACK_GUARD_PAGE, 0, 0)) {
      vmm_unmap_page_in(pml4_phys, USER_STACK_GUARD_PAGE);
  }
  ```
  Any downward stack growth exceeding 64 KiB faults on the unmapped guard page.

#### System V AMD64 ABI Initial Stack Layout (`fs/elf.c:69-189`)
`process_setup_user_stack()` implements the ABI specification:
1. Allocates and maps 16 user stack pages (`USER_STACK_BOTTOM` to `USER_STACK_TOP`) with `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.
2. Argument string data (`argv[0]`, `argv[1]`, etc.) are written downwards from `USER_STACK_TOP` into the stack via `stack_write_bytes()`.
3. An argument pointer and metadata table is constructed:
   ```
   +-------------------------------------------------------+ 0x00007FFFFFFFE000 (USER_STACK_TOP)
   | Argument strings (e.g. "param1\0", "test_bin\0")      |
   +-------------------------------------------------------+
   | Alignment padding (0-15 bytes to align RSP % 16 == 0) |
   +-------------------------------------------------------+
   | auxv[0].a_val  = 0                                    | [RSP + 8*(5+argc)]
   | auxv[0].a_type = AT_NULL (0)                          | [RSP + 8*(4+argc)]
   | envp[0]        = NULL                                 | [RSP + 8*(3+argc)]
   | argv[argc]     = NULL                                 | [RSP + 8*(2+argc)]
   | argv[argc-1]   = user virtual ptr to argv string      | ...
   | ...                                                   | ...
   | argv[0]        = user virtual ptr to argv string      | [RSP + 8]
   | argc           = integer count (e.g. 2)               | [RSP + 0] <--- initial user %rsp
   +-------------------------------------------------------+
   ```
4. Initial `%rsp` alignment:
   `uint64_t target_rsp = (curr_sp - table_bytes) & ~0xFull;`
   Enforces `target_rsp % 16 == 0`.
   When a standard freestanding C `_start` executes:
   - `pop rdi` extracts `argc` into `%rdi`. `%rsp` is now offset by 8 bytes (`%rsp % 16 == 8`).
   - `mov rsi, rsp` sets `%rsi` to `&argv[0]`.
   - `call main` pushes the 8-byte return address, resulting in `%rsp % 16 == 0` at the entry of `main()`, complying with AMD64 ABI requirements for SSE instructions.

---

### 1.3 Privilege Transition & Hardware Context (`arch/x86_64/cpu.asm`, `kernel/process.c`)

#### Segment Selectors & GDT (`arch/x86_64/descriptors.c:17-25`)
| Index | Selector | Description | Access Byte | Flags | DPL |
|---|---|---|---|---|---|
| 0 | `0x00` | Null Descriptor | `0x00` | `0x0` | - |
| 1 | `0x08` | Kernel Code 64-bit | `0x9A` | `0xA` | 0 |
| 2 | `0x10` | Kernel Data 64-bit | `0x92` | `0xC` | 0 |
| 3-4 | `0x18` | Task State Segment (TSS, 128-bit) | `0x89` | `0x0` | 0 |
| 5 | `0x2B` | User Code 64-bit (`0x28 \| 3`) | `0xFA` | `0xA` | 3 |
| 6 | `0x33` | User Data 64-bit (`0x30 \| 3`) | `0xF2` | `0xC` | 3 |

#### TSS & `RSP0` Management (`arch/x86_64/descriptors.c:80-92`, `kernel/task.c:307-312`)
- The TSS is loaded during boot via `ltr ax` with selector `0x18`.
- In `kernel/task.c:schedule()` and `kernel/process.c:process_exec()`, `tss_set_rsp0()` updates `tss.rsp0` to the top of the active thread's kernel stack:
  ```c
  tss_set_rsp0(((uint64_t)cur_thread->stack + cur_thread->stack_size) & ~0xFull);
  ```
- When a hardware interrupt, exception, or `int 0x80` occurs in Ring 3:
  - The CPU reads `tss.rsp0` from the TSS.
  - Automatically switches stack to `RSP0`.
  - Pushes `SS`, `RSP`, `RFLAGS`, `CS`, `RIP`, and error code (if applicable) onto the kernel stack.
  - No user-space stack corruption or stack pivot can compromise the kernel stack.

#### Ring 3 Transition (`arch/x86_64/cpu.asm:40-81`)
`enter_userspace(entry_point, user_rsp)` performs an atomic `iretq` switch:
```nasm
enter_userspace:
    cli
    mov ax, 0x33         ; Load user data selector into DS, ES, FS, GS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x33      ; SS:  GDT_USER_DS (DPL 3)
    push rsi             ; RSP: user_rsp (aligned to 16 bytes)
    push qword 0x202     ; RFLAGS: IF=1 (interrupts enabled in user mode), bit 1 reserved=1
    push qword 0x2b      ; CS:  GDT_USER_CS (DPL 3)
    push rdi             ; RIP: entry_point

    ; Zero all 15 general-purpose registers to eliminate kernel data leakage
    xor eax, eax
    xor ebx, ebx
    ...
    xor r15, r15

    iretq                ; Atomically enters Ring 3 at CPL 3
```

---

### 1.4 Build System & Toolchain (`tools/kos.c`, `tools/kos-tool.exe`)

#### Compilation & Tool Orchestration
`tools/kos.c` is a self-contained C tool implementing `check`, `build`, and `run` commands:
1. **Tool Discovery**:
   - Clang: Checks `C:/Program Files/LLVM/bin/clang.exe` or `clang` in PATH.
   - NASM: Checks `third_party/tools/nasm-3.02/nasm.exe` or `nasm` in PATH.
   - LLD: Checks `C:/Program Files/LLVM/bin/ld.lld.exe` or `ld.lld` in PATH.
   - NM: Checks `C:/Program Files/LLVM/bin/llvm-nm.exe` or `llvm-nm` in PATH.
   - QEMU: Checks `C:/Program Files/qemu/qemu-system-x86_64.exe`.
2. **Compiler Flags**:
   ```
   --target=x86_64-unknown-none-elf -std=c17 -ffreestanding -fno-stack-protector
   -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-80387
   -ffunction-sections -fdata-sections -fno-common -fno-asynchronous-unwind-tables
   -fno-unwind-tables -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer
   -mno-omit-leaf-frame-pointer -g -I include -I third_party/limine-protocol/include
   -I third_party/font8x8
   ```
3. **Linker Command**:
   ```
   ld.lld -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections
   --build-id=none --fatal-warnings -T boot/linker.ld -o build/kos.elf <objects...>
   ```
4. **Two-Pass Symbol Embedding**:
   - Pass 1 links intermediate `build/kos.elf` with stub symbol table.
   - `llvm-nm -n build/kos.elf` extracts sorted kernel text symbols into `build/kernel_symbols.c`.
   - Pass 2 recompiles symbol table and links final `build/kos.elf`.
5. **Initramfs Generation (`tools/kos.c:227-257`)**:
   - Assembles `build/initramfs.cpio` using CPIO newc format (`070701`).
   - Packs `assets/initramfs/README.TXT` and `build/kos.elf`.
   - Limine loads `boot():/boot/initramfs.cpio` as module `kos-initramfs`.
   - KOS boots, mounts initramfs as root volume `C:`, and initializes RAM disk volume `D:`.

#### Build Failure Investigation & Resolution
Running `.\tools\kos-tool.exe build` during initial inspection resulted in:
```
ld.lld: error: undefined symbol: net_device_get_default
ld.lld: error: undefined symbol: net_device_register
```
**Investigation**:
- `git diff tools/kos.c` revealed that `net/net_device.c` and `net/socket_subsystem.c` were added to `c_sources[]` in `tools/kos.c`.
- However, the binary `tools/kos-tool.exe` on disk had not been recompiled from `tools/kos.c`.
- **Verification**: Compiling `tools/kos.c` to an updated binary and running `build` resulted in `exit code 0`:
  `Kernel ELF built with 818 embedded symbols: build/kos.elf`
  `UEFI ESP staged: build/esp`.
- Recompiling `tools/kos-tool.exe` restores a 100% clean automated build.

---

### 1.5 Shell & Test Integration (`kernel/command.c`)

#### Shell Dispatcher
- Shell commands are registered in `command_entries[]` (`kernel/command.c:4550-4610`) and dispatched via `kernel_command_execute()` (`kernel/command.c:4612-4650`).
- Commands are accessible interactively through `kernel/terminal.c` or invoked during kernel bootstrap (`kernel/main.c:411-445`).

#### Existing `usertest` Command (`kernel/command.c:2908-2971`)
The command `usertest` implements six automated test scenarios:
1. **Test 1: Valid ELF64 Execution in Ring 3 (`test_valid_elf_execution`)**:
   - Dynamically constructs a valid ELF64 header with `PT_LOAD` segment containing:
     ```nasm
     mov rax, 4   ; KOS_SYSCALL_EXIT
     mov rdi, 42  ; Exit code 42
     int 0x80
     jmp $
     ```
   - Writes binary to `D:/valid.elf` in RAMFS.
   - Creates child process `valid_proc`, spawns execution thread via `usertest_child_launcher`, calls `process_exec("D:/valid.elf", 0, NULL)`.
   - Transitions to Ring 3 via `enter_userspace`.
   - Syscall `int 0x80` delivers exit code 42 to `process_exit()`.
   - Parent process synchronizes via `process_wait(child, &exit_code, 3000)`.
   - Asserts exit code == 42, child transitions to `PROCESS_STATE_DEAD`, and 0 frame/heap leaks.
2. **Test 2: Corrupted ELF Rejection (`test_corrupted_elf_rejection`)**:
   - Subcase 2a: Invalid magic bytes (`\x7fXYZ`).
   - Subcase 2b: Non-x86_64 target machine (`e_machine = EM_386`).
   - Subcase 2c: Entry point outside loadable segment (`0x00100000`).
   - Subcase 2d: `p_memsz < p_filesz` boundary violation.
   - Subcase 2e: Truncated ELF file (< 64 bytes).
   - Subcase 2f: `process_exec` rollback with corrupted image cleanly returns -1 without leaking resources.
3. **Test 3: Kernel Memory Isolation Enforcement (`test_kernel_mem_isolation`)**:
   - Subcase 3a: Ring 3 reads higher-half kernel text `0xFFFFFFFF80000000`.
   - Subcase 3b: Ring 3 reads higher-half direct physical map `0xFFFF800000000000`.
   - Both trigger hardware `#PF` (`U=1, P=1`).
   - Kernel detects `is_user == true` (`arch/x86_64/interrupts.c:256`), logs warning, terminates process with `SIGSEGV (-11)`, wakes parent, and kernel continues normal operation without panic.
4. **Test 4: User Stack Guard Page Containment (`test_stack_guard_containment`)**:
   - Subcase 4a: Ring 3 writes to top of guard page: `0x00007FFFFFFEDFF8`.
   - Subcase 4b: Ring 3 writes to base of guard page: `0x00007FFFFFFED000`.
   - Both hit unmapped PTE (`P=0`). Trapped as user mode `#PF` (`SIGSEGV -11`). Kernel remains stable.
5. **Test 5: TSS/RSP0 Ring 3 Hardware Preemption (`test_preemption_hardware`)**:
   - Ring 3 executes an intensive compute loop (`30,000,000` iterations).
   - Periodic 100 Hz PIT timer IRQs fire while CPU is in Ring 3.
   - CPU switches to `TSS.RSP0`, runs `timer_irq_handler` and `task_timer_tick`, decrements quantum, and returns smoothly to Ring 3 via `iretq`.
   - Loop completes and exits cleanly with exit code 55.
6. **Test 6: 50-Iteration Zero Memory Leak Verification (`test_leak_50_iterations`)**:
   - Runs a 50-iteration loop creating processes, running ELF in Ring 3, waiting, and destroying.
   - Compares final free frame count and active heap allocations against baseline:
     `final_frames == baseline_frames` and `final_heap == baseline_heap` (0 memory leaks).

---

## 2. Logic Chain

```
                +-------------------------------------------------------------+
                | Observation: fs/elf.c & include/kos/elf.h fully implement   |
                | Elf64_Ehdr, Elf64_Phdr, PT_LOAD mapping, and stack layout.  |
                +-------------------------------------------------------------+
                                               |
                                               v
                +-------------------------------------------------------------+
                | Logic: ELF loading and address space setup are already      |
                | robust and compliant with AMD64 specifications.             |
                +-------------------------------------------------------------+
                                               |
         +-------------------------------------+-------------------------------------+
         |                                                                           |
         v                                                                           v
+---------------------------------------------+             +---------------------------------------------+
| Observation: arch/x86_64/cpu.asm zero-fills |             | Observation: Syscall numbers in syscall.h   |
| all GPRs in enter_userspace(rip, rsp).      |             | (GETTIME=1, SYSINFO=2, WRITE=3, EXIT=4)     |
| Stack contains argc, argv[], envp[], auxv.  |             | differ from Milestone prompt specification  |
+---------------------------------------------+             | (EXIT=1, WRITE=2, READ=3, GETTIME=4, etc.)  |
         |                                                  +---------------------------------------------+
         v                                                                           |
+---------------------------------------------+                                      v
| Logic: System V AMD64 ABI specifies argc    |             +---------------------------------------------+
| at (%rsp) and argv at 8(%rsp). C runtime    |             | Logic: Syscall numbers must be standardized |
| pop rdi; mov rsi, rsp correctly initializes |             | or aliased to satisfy prompt without        |
| registers and aligns stack to 16 bytes.     |             | breaking existing usertest hardcoded bytes. |
+---------------------------------------------+             +---------------------------------------------+
         |                                                                           |
         +-------------------------------------+-------------------------------------+
                                               |
                                               v
                +-------------------------------------------------------------+
                | Observation: kernel/command.c lacks an interactive "exec"   |
                | command, but usertest contains usertest_child_launcher.     |
                | No freestanding user-mode C program exists in tree yet.     |
                +-------------------------------------------------------------+
                                               |
                                               v
                +-------------------------------------------------------------+
                | Conclusion & Action Plan:                                   |
                | 1. Recompile tools/kos-tool.exe to fix build break.         |
                | 2. Add copy_from_user / copy_to_user memory safe functions. |
                | 3. Add user/crt0.asm, user/user.ld, user/hello.c.           |
                | 4. Add "exec <path> [args]" to kernel/command.c.            |
                | 5. Standardize/alias syscall dispatch table.                |
                +-------------------------------------------------------------+
```

1. **Step 1 (ELF Validation & Segment Loading)**:
   - `fs/elf.c` validates magic (`0x7f, 'E', 'L', 'F'`), architecture (`ELFCLASS64`), endianness (`ELFDATA2LSB`), machine (`EM_X86_64`), entry point bounds (`0x00200000 <= entry < 0x0000800000000000`), alignment, and non-overlapping `PT_LOAD` segments.
   - When mapping, page table flags are strictly assigned: `VMM_PAGE_USER`, `VMM_PAGE_WRITABLE` (if `PF_W`), and `VMM_PAGE_NO_EXECUTE` (if not `PF_X`).
   - Frame allocation zero-fills every physical page; filesz bytes are copied from disk; memsz bytes beyond filesz are zeroed by definition.
   - *Inference*: The ELF loader satisfies Requirement R2 completely.

2. **Step 2 (Stack ABI & Alignment)**:
   - The user stack is mapped from `0x00007FFFFFFEE000` to `0x00007FFFFFFFE000` (64 KiB), with an unmapped 4 KiB guard page at `0x00007FFFFFFED000`.
   - String values are written downward from the top of the stack.
   - The table at `target_rsp` contains `argc`, `argv[0..argc-1]`, `NULL`, `envp[0] = NULL`, and `AT_NULL`.
   - `target_rsp` is masked with `~0xFull`, guaranteeing 16-byte alignment.
   - *Inference*: Passing `user_rsp` to `enter_userspace` sets `%rsp` to the table base. A standard `_start` using `pop rdi; mov rsi, rsp; call main` sets `argc` in `%rdi`, `argv` in `%rsi`, and aligns `%rsp` to 16 bytes before entering `main()`.

3. **Step 3 (Ring 3 Hardware Context & Trap Safety)**:
   - GDT entries `0x2B` (User Code, DPL 3) and `0x33` (User Data, DPL 3) are loaded.
   - `enter_userspace` pushes `SS (0x33)`, `RSP (user_rsp)`, `RFLAGS (0x202)`, `CS (0x2B)`, `RIP (entry_point)` and executes `iretq`.
   - `tss_set_rsp0()` updates `TSS.RSP0` on every thread switch and before `enter_userspace`.
   - Interrupts or faults in Ring 3 switch automatically to `TSS.RSP0`.
   - In `arch/x86_64/interrupts.c`, `is_user` detects `context->cs & 3 == 3`. Page faults call `process_exit(cur, -11)` and general exceptions call `process_exit(cur, -1)`. The kernel does not panic.
   - *Inference*: Requirement R3 is operational.

4. **Step 4 (Build System & User Binary Generation)**:
   - `tools/kos.c` orchestrates Clang, NASM, and LLD.
   - A freestanding user binary can be built with standard Clang (`-mcmodel=small -ffreestanding -nostdlib`), NASM (`-f elf64`), and LLD (`-m elf_x86_64 -T user/user.ld`).
   - The compiled ELF can be integrated via CPIO initramfs (`C:\HELLO.ELF`) or written to RAMFS (`D:\HELLO.ELF`).
   - *Inference*: Requirement R5 can be implemented by creating a minimal `user/` directory with `crt0.asm`, `hello.c`, and a linker script.

5. **Step 5 (Syscall Table Numbering Conflict)**:
   - `include/kos/syscall.h` defines `GETTIME=1, SYSINFO=2, WRITE=3, EXIT=4`.
   - The Milestone Prompt requests `SYS_EXIT=1, SYS_WRITE=2, SYS_READ=3, SYS_GETTIME=4, SYS_SYSINFO=5, SYS_YIELD=6, SYS_SLEEP=7`.
   - In `kernel/command.c`, existing tests in `command_usertest` hardcode opcode `mov rax, 4` for exit.
   - *Inference*: To avoid breaking existing tests while satisfying the prompt, the syscall dispatcher should support both the prompt's standardized numbering (`SYS_EXIT = 1`) and the legacy numbers (`4 = EXIT`), or update the test opcodes accordingly.

---

## 3. Caveats

1. **`copy_from_user` / `copy_to_user` Missing**:
   - `copy_from_user` and `copy_to_user` are not currently implemented.
   - When user-space passes a pointer to `sys_write` (e.g. `write(fd, buf, len)`), the kernel must validate that `buf` and `buf + len` lie strictly within `[0x00200000, 0x0000800000000000)` and verify page presence to prevent a kernel-mode page fault panic.
2. **`tools/kos-tool.exe` Out of Sync**:
   - The binary executable `tools/kos-tool.exe` on disk must be recompiled from `tools/kos.c` using hosted Clang to include recently added network modules.
3. **No Standalone User Binary in Tree**:
   - Existing tests generate test ELFs dynamically into memory buffers (`create_test_elf`) and write them to RAMFS.
   - There is no separate `user/` directory with a standalone C project (`hello.c` + `crt0.asm`).
4. **Shell `exec` Command Absent**:
   - Currently only `usertest` and `elfinfo` exist in `kernel/command.c`. An interactive `exec <path> [args]` command needs to be added to the shell registry.

---

## 4. Conclusion & Proposed Implementation Plan

### 4.1 Assessment
The core components for Milestone K13 & K14 (ELF loader, user stack formatting, address space creation/destruction, Ring 3 switching via `iretq`, TSS/RSP0 stack switching, and unhandled exception termination) are already fully implemented and verified by self-tests in the KOS codebase.

### 4.2 Proposed Step-by-Step Implementation

#### Step 1: Recompile Host Tool (`tools\kos-tool.exe`)
Recompile `tools/kos.c` with the host Clang compiler:
```powershell
& "C:\Program Files\LLVM\bin\clang.exe" tools/kos.c -o tools/kos-tool.exe
```
Verify clean build with:
```powershell
.\tools\kos-tool.exe build
```

#### Step 2: Implement Safe User Memory Copy Helpers (`include/kos/memory.h`, `lib/memory.c`)
Add bounds checking and safe copying:
```c
bool copy_from_user(void *dst_kernel, const void *src_user, uint64_t len);
bool copy_to_user(void *dst_user, const void *src_kernel, uint64_t len);
char *strndup_from_user(const char *src_user, uint64_t max_len);
```
Rule: Reject any address `< 0x00200000` or `>= 0x0000800000000000`, or where `src + len < src`.

#### Step 3: Standardize Syscall Table (`include/kos/syscall.h`, `kernel/syscall.c`)
Update `enum kos_syscall_number` to match Milestone prompt:
```c
enum kos_syscall_number {
    SYS_EXIT    = 1,
    SYS_WRITE   = 2,
    SYS_READ    = 3,
    SYS_GETTIME = 4,
    SYS_SYSINFO = 5,
    SYS_YIELD   = 6,
    SYS_SLEEP   = 7,
};
```
In `kos_syscall_dispatch()`, implement:
- `SYS_EXIT`: calls `process_exit(proc, (int)arg0)`.
- `SYS_WRITE`: validates user buffer with `copy_from_user()`, writes to `console_write()` and `serial_write()`.
- `SYS_GETTIME`: returns `timer_uptime_seconds()` or ticks.
- `SYS_SYSINFO`: populates user structure or returns task/frame count.
- `SYS_YIELD`: calls `task_yield()`.
- `SYS_SLEEP`: calls `task_sleep_ms(arg0)`.
*(Maintain legacy aliases for 4 -> EXIT, 1 -> GETTIME during transition).*

#### Step 4: Create Standalone User Program & C Runtime (`user/`)
1. **User Linker Script (`user/user.ld`)**:
   ```ld
   OUTPUT_FORMAT(elf64-x86-64)
   OUTPUT_ARCH(i386:x86-64)
   ENTRY(_start)
   SECTIONS {
       . = 0x00400000;
       .text : ALIGN(4096) { *(.text._start) *(.text*) }
       .rodata : ALIGN(4096) { *(.rodata*) }
       .data : ALIGN(4096) { *(.data*) }
       .bss (NOLOAD) : ALIGN(4096) { *(COMMON) *(.bss*) }
       /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
   }
   ```
2. **C Runtime Entry Point (`user/crt0.asm`)**:
   ```nasm
   bits 64
   default rel
   global _start
   extern main

   section .text._start
   _start:
       xor rbp, rbp       ; Clear RBP for stack traces
       pop rdi            ; RDI = argc (pop advances RSP by 8, aligning RSP % 16 == 8)
       mov rsi, rsp       ; RSI = &argv[0]
       call main          ; Call pushes 8 bytes, aligning RSP % 16 == 0 in main
       mov edi, eax       ; RDI = exit code from main()
       mov eax, 1         ; SYS_EXIT (1)
       int 0x80
   .halt:
       pause
       jmp .halt
   ```
3. **Minimal Syscall Library & Program (`user/hello.c`)**:
   ```c
   static inline uint64_t syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
       uint64_t ret;
       __asm__ volatile (
           "int $0x80"
           : "=a"(ret)
           : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
           : "rcx", "r11", "memory"
       );
       return ret;
   }

   int main(int argc, char **argv) {
       const char msg[] = "Hello from Ring 3 User-Space in KOS!\n";
       syscall3(2, 1, (uint64_t)msg, sizeof(msg) - 1); /* SYS_WRITE */
       return 0;
   }
   ```
4. **Toolchain Integration**:
   Add user build rules to `tools/kos.c` to compile `user/hello.c` to `build/hello.elf` and include it in `build/initramfs.cpio` (accessible as `C:\HELLO.ELF`).

#### Step 5: Add `exec` Command to Kernel Shell (`kernel/command.c`)
Add `command_exec` to `command_entries[]`:
```c
static void command_exec(const char *arguments) {
    if (arguments == NULL || *arguments == '\0') {
        log_error("Usage: exec <path> [args...]");
        return;
    }
    // Parse path and argv
    // Check vfs_read_file(path, ...)
    struct process *child = process_create("user_app");
    if (!child) { log_error("Failed to create process"); return; }
    struct thread *t = thread_create(child, usertest_child_launcher, (void *)path, "app_t");
    if (!t) { process_destroy(child); return; }
    int exit_code = 0;
    if (process_wait(child, &exit_code, 5000)) {
        log_infof("Process exited with code %d", exit_code);
    } else {
        log_error("Process execution timed out");
    }
}
```

---

## 5. Verification Method

To independently verify the findings and implementation:

### 5.1 Static Inspection Verification
1. **ELF Loader Verification**:
   Inspect `fs/elf.c:202-368` to verify all magic, boundary, and segment checks.
2. **User Stack Verification**:
   Inspect `fs/elf.c:69-189` to verify `process_setup_user_stack()` argument string packing, table generation, and 16-byte alignment.
3. **Privilege Transition Verification**:
   Inspect `arch/x86_64/cpu.asm:40-81` to verify selector `0x2B` (CS) and `0x33` (SS, DS, ES, FS, GS) and `iretq` stack frame.

### 5.2 Build Verification
Execute host tool compilation and kernel build:
```powershell
& "C:\Program Files\LLVM\bin\clang.exe" tools/kos.c -o tools/kos-tool.exe
.\tools\kos-tool.exe build
```
**Expected Result**:
- Compiler exits with code 0 (0 warnings, 0 errors).
- `Kernel ELF built with 818 embedded symbols: build/kos.elf`.
- `UEFI ESP staged: build/esp`.

### 5.3 Automated Self-Test Verification in QEMU
Run KOS in QEMU and execute tests:
```powershell
.\tools\kos-tool.exe run
```
At the interactive shell terminal:
1. `usertest all`: Runs all 6 Ring 3 test suites.
   **Expected Result**:
   - `[PASS] Test 1: Valid ELF64 binary execution (exit_code=42, 0 leaks)`
   - `[PASS] Test 2: Corrupted/invalid ELF binary rejection (6 subcases, 0 leaks)`
   - `[PASS] Test 3: Kernel memory isolation enforcement (2 higher-half addresses, SIGSEGV -11 trapped, 0 leaks)`
   - `[PASS] Test 4: User stack guard page containment (2 guard boundaries trapped, 0 leaks)`
   - `[PASS] Test 5: TSS/RSP0 Ring 3 hardware preemption`
   - `[PASS] Test 6: 50-iteration zero memory leak test (0 leaks)`
   - `>>> [PASS] ALL K14 USERSPACE & PROCESS TESTS PASSED! <<<`
2. `elfinfo C:\KOS.ELF`:
   **Expected Result**:
   Displays entry point, program header count, and loadable segment count without crashing.
3. `exec C:\HELLO.ELF` (once Step 4 is implemented):
   **Expected Result**:
   Prints `Hello from Ring 3 User-Space in KOS!` and reports exit code 0.

### 5.4 Invalidation Conditions
- If an invalid ELF binary (corrupted magic, `p_memsz < p_filesz`, truncated) causes a kernel panic rather than returning an error code.
- If Ring 3 execution of privileged instructions or accessing higher-half addresses crashes the host kernel rather than cleanly terminating the faulting process.
- If repeated executions of user processes cause `pmm_free_frame_count()` or `heap_active_allocation_count()` to diverge from baseline.
