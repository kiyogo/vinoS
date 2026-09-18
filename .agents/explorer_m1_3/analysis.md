# Milestone K13/K14 Technical Analysis: Process Integration, Build Toolchain & Automated Self-Tests (Milestone 1)

**Agent**: `explorer_m1_3`  
**Role**: M1 Process Integration, Build Toolchain & Self-Test Explorer  
**Milestone**: K13/K14 M1 (Address Space Isolation, Ring 3 Userspace & ELF Loader)  
**Date**: 2026-09-18T10:20:00Z  
**Project Root**: `D:\DATA\source\OS\KOS\OS2`  

---

## 1. Scope & Objective

Milestone 1 (M1) establishes per-process isolated address spaces, safe user-memory access routines, and MSR hardware primitives. The specific responsibility of `explorer_m1_3` is to design the complete integration bridge across three critical areas:
1. **Process Lifecycle Integration** (`include/kos/process.h`, `kernel/process.c`):
   - Embed `struct address_space *as` into `struct process`.
   - Update `process_create()` to initialize `proc->as = NULL`.
   - Update `process_exec()` to allocate `new_as = as_create()`, map the ELF binary and user stack into `new_as`, update `proc->as = new_as`, maintain synchronized `proc->pml4_physical = new_as->pml4_phys`, and clean up any previous address space via `as_unref(old_as)`.
   - Update `process_destroy()` to release `proc->as` via `as_unref(proc->as)` with a backward-compatible fallback for raw PML4 reclamation.
2. **Build System Updates** (`tools/kos.c`, `tools/kos-tool.exe`):
   - Add `"mm/address_space.c"` to `c_sources[]` in `tools/kos.c`.
   - Recompile `tools/kos-tool.exe` from `tools/kos.c` using the hosted Clang toolchain.
   - Verify that `.\tools\kos-tool.exe build` compiles and links `mm/address_space.c` with zero warnings and zero errors.
3. **M1 Automated Self-Tests** (`address_space_self_test()`, `kernel/command.c`):
   - Design a self-contained test suite `address_space_self_test()` verifying:
     - `as_create()` and `as_destroy()` maintain exact frame parity: `pmm_free_frame_count()` before == after.
     - `copy_from_user()` and `copy_to_user()` strictly reject NULL, kernel addresses (`0xFFFFFFFF80000000`, `0xFFFF800000000000`), out-of-bounds addresses (`>= 0x0000800000000000`), integer overflow addresses (`ptr + len < ptr`), unmapped pages, and enforce read-only protection.
     - `copy_from_user()` and `copy_to_user()` successfully transfer data across 4 KiB page boundaries.
     - `strndup_from_user()` retrieves valid strings, enforces length clamping, and cleanly rejects bad or unmapped user addresses without causing a kernel panic.
   - Register shell command `astest` in `kernel/command.c` and integrate into kernel boot validation.

---

## 2. Subsystem State & Analysis

### 2.1 Current `struct process` Representation
In `include/kos/process.h:27-42`:
```c
struct process {
    uint64_t pid;
    char name[PROCESS_NAME_MAX];
    uint64_t ppid;
    enum process_state state;
    int exit_code;
    struct list_head threads;
    uint32_t thread_count;
    uint64_t pml4_physical;
    kos_spinlock_t lock;
    struct list_head process_node;
    struct list_head children;
    struct list_head sibling_node;
    struct handle_table *handles;
    struct wait_queue exit_wq;
};
```
Observations:
- `pml4_physical` is stored as a raw 64-bit integer, used by `schedule()` and `task_switch()` for rapid CR3 reloads.
- There is currently no `struct address_space *as` pointer in `struct process`.
- `kernel/process.c:process_exec()` directly calls `vmm_create_address_space()` and assigns the raw PML4 frame to `proc->pml4_physical`.
- `kernel/process.c:process_destroy()` directly calls `vmm_destroy_address_space(proc->pml4_physical)`.

Design Solution:
- Forward declare `struct address_space;` in `include/kos/process.h` to avoid circular header includes.
- Add `struct address_space *as;` to `struct process`.
- Retain `uint64_t pml4_physical;` as a fast cached accessor for the scheduler and task switch assembly routines.

