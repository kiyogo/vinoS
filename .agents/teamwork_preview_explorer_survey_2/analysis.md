# Milestone K14 Architecture Survey: VMM, ELF64 Loader, Address Space & User Stack

**Author**: Explorer Survey Agent 2  
**Date**: 2026-09-18  
**Scope**: Milestone K14 (Process and Ring 3 Userspace) — Requirement R1 (ELF64 Binary Loader & Address Space Mapping)

---

## 1. Executive Summary

Milestone K14 introduces Ring 3 user execution and isolated process address spaces to KOS. This technical analysis audits the existing Virtual Memory Manager (`mm/vmm.c`, `include/kos/vmm.h`), process definitions (`kernel/process.c`, `include/kos/process.h`), ELF parser (`fs/elf.c`, `include/kos/elf.h`), interrupt/fault subsystems (`arch/x86_64/interrupts.c`), and build system (`tools/kos.c`).

### Key Discoveries:
1. **Single Global PML4**: `mm/vmm.c` operates exclusively on a single static `vmm.pml4` initialized from Limine's boot CR3. No API currently exists to allocate, clone, switch, or free per-process PML4 address spaces.
2. **Missing User Page Flags**: `VMM_PAGE_USER` (PTE bit 2) is **not defined** in `include/kos/vmm.h`, and `vmm_map_page()` strictly rejects any bit other than `VMM_PAGE_WRITABLE` (bit 1) and `VMM_PAGE_NO_EXECUTE` (bit 63). Intermediate tables (PML4E, PDPTE, PDE) are created without `PAGE_USER`, which causes hardware-level protection faults if Ring 3 attempts to access them.
3. **No CR3 Writing**: `include/kos/cpu.h` and `arch/x86_64/cpu_control.c` expose `cpu_read_cr3()`, but no `cpu_write_cr3()`. Process creation simply assigns `proc->pml4_physical = cpu_read_cr3() & ~0xFFF` (sharing kernel page tables), and `schedule()` in `kernel/task.c` never switches CR3.
4. **Inspect-Only ELF**: `fs/elf.c` implements `elf_inspect_image()`, which only inspects header fields using manual byte deserializers. No ELF loader exists to map `PT_LOAD` segments, allocate physical memory, zero BSS sections, or set up execution state.
5. **No User Stack / Guard Pages**: All existing tasks execute in kernel space on `TASK_KERNEL_STACK_SIZE` (16 KiB) heap allocations (`kmalloc`). There is no user stack allocation, no non-present guard page, and no System V AMD64 ABI stack layout.
6. **Fatal User Faults in Handler**: `page_fault_handler()` in `arch/x86_64/interrupts.c` halts the CPU (`cpu_halt()`) upon any user-mode fault (`PF_ERROR_USER`). This must be updated to terminate the offending user process cleanly.
7. **Storage Ready**: Initramfs (CPIO newc) mounted at `C:` and RAMFS mounted at `D:` provide functional file storage accessed via `vfs_read_file()`. `tools/kos.c` can package user ELF binaries into `build/initramfs.cpio`.

---

## 2. Detailed Technical Findings

### 2.1 VMM & Address Space Management

#### 2.1.1 Existing Paging Implementation (`mm/vmm.c`)
- The VMM manages a four-level x86_64 paging structure (PML4 -> PDPT -> PD -> PT).
- State is encapsulated in:
  ```c
  struct virtual_memory_manager {
      uint64_t hhdm_offset;
      uint64_t *pml4;
      bool initialized;
  };
  static struct virtual_memory_manager vmm;
  ```
- `vmm_initialize(uint64_t hhdm_offset)` (lines 102–113) fetches the boot CR3 via `cpu_read_cr3()` and sets `vmm.pml4 = physical_to_virtual(cr3)`.
- All mapping functions (`vmm_map_page`, `vmm_unmap_page`, `vmm_set_page_permissions`, `vmm_query_page`) mutate `vmm.pml4` directly under `vmm_lock`.
- **Limitation**: There is no concept of a separate `aspace` or target PML4 parameter.