### 2.2 Address Space Lifecycle in `kernel/process.c`

#### 2.2.1 Initialization (`process_subsystem_init` & `process_create`)
- In `process_subsystem_init()`:
  - Array slots `processes[i].as = NULL;`
  - `idle_process.as = NULL;` (runs in kernel master page table).
  - `kernel_process.as = NULL;` (runs in kernel master page table).
- In `process_create(const char *name)`:
  - When a process is created, it starts as a kernel PCB container:
    ```c
    proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;
    proc->as = NULL;
    ```
  - This avoids allocating redundant PML4 tables for kernel threads or processes prior to binary execution.

#### 2.2.2 Binary Execution (`process_exec`)
In `kernel/process.c:358-427`:
```c
int process_exec(const char *path, int argc, char *const argv[]) {
    if (path == NULL) return -1;
    struct process *proc = process_get_current();
    if (proc == NULL) return -1;

    /* 1. Allocate isolated user address space */
    struct address_space *new_as = as_create();
    if (new_as == NULL) {
        return -1;
    }
    uint64_t pml4_phys = new_as->pml4_phys;

    /* 2. Load ELF binary image into new address space */
    struct elf_binary_image img;
    bool ok = elf_load_binary(path, pml4_phys, &img);
    if (!ok) {
        as_destroy(new_as);
        return -1;
    }

    /* 3. Allocate and format user stack per System V AMD64 ABI */
    uint64_t user_rsp = 0;
    ok = process_setup_user_stack(pml4_phys, argc, argv, &user_rsp);
    if (!ok) {
        as_destroy(new_as);
        return -1;
    }

    /* 4. Track user stack region in VMA */
    (void)as_map_region(new_as, USER_STACK_BOTTOM, USER_STACK_SIZE,
                        VMA_READ | VMA_WRITE | VMA_STACK, "[stack]");

    /* 5. Clean up previous address space if present */
    struct address_space *old_as = proc->as;
    uint64_t old_pml4 = proc->pml4_physical;

    proc->as = new_as;
    proc->pml4_physical = new_as->pml4_phys;

    if (old_as != NULL) {
        as_unref(old_as);
    } else if (old_pml4 != 0 && old_pml4 != kernel_process.pml4_physical) {
        vmm_destroy_address_space(old_pml4);
    }

    /* 6. Update process name */
    const char *basename = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }
    size_t nlen = 0;
    while (basename[nlen] != '\0' && nlen < sizeof(proc->name) - 1) {
        proc->name[nlen] = basename[nlen];
        nlen++;
    }
    proc->name[nlen] = '\0';

    /* 7. Ensure TSS RSP0 is set to current thread kernel stack */
    struct thread *cur_thread = thread_current();
    if (cur_thread != NULL && cur_thread->stack != NULL) {
        tss_set_rsp0(((uint64_t)cur_thread->stack + cur_thread->stack_size) & ~0xFull);
    }

    /* 8. Load new address space into CR3 */
    cpu_write_cr3(pml4_phys);

    /* 9. Transition to Ring 3 userspace */
    enter_userspace(img.entry_point, user_rsp);

    return 0;
}
```

#### 2.2.3 Process Destruction (`process_destroy`)
In `kernel/process.c:242-248`:
```c
    /* 6. Release private user address space */
    struct address_space *as = proc->as;
    proc->as = NULL;
    uint64_t user_pml4 = proc->pml4_physical;
    proc->pml4_physical = 0;

    if (as != NULL) {
        as_unref(as);
    } else if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical) {
        /* Fallback for legacy raw PML4 */
        vmm_destroy_address_space(user_pml4);
    }
```
Key Benefits:
- Zero double-free hazard: `proc->as` and `proc->pml4_physical` are nulled before destruction.
- Proper refcount decrement: `as_unref(as)` handles multi-thread/child address space sharing.
- Backward compatibility: if `proc->as` is NULL but `pml4_physical` holds a private PML4, `vmm_destroy_address_space()` cleans it up.

---

## 3. Build System Updates (`tools/kos.c`)

### 3.1 Source File Inventory
In `tools/kos.c:23-36`, `c_sources[]` lists all freestanding C source files compiled by Clang.
Add `"mm/address_space.c"` to the array:
```c
    "kernel/terminal.c", "mm/address_space.c", "mm/heap.c", "mm/pmm.c", "mm/vmm.c",
```

### 3.2 Compilation Mechanics
1. `object_path("mm/address_space.c", ...)` produces `"build/obj/mm_address_space.o"` automatically by replacing directory separators with underscores.
2. Clang compiles `mm/address_space.c` with flags:
   `--target=x86_64-unknown-none-elf -std=c17 -ffreestanding -Wall -Wextra -Werror -O2 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -g -I include ...`
3. Linker `ld.lld` includes `build/obj/mm_address_space.o` in intermediate and final link commands.
4. Host tool executable `tools/kos-tool.exe` is recompiled with:
   ```powershell
   & "C:\Program Files\LLVM\bin\clang.exe" tools/kos.c -o tools/kos-tool.exe
   ```
5. `tools\kos-tool.exe build` succeeds with zero warnings and zero errors.

---

## 4. M1 Automated Self-Tests (`address_space_self_test()`)

### 4.1 Test Matrix

| Test ID | Name | Target Functions | Verification Condition |
|---|---|---|---|
| **T1** | Address Space Lifecycle & Leak Audit | `as_create()`, `as_destroy()`, `as_ref()`, `as_unref()`, `as_map_region()` | `pmm_free_frame_count()` before == after (0 frame leak), initial refcount 1, correct PML4 entry isolation (0..255 == 0, 256..511 == kernel) |
| **T2** | Bad Address Rejection | `copy_from_user()`, `copy_to_user()`, `user_memory_validate()` | Returns `-EFAULT` (-14) on `NULL`, higher-half text (`0xFFFFFFFF80000000`), HHDM (`0xFFFF800000000000`), non-canonical (`0x0000800000000000`), boundary crossing, 64-bit overflow (`ptr + len < ptr`), and unmapped pages. 0 kernel panic. |
| **T3** | Valid User Copy & Write Protection | `copy_to_user()`, `copy_from_user()` | Byte-exact match on mapped user page. Rejection with `-EFAULT` when writing to user read-only page. Successful read from read-only page. |
| **T4** | Page-Boundary Spanning Transfer | `copy_to_user()`, `copy_from_user()` | Copies 64 bytes spanning across `0x00400FE0` (page 1 into page 2). Data verified byte-for-byte. |
| **T5** | Safe String Duplication | `strndup_from_user()` | Returns allocated string matching user memory. Rejects NULL, kernel, and unmapped. Clamps string length when `max_len` < string length. |
| **T6** | Process Integration & Teardown | `process_create()`, `proc->as`, `process_destroy()` | Destroying process cleanly invokes `as_unref()`, releases all VMAs and frames, and restores PMM free frame count to baseline. |

### 4.2 Execution Tracing
The self-test runs:
1. Automatically during boot inside `process_subsystem_init()` (or `kernel/main.c`).
2. Interactively via shell command `astest` in `kernel/terminal.c`.

---

## 5. Risk Analysis & Mitigations

1. **Risk**: Direct user pointer dereference during copy causes kernel-mode page fault panic.  
   **Mitigation**: All copies validate user range, query physical frames via `vmm_query_page_in()`, and copy strictly through kernel HHDM virtual addresses (`vmm_physical_to_virtual(phys)`). Kernel page faults are impossible.
2. **Risk**: Rebuilding `tools/kos.c` fails if host compiler is not in PATH.  
   **Mitigation**: `tools/kos.c` explicitly checks `C:/Program Files/LLVM/bin/clang.exe` directly on Windows before falling back to PATH.
3. **Risk**: Stale `pml4_physical` cached in `struct process` diverges from `as->pml4_phys`.  
   **Mitigation**: `process_exec()` unconditionally sets `proc->pml4_physical = new_as->pml4_phys` at the same time `proc->as = new_as` is assigned.