#### 2.1.2 Virtual Address Space Partitioning & Higher-Half Sharing
In x86_64 architecture with 4-level paging (48-bit canonical addressing):
- **User Virtual Space (Lower Half)**: `0x0000000000000000` to `0x00007fffffffffff` (PML4 indices `0` to `255`, totaling 128 TiB).
- **Non-Canonical Hole**: `0x0000800000000000` to `0xffff7fffffffffff`.
- **Kernel Virtual Space (Higher Half)**: `0xffff800000000000` to `0xffffffffffffffff` (PML4 indices `256` to `511`, totaling 128 TiB).
  - Limine Higher-Half Direct Map (HHDM): maps all physical RAM at `kos_hhdm_request.response->offset` (typically index >= 256).
  - Kernel Text, Data, BSS: linked at `0xffffffff80000000` (`boot/linker.ld:16`, PML4 index `511`).
  - Kernel Heap: `HEAP_BASE = 0xffffffffc0000000` (`mm/heap.c:12`, PML4 index `511`).

#### 2.1.3 Address Space Creation & Higher-Half Cloning Mechanism
To create an isolated address space for a user process:
1. **PML4 Frame Allocation**: Allocate a single physical page frame using `pmm_allocate_frame()`.
2. **Access via HHDM**: Get its kernel virtual address: `uint64_t *new_pml4 = physical_to_virtual(pml4_frame)`.
3. **Zero Lower Half (User Entries 0–255)**:
   ```c
   memset(&new_pml4[0], 0, 256 * sizeof(uint64_t));
   ```
4. **Clone Upper Half (Kernel Entries 256–511)**:
   ```c
   memcpy(&new_pml4[256], &vmm.pml4[256], 256 * sizeof(uint64_t));
   ```
   *Rationale*: Copying PML4 entries 256–511 shares the top-level pointers to the kernel's existing PDPTs. Any kernel page table updates (or shared kernel structures) remain synchronized across all processes without duplicating underlying kernel page tables. Furthermore, because these entries do not have the `PTE_USER` bit set, user mode (Ring 3) is strictly prevented by the CPU MMU from accessing any higher-half address.

#### 2.1.4 Address Space Destruction & Resource Reclamation
Upon process termination (`process_destroy()`):
- The lower half (PML4 entries 0..255) must be recursively walked to free:
  - All allocated leaf frames (program code, data, BSS, user stack).
  - All allocated intermediate page tables (PT, PD, PDPT) belonging to the user half.
- The upper half (PML4 entries 256..511) **must NOT** be traversed or freed, as those tables are shared kernel structures.
- Finally, the PML4 frame itself is freed via `pmm_free_frame(pml4_physical)`.

---

### 2.2 User Page Permissions & Paging Hierarchy Bits

#### 2.2.1 Status of Page Table Bits in KOS
Reviewing `include/kos/vmm.h:7-10`:
```c
enum {
    VMM_PAGE_WRITABLE = 1ull << 1,
    VMM_PAGE_NO_EXECUTE = 1ull << 63,
};
```
- Bit 0 (`PAGE_PRESENT = 1ull << 0`): Internal to `mm/vmm.c:12`.
- Bit 1 (`PAGE_WRITABLE = 1ull << 1`): Supported (`VMM_PAGE_WRITABLE`).
- Bit 2 (`PAGE_USER = 1ull << 2`): **Missing completely**.
- Bit 63 (`PAGE_NO_EXECUTE = 1ull << 63`): Supported (`VMM_PAGE_NO_EXECUTE`).

In `mm/vmm.c:117-118`:
```c
if (virtual_address % PAGE_SIZE != 0 || physical_address % PAGE_SIZE != 0
    || (flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE)) != 0) {
    return false;
}
```
Any call attempting to pass `1ull << 2` currently returns `false`.

#### 2.2.2 Hierarchy Propagation Rule (Crucial Architecture Constraint)
In AMD64 paging:
$$\text{Effective Privilege} = \text{PML4E.U} \land \text{PDPTE.U} \land \text{PDE.U} \land \text{PTE.U}$$
If **any** entry in the paging hierarchy has the User/Supervisor bit $U/S = 0$ (Supervisor), the leaf page is treated as Supervisor-only.
In `mm/vmm.c:97` (`next_table`):
```c
*entry = frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE;
```
Intermediate tables are created with $U/S = 0$.
**Required Change**:
When mapping user pages (or allocating tables for addresses $< 0x0000800000000000$), all intermediate table entries (PML4E, PDPTE, PDE) **must** have:
```c
*entry = frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | VMM_PAGE_USER | PAGE_KOS_OWNED_TABLE;
```
This ensures leaf PTE permissions dictate final accessibility (`PF_R`, `PF_W`, `PF_X`).

---

### 2.3 ELF64 Parser & Binary Loader Subsystem

#### 2.3.1 Existing Code Analysis (`fs/elf.c`)
The current `fs/elf.c` contains 83 lines:
- Helper routines `read_u16()`, `read_u32()`, `read_u64()` manually parse raw byte offsets.
- `elf_inspect_image()` checks:
  - Magic: `0x7f 'E' 'L' 'F'`
  - Class: `ELF_CLASS_64 (2)`
  - Endian: `ELF_LITTLE_ENDIAN (1)`
  - Version: 1
  - Type: `ELF_TYPE_EXECUTABLE (2)` or `ELF_TYPE_SHARED (3)`
  - Machine: `ELF_MACHINE_X86_64 (0x3e)`
  - Program header offset and bounds
- **Deficiencies for K14**:
  - No loading capability.
  - Does not allocate page frames or map virtual addresses.
  - Does not zero BSS (`p_memsz > p_filesz`).
  - Does not translate `p_flags` (`PF_X`, `PF_W`, `PF_R`) to VMM flags.
  - Does not validate that `p_vaddr` is within canonical user address space.
  - No canonical C data structures for ELF64.

#### 2.3.2 Standard ELF64 Structures Required (`include/kos/elf.h`)
The loader requires standard 64-bit ELF data structures:
```c
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word    p_type;
    Elf64_Word    p_flags;
    Elf64_Off     p_offset;
    Elf64_Addr    p_vaddr;
    Elf64_Addr    p_paddr;
    Elf64_Xword   p_filesz;
    Elf64_Xword   p_memsz;
    Elf64_Xword   p_align;
} Elf64_Phdr;
```

#### 2.3.3 Strict Validation Pipeline
To ensure corrupted or malicious binaries fail gracefully without panicking the kernel:

| Check Category | Verification Criteria | Action on Failure |
|---|---|---|
| **ELF Header** | `image_size >= sizeof(Elf64_Ehdr)` | Return `false` / `KOS_ERR_INVALID` |
| | `memcmp(e_ident, "\x7fELF", 4) == 0` | Return `false` |
| | `e_ident[EI_CLASS] == 2` (64-bit) | Return `false` |
| | `e_ident[EI_DATA] == 1` (2's complement little-endian) | Return `false` |
| | `e_type == ET_EXEC` (2) | Return `false` (dynamic shared objects not supported in R1) |
| | `e_machine == EM_X86_64` (0x3E = 62) | Return `false` |
| | `e_version == 1` | Return `false` |
| | `e_ehsize == sizeof(Elf64_Ehdr)` (64) | Return `false` |
| | `e_phentsize == sizeof(Elf64_Phdr)` (56) | Return `false` |
| | `e_phnum > 0` and `e_phnum <= 64` (sanity bound) | Return `false` |
| | `e_phoff + (uint64_t)e_phnum * e_phentsize <= image_size` | Return `false` |
| **Program Headers** | Reject unsupported segment types (e.g. `PT_INTERP` dynamic linker) | Return `false` |
| | `p_offset + p_filesz <= image_size` | Return `false` |
| | `p_memsz >= p_filesz` | Return `false` |
| | If `p_align > 0`: power of 2, and `(p_vaddr % p_align) == (p_offset % p_align)` | Return `false` |
| **Virtual Bounds** | `p_vaddr >= 0x00200000` (2 MiB, protecting NULL page & low identity) | Return `false` |
| | `p_vaddr + p_memsz <= 0x00007fffffffffff` (must be within user half) | Return `false` |
| | Wraparound check: `p_vaddr + p_memsz >= p_vaddr` | Return `false` |
| | No segment overlap among multiple `PT_LOAD` segments | Return `false` |
| **Entry Point** | `e_entry >= 0x00200000` and `e_entry < 0x00007fffffffffff` | Return `false` |
| | `e_entry` must fall within a `PT_LOAD` segment marked with `PF_X` | Return `false` |

#### 2.3.4 Segment Loading Algorithm
For each valid `PT_LOAD` segment:
1. Translate `p_flags` to VMM flags:
   - Base flags: `VMM_PAGE_USER`
   - If `p_flags & PF_W`: `flags |= VMM_PAGE_WRITABLE`
   - If `!(p_flags & PF_X)`: `flags |= VMM_PAGE_NO_EXECUTE`
2. Page-align boundaries:
   $$\text{start\_vaddr} = \text{align\_down}(p\_vaddr, \text{PAGE\_SIZE})$$
   $$\text{end\_vaddr} = \text{align\_up}(p\_vaddr + p\_memsz, \text{PAGE\_SIZE})$$
3. For each virtual page from `start_vaddr` to `end_vaddr`:
   - Allocate a physical frame: `paddr = pmm_allocate_frame()`.
   - Map into address space: `vmm_map_page_in(pml4_phys, vaddr, paddr, flags)`.
   - Access frame via HHDM: `page_ptr = physical_to_virtual(paddr)`.
   - Zero entire 4096 bytes: `memset(page_ptr, 0, PAGE_SIZE)`.
   - Copy file contents intersecting this page:
     Calculate overlap between $[p\_vaddr, p\_vaddr + p\_filesz)$ and $[\text{vaddr}, \text{vaddr} + 4096)$.
     If overlap exists: `memcpy(page_ptr + page_offset, image + p_offset + file_offset, copy_bytes)`.
   - The remaining bytes in the page automatically remain zeroed (handling BSS).
4. If any allocation or mapping fails: unwind all previously mapped segments and frames, freeing memory and returning `false`.

---

### 2.4 User Stack & Guard Pages

#### 2.4.1 Stack Location & Size Parameters
- **Top of User Stack**:
  ```c
  #define USER_STACK_TOP        0x00007fffffffe000ull
  #define USER_STACK_SIZE       (64 * 1024ull)  /* 64 KiB = 16 pages */
  #define USER_STACK_PAGE_COUNT (USER_STACK_SIZE / KOS_PAGE_SIZE)
  ```
  `USER_STACK_TOP` sits just below the top of the 48-bit canonical user space (`0x00007fffffffffff`), page-aligned.
- **Stack Usable Range**:
  `[stack_bottom, USER_STACK_TOP)` where:
  $$\text{stack\_bottom} = \text{USER\_STACK\_TOP} - \text{USER\_STACK\_SIZE} = 0x00007ffffffeed000ull$$

#### 2.4.2 Non-Present Guard Page Architecture
- **Location**:
  $$\text{guard\_vaddr} = \text{stack\_bottom} - \text{PAGE\_SIZE} = 0x00007ffffffecc000ull$$
- **Allocation Mechanism**:
  The guard page is **explicitly NOT mapped** in the process page tables (or mapped with `PAGE_PRESENT = 0`).
- **Hardware Fault Trigger**:
  When a user thread overflows its stack and decrements `RSP` into the guard page, any memory access generates a Page Fault (#PF, vector 14).
  - Register `CR2` = `guard_vaddr` $\le \text{fault\_addr} < \text{stack\_bottom}$.
  - Error code: `P = 0` (non-present), `U/S = 1` (user mode), `W/R = 1` (write access).
- **Fault Recovery / Process Termination**:
  Currently, `page_fault_handler()` (`arch/x86_64/interrupts.c:268`) issues a panic halt.
  In K14, `page_fault_handler()` must inspect `context->error_code`:
  ```c
  if (context->error_code & PF_ERROR_USER) {
      struct process *proc = process_get_current();
      if (cr2 >= proc->user_stack_guard && cr2 < proc->user_stack_bottom) {
          log_error("Process stack overflow: guard page fault (PID %llu)", proc->pid);
      } else {
          log_error("Process segmentation violation at 0x%016lx (PID %llu)", cr2, proc->pid);
      }
      process_exit(proc, -1);
      schedule(); /* Switch to next runnable thread */
  }
  ```
  The process is terminated and reaped without crashing the kernel.

---

### 2.5 Initial User Stack Layout (System V AMD64 ABI)

#### 2.5.1 AMD64 ABI Initial Stack State
According to the System V AMD64 ABI specification (Section 3.4.1):
1. Upon entry to `_start`:
   - `%rsp` must point directly to `argc` on the stack.
   - `%rsp` must be 16-byte aligned: `(rsp & 0xf) == 0`.
   - `%rdx` holds a function pointer for finalizer/atexit registration (set to `0` if none).
   - `%rax`, `%rbx`, `%rcx`, `%rsi`, `%rdi`, `%rbp`, `%r8`–`%r15` are undefined or `0`.
2. Stack Layout (from `USER_STACK_TOP` down to initial `%rsp`):

```
+-------------------------------------------------------------+ USER_STACK_TOP (0x7fffffffe000)
| String storage: envp strings ("PATH=...", "TERM=...")       |
| String storage: argv strings ("arg0", "arg1", ...)          |
+-------------------------------------------------------------+
| Padding (0 to 15 bytes) to ensure RSP 16-byte alignment     |
+-------------------------------------------------------------+
| Auxiliary Vector: AT_NULL (key = 0, value = 0)   [16 bytes] |
+-------------------------------------------------------------+
| NULL pointer (terminating envp array)            [8 bytes]  |
| envp[envc-1] pointer                             [8 bytes]  |
| ...                                                         |
| envp[0] pointer                                  [8 bytes]  |
+-------------------------------------------------------------+
| NULL pointer (terminating argv array)            [8 bytes]  |
| argv[argc-1] pointer                             [8 bytes]  |
| ...                                                         |
| argv[0] pointer                                  [8 bytes]  |
+-------------------------------------------------------------+
| argc (uint64_t)                                  [8 bytes]  |
+-------------------------------------------------------------+ <-- Initial RSP (%rsp % 16 == 0)
```

#### 2.5.2 Layout Computation & Push Logic
Let $A$ be `argc`, $E$ be the number of `envp` pointers.
The pointer & metadata section contains:
- $1$ quadword for `argc`
- $A + 1$ quadwords for `argv[]` (including terminating `NULL`)
- $E + 1$ quadwords for `envp[]` (including terminating `NULL`)
- $2$ quadwords for `auxv` (`AT_NULL` key + value)
Total quadwords = $1 + (A + 1) + (E + 1) + 2 = A + E + 5$.
Total bytes for table = $(A + E + 5) \times 8$.

To ensure `%rsp` is 16-byte aligned:
$$\text{sp}_{\text{initial}} = (\text{sp}_{\text{after\_strings}} - \text{table\_bytes}) \;\&\; \sim0\text{xFLL}$$
This satisfies the System V ABI guarantee that `(%rsp & 0xf) == 0`.

---

### 2.6 Binary Storage in KOS

#### 2.6.1 Storage Hierarchy
1. **Initramfs (`kos-initramfs`)**:
   - Specified in `boot/limine.conf:11-12`:
     ```
     module_path: boot():/boot/initramfs.cpio
     module_string: kos-initramfs
     ```
   - Limine loads the archive into memory as a boot module.
   - Initialized in `kernel/main.c:337-340`:
     ```c
     const struct limine_file *initramfs_module = find_initramfs_module();
     initramfs_initialize(initramfs_module->address, initramfs_module->size);
     vfs_mount_root(&initramfs_vfs_backend);
     ```
   - Mounted as root `C:`. Read-only, parsed into memory descriptors.
2. **RAMFS (`Data RAM`)**:
   - Initialized in `kernel/main.c:340-341`:
     ```c
     ramfs_initialize();
     vfs_mount_volume('D', "Data RAM", &ramfs_vfs_backend);
     ```
   - Mounted as volume `D:`. Read-write in-memory filesystem.
3. **VFS File Interface**:
   - `vfs_read_file(const char *path, const uint8_t **data, uint64_t *size)`
   - Returns a direct memory pointer `*data` to the file contents (both CPIO modules and RAMFS buffers are memory-resident).
4. **Host Packaging Pipeline (`tools/kos.c`)**:
   - `make_initramfs()` in `tools/kos.c:224-254` builds `build/initramfs.cpio` using CPIO newc format (`070701`).
   - For Milestone K14, `tools/kos.c` can be extended to compile user C programs (e.g. `user/init.c`, `user/hello.c`) with user flags (`-mno-red-zone`, user mode CRT stub), and package them into `build/initramfs.cpio` as `C:/bin/hello.elf`.

---

## 3. Recommendations & Required Architectural Changes for R1

The following matrix specifies the exact files, changes, and design decisions needed for Milestone K14 R1:

### 3.1 Architectural Change Matrix

| Component | Target File(s) | Required Modifications | Rationale |
|---|---|---|---|
| **VMM Header** | `include/kos/vmm.h` | 1. Add `VMM_PAGE_USER = 1ull << 2`<br>2. Add address space lifecycle declarations:<br>`uint64_t vmm_create_address_space(void);`<br>`void vmm_destroy_address_space(uint64_t pml4_phys);`<br>`bool vmm_map_page_in(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags);`<br>`uint64_t vmm_unmap_page_in(uint64_t pml4_phys, uint64_t va);`<br>`void vmm_switch_address_space(uint64_t pml4_phys);` | Enable user page flags and per-process address space manipulation without breaking existing kernel VMM usage. |
| **VMM Core** | `mm/vmm.c` | 1. Update flag validation to permit `VMM_PAGE_USER`.<br>2. Implement `vmm_create_address_space()`: allocates PML4, clears entries 0..255, clones entries 256..511 from `vmm.pml4`.<br>3. Implement `vmm_destroy_address_space()`: recursively walks entries 0..255, frees all leaf frames and owned page tables (PT, PD, PDPT), frees PML4 frame.<br>4. Update `next_table()`: propagate `VMM_PAGE_USER` to intermediate directory entries when mapping user space ($va < 0x800000000000$). | Provide true address space isolation and prevent supervisor-only faults in user mode. |
| **CPU Control** | `include/kos/cpu.h`<br>`arch/x86_64/cpu_control.c` | Add `void cpu_write_cr3(uint64_t cr3);`<br>```c<br>void cpu_write_cr3(uint64_t cr3) {<br>    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");<br>}<br>``` | Hardware instruction necessary to switch page tables on task switch. |
| **Process Model** | `include/kos/process.h`<br>`kernel/process.c` | 1. Add fields to `struct process`:<br>`uint64_t user_entry;`<br>`uint64_t user_stack_top;`<br>`uint64_t user_stack_bottom;`<br>`uint64_t user_stack_guard;`<br>2. In `process_create()`: allocate independent address space via `vmm_create_address_space()`.<br>3. In `process_destroy()`: invoke `vmm_destroy_address_space(proc->pml4_physical)`. | Ensure processes own distinct PML4s and clean up all allocated user frames on exit without memory leaks. |
| **Task & Context Switching** | `kernel/task.c` | 1. In `schedule()`: check if `next->process->pml4_physical != cpu_read_cr3()` and switch CR3 via `cpu_write_cr3()`.<br>2. In `schedule()`: update `tss_set_rsp0((uint64_t)next->stack + next->stack_size);`. | Guarantees hardware interrupts and exceptions from Ring 3 switch to the thread's dedicated kernel stack and running address space. |
| **ELF64 Subsystem** | `include/kos/elf.h`<br>`fs/elf.c` | 1. Define standard `Elf64_Ehdr`, `Elf64_Phdr`, `PT_*`, `PF_*`.<br>2. Implement `elf_load_binary(const uint8_t *data, uint64_t size, struct process *proc, uint64_t *out_entry)`:<br>- Validates headers, limits, alignments, canonical bounds.<br>- Maps `PT_LOAD` segments into `proc->pml4_physical`.<br>- Zeros BSS segments.<br>- Graceful rollback on error. | Fulfills requirement R1 for loading ELF64 binaries into userspace. |
| **User Stack Setup** | `kernel/process.c` (or `kernel/user_stack.c`) | Implement `process_setup_user_stack(struct process *proc, int argc, char **argv, char **envp, uint64_t *out_rsp)`:<br>- Allocates 64 KiB stack.<br>- Maps pages with `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.<br>- Leaves bottom page unmapped as guard page.<br>- Formats initial stack per System V AMD64 ABI (argc, argv, envp, auxv, 16-byte alignment). | Enables standard C user programs to parse arguments and protects against stack overflow. |
| **Fault Handling** | `arch/x86_64/interrupts.c` | In `page_fault_handler()`:<br>Replace panic/halt for `context->error_code & PF_ERROR_USER`. Terminate faulting process via `process_exit(proc, -1)` and reschedule. | Prevents user-space crashes from taking down the OS kernel. |
| **Ring 3 Entry** | `arch/x86_64/task_switch.asm` | Implement `task_enter_userspace(uint64_t rip, uint64_t rsp)`:<br>Load user data segments (`0x33`), push `iretq` frame (SS=0x33, RSP, RFLAGS=0x202, CS=0x2B, RIP), execute `iretq`. | Hardware-standard transition from Ring 0 to Ring 3. |

---

## 4. Architectural Sequence for R1 Execution

```
[User Binary on C:/ or D:/]
          │
          ▼ vfs_read_file()
[Contiguous In-Memory Buffer]
          │
          ▼ elf_validate_headers() (Magic, 64-bit, Static ET_EXEC, Alignment, Bounds)
[Validated ELF Image]
          │
          ├───────────────────────────────────────────────────────┐
          ▼ vmm_create_address_space()                           ▼
[New PML4: Zeroed Lower Half 0..255, Cloned Kernel Half 256..511]  │
          │                                                       │
          ▼ elf_map_pt_load_segments()                            │
[Mapped Code (RX), Rodata (R), Data/BSS (RW) with VMM_PAGE_USER]  │
          │                                                       │
          ▼ process_setup_user_stack()                            │
[Mapped 64KB User Stack + Unmapped Guard Page + System V ABI RSP]  │
          │                                                       │
          ▼ thread_create_user(proc, entry_point, user_rsp)       │
[Runnable User Thread in Scheduler Queue]                         │
          │                                                       │
          ▼ schedule()                                            │
[CR3 <- proc->pml4_physical, TSS.rsp0 <- thread->kernel_stack]    │
          │                                                       │
          ▼ task_enter_userspace() via iretq                      │
[Execution in Ring 3 Userspace] <─────────────────────────────────┘
```

---
*End of Analysis Report.*
