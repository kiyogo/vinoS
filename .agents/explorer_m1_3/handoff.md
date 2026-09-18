# Handoff Report: M1 Process Integration, Build Toolchain & Automated Self-Tests

**Agent**: `explorer_m1_3`  
**Role**: M1 Process Integration, Toolchain & Test Explorer  
**Milestone**: K13/K14 M1 (Address Space Isolation, Ring 3 Userspace & ELF Loader)  
**Date**: 2026-09-18T10:22:00Z  
**Project Root**: `D:\DATA\source\OS\KOS\OS2`  
**Working Directory**: `D:\DATA\source\OS\KOS\OS2\.agents\explorer_m1_3`  

---

## 1. Observation

Direct examination of the KOS repository, header files, and build tools revealed the following facts, line numbers, and behaviors:

### 1.1 `struct process` and Address Space Tracking (`include/kos/process.h`)
In `include/kos/process.h:27-42`:
```c
27: struct process {
28:     uint64_t pid;
29:     char name[PROCESS_NAME_MAX];
30:     uint64_t ppid;
31:     enum process_state state;
32:     int exit_code;
33:     struct list_head threads;
34:     uint32_t thread_count;
35:     uint64_t pml4_physical;
36:     kos_spinlock_t lock;
37:     struct list_head process_node;
38:     struct list_head children;
39:     struct list_head sibling_node;
40:     struct handle_table *handles;
41:     struct wait_queue exit_wq;
42: };
```
- `pml4_physical` is currently the sole address space descriptor stored in `struct process`.
- `struct address_space *as;` is absent.
- Forward declaration `struct address_space;` is absent.

### 1.2 Process Lifecycle Functions in `kernel/process.c`
1. **Process Creation (`kernel/process.c:136-144`)**:
   ```c
   136:     proc->pid = next_pid++;
   137:     struct process *cur = process_get_current();
   138:     proc->ppid = cur ? cur->pid : 1;
   139:     proc->state = PROCESS_STATE_ACTIVE;
   140:     proc->exit_code = 0;
   141:     list_init(&proc->threads);
   142:     proc->thread_count = 0;
   143:     proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;
   144:     spinlock_init(&proc->lock);
   ```
   `proc->pml4_physical` is populated with the boot CR3 (kernel PML4). `proc->as` is not initialized.
2. **Binary Execution (`kernel/process.c:368-398`)**:
   ```c
   368:     /* 1. Create a private user address space */
   369:     uint64_t pml4_phys = vmm_create_address_space();
   ...
   391:     uint64_t old_pml4 = proc->pml4_physical;
   392:     if (old_pml4 != 0 && old_pml4 != kernel_process.pml4_physical) {
   393:         vmm_destroy_address_space(old_pml4);
   394:     }
   395: 
   396:     /* 5. Assign new address space */
   397:     proc->pml4_physical = pml4_phys;
   ```
   `process_exec()` operates directly on raw PML4 physical addresses via `vmm_create_address_space()` and `vmm_destroy_address_space()`, bypassing `struct address_space`.
3. **Process Destruction (`kernel/process.c:242-248`)**:
   ```c
   243:     uint64_t user_pml4 = proc->pml4_physical;
   244:     proc->pml4_physical = 0;
   245:     if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical) {
   246:         vmm_destroy_address_space(user_pml4);
   247:     }
   ```
   Only the raw PML4 frame is destroyed. Any higher-level metadata (VMA list, reference count) is not tracked or freed.

### 1.3 Build System Source List in `tools/kos.c`
In `tools/kos.c:23-36`:
```c
23: static const char *const c_sources[] = {
...
35:     "kernel/terminal.c", "mm/heap.c", "mm/pmm.c", "mm/vmm.c",
36: };
```
- `"mm/address_space.c"` is absent from `c_sources[]`.
- `object_path()` in `tools/kos.c:142-160` generates `"build/obj/mm_address_space.o"` automatically from `"mm/address_space.c"` by replacing path slashes with underscores and substituting the extension with `.o`.
- The existing build toolchain was tested:
  - `.\tools\kos-tool.exe check` exited with code 0 (LLVM Clang, NASM, LLD, llvm-nm, QEMU detected).
  - `.\tools\kos-tool.exe build` exited with code 0:
    `Kernel ELF built with 818 embedded symbols: build/kos.elf`
    `UEFI ESP staged: build/esp`.

### 1.4 Self-Test Architecture in KOS
- Subsystems expose standalone self-tests returning `bool`:
  - `pmm_self_test()` (`kernel/main.c:152`)
  - `heap_self_test()` (`kernel/main.c:121`)
  - `vmm_self_test()` (`mm/vmm.c:546`)
  - `elf_self_test()` (`fs/elf.c:413`)
  - `process_lifecycle_self_test()` (`kernel/process.c:687`)
- All memory-related self-tests enforce zero physical frame leaks by asserting:
  `final_free_frames == initial_free_frames` (`pmm_free_frame_count()`).
- In `kernel/command.c`, shell commands dispatch self-test runners (e.g., `mutextest`, `pftest`, `usertest`).

---

## 2. Logic Chain

```
[Observation 1.1 & 1.2: Raw pml4_physical in struct process]
                     |
                     v
[Logic Step 1: Address Space Encapsulation]
Add struct address_space *as to struct process; forward declare struct address_space in include/kos/process.h.
Synchronize proc->pml4_physical = proc->as->pml4_phys to maintain fast O(1) CR3 access in schedule() and task_switch().
                     |
                     v
[Logic Step 2: Lifecycle Management in process.c]
- process_create(): initialize proc->as = NULL; proc->pml4_physical = kernel CR3.
- process_exec(): allocate new_as = as_create(); on success, clean up old_as via as_unref(old_as); assign proc->as = new_as.
- process_destroy(): call as_unref(proc->as); retain fallback for raw PML4 to prevent memory leaks during tests.
                     |
                     v
[Observation 1.3: tools/kos.c c_sources[] excludes mm/address_space.c]
                     |
                     v
[Logic Step 3: Build Toolchain Integration]
Add "mm/address_space.c" to c_sources[] in tools/kos.c.
Recompile tools/kos-tool.exe with host Clang.
Verification: object_path converts "mm/address_space.c" to "build/obj/mm_address_space.o" and links into build/kos.elf.
                     |
                     v
[Observation 1.4: Zero-leak PMM self-test standard]
                     |
                     v
[Logic Step 4: Comprehensive M1 Automated Self-Tests]
Implement address_space_self_test() in mm/address_space.c.
Verify:
1. as_create / as_destroy maintains pmm_free_frame_count() before == after.
2. copy_from_user / copy_to_user rejects NULL, kernel addresses, out-of-bounds, unmapped pages, and respects read-only permissions.
3. Spanning copies across 4 KiB page boundaries succeed without data corruption.
4. strndup_from_user validates string length, clamping, NULL, and unmapped rejection.
5. Teardown of test process and address space results in 0 frame leaks.
```

1. **Address Space Lifetime & Fast Switching**:
   - `task_switch()` in `arch/x86_64/task_switch.asm` and `kernel/task.c:303` reads `next->process->pml4_physical` and loads it into CR3.
   - Retaining `proc->pml4_physical` alongside `proc->as` guarantees that the hot scheduler context switch path requires no pointer chasing or indirection.
   - Initializing `proc->as = NULL` in `process_create()` ensures kernel worker threads and intermediate PCBs consume 0 unnecessary user page tables.

2. **Clean Reclamation & Refcounting**:
   - In `process_destroy()`, calling `as_unref(proc->as)` decrements `as->refcount`. When the count reaches 0, `as_destroy(as)` is called, which calls `vmm_destroy_address_space(as->pml4_phys)` and frees all VMAs and the `struct address_space` memory.
   - Clearing `proc->as = NULL` and `proc->pml4_physical = 0` prior to unreferencing guarantees idempotent teardown and prevents double-free vulnerabilities.
   - The fallback check `else if (user_pml4 != 0 && user_pml4 != kernel_process.pml4_physical)` preserves compatibility with legacy unit tests.

3. **Safe Memory Access Invariance**:
   - In KOS, any kernel-mode page fault triggers an immediate unrecoverable kernel panic (`arch/x86_64/interrupts.c:257`).
   - `copy_from_user()`, `copy_to_user()`, and `strndup_from_user()` prevent kernel faults by:
     a. Strictly rejecting non-canonical pointers and addresses `>= 0x0000800000000000ull`.
     b. Querying the process PML4 via `vmm_query_page_in()` for each touched page to verify `PAGE_PRESENT`, `VMM_PAGE_USER`, and `VMM_PAGE_WRITABLE` (if destination).
     c. Performing the byte copy chunk-by-chunk through the kernel Higher-Half Direct Map (HHDM) (`vmm_physical_to_virtual(phys)`).
   - Because the copy operates exclusively on HHDM kernel addresses, hardware page faults cannot occur during user-memory copying.

---

## 3. Caveats

1. **Heap Initialization Dependency**:
   - `as_create()` allocates `struct address_space` via `kmalloc()`, and `as_map_region()` allocates `struct vm_area` via `kmalloc()`.
   - Consequently, `address_space_self_test()` and address space creation cannot run before `heap_initialize()` finishes in `kernel/main.c:294`. Running it in `process_subsystem_init()` (invoked by `task_initialize()` at line 412) is completely safe.
2. **Current Thread Context for User Copy**:
   - `copy_from_user()` and `copy_to_user()` resolve target mappings using `process_get_current()`.
   - In standalone self-tests running on the bootstrap kernel thread, the test temporarily associates the thread with the test process (`cur_thread->process = test_proc;`), runs the tests, and restores the original process pointer before destroying `test_proc`.
3. **No CoW or Fork in M1**:
   - In Milestone 1, each user physical frame mapped in `struct address_space` is uniquely owned by that address space. Page frames are freed directly when the address space is destroyed. Milestone K15 (fork/CoW) will introduce frame-level refcounting.

---

## 4. Conclusion & Precise Code Modifications

### 4.1 Changes to `include/kos/process.h`

```c
/* Forward declarations */
struct handle_table;
struct address_space; /* Added forward declaration */

struct process {
    uint64_t pid;
    char name[PROCESS_NAME_MAX];
    uint64_t ppid;
    enum process_state state;
    int exit_code;
    struct list_head threads;
    uint32_t thread_count;
    uint64_t pml4_physical;
    struct address_space *as; /* Added isolated address space pointer */
    kos_spinlock_t lock;
    struct list_head process_node;
    struct list_head children;
    struct list_head sibling_node;
    struct handle_table *handles;
    struct wait_queue exit_wq;
};
```

### 4.2 Changes to `kernel/process.c`

#### Header Include
```c
#include <kos/address_space.h>
```

#### `process_create()` Modification
```c
    proc->pid = next_pid++;
    struct process *cur = process_get_current();
    proc->ppid = cur ? cur->pid : 1;
    proc->state = PROCESS_STATE_ACTIVE;
    proc->exit_code = 0;
    list_init(&proc->threads);
    proc->thread_count = 0;
    proc->pml4_physical = cpu_read_cr3() & ~0xFFFull;
    proc->as = NULL; /* Initialized to NULL */
    spinlock_init(&proc->lock);
```

#### `process_destroy()` Modification
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

#### `process_exec()` Modification
```c
int process_exec(const char *path, int argc, char *const argv[]) {
    if (path == NULL) {
        return -1;
    }

    struct process *proc = process_get_current();
    if (proc == NULL) {
        return -1;
    }

    /* 1. Create a private user address space */
    struct address_space *new_as = as_create();
    if (new_as == NULL) {
        return -1;
    }
    uint64_t pml4_phys = new_as->pml4_phys;

    /* 2. Load the ELF binary image into the private address space */
    struct elf_binary_image img;
    bool ok = elf_load_binary(path, pml4_phys, &img);
    if (!ok) {
        as_destroy(new_as);
        return -1;
    }

    /* 3. Allocate and set up user stack with argc/argv formatted per System V ABI */
    uint64_t user_rsp = 0;
    ok = process_setup_user_stack(pml4_phys, argc, argv, &user_rsp);
    if (!ok) {
        as_destroy(new_as);
        return -1;
    }

    /* Track user stack region in VMA */
    (void)as_map_region(new_as, USER_STACK_BOTTOM, USER_STACK_SIZE,
                        VMA_READ | VMA_WRITE | VMA_STACK, "[stack]");

    /* 4. Clean up old user address space if proc had a previous private address space */
    struct address_space *old_as = proc->as;
    uint64_t old_pml4 = proc->pml4_physical;

    proc->as = new_as;
    proc->pml4_physical = new_as->pml4_phys;

    if (old_as != NULL) {
        as_unref(old_as);
    } else if (old_pml4 != 0 && old_pml4 != kernel_process.pml4_physical) {
        vmm_destroy_address_space(old_pml4);
    }

    /* 5. Update process name to match binary filename */
    const char *basename = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            basename = p + 1;
        }
    }
    size_t nlen = 0;
    while (basename[nlen] != '\0' && nlen < sizeof(proc->name) - 1) {
        proc->name[nlen] = basename[nlen];
        nlen++;
    }
    proc->name[nlen] = '\0';

    /* 6. Ensure TSS RSP0 is configured with kernel stack for current thread */
    struct thread *cur_thread = thread_current();
    if (cur_thread != NULL && cur_thread->stack != NULL) {
        tss_set_rsp0(((uint64_t)cur_thread->stack + cur_thread->stack_size) & ~0xFull);
    }

    /* 7. Switch to the new address space */
    cpu_write_cr3(pml4_phys);

    /* 8. Jump to Ring 3 userspace (noreturn) */
    enter_userspace(img.entry_point, user_rsp);

    /* Unreachable */
    return 0;
}
```

#### `process_subsystem_init()` Update
```c
    for (size_t i = 0; i < PROCESS_MAX_COUNT; i++) {
        processes[i].state = PROCESS_STATE_UNUSED;
        processes[i].pid = 0;
        processes[i].thread_count = 0;
        processes[i].as = NULL; /* Initialized to NULL */
        list_init(&processes[i].threads);
        ...
    }

    /* Idle Process */
    idle_process.as = NULL;

    /* Kernel Process */
    kernel_process.as = NULL;
```

#### `process_lifecycle_self_test()` Update
```c
    /* 3. Test private user address space creation and recursive reclamation (zero leaks) */
    uint64_t frames_before = pmm_free_frame_count();
    struct process *leak_proc = process_create("leak_test_proc");
    if (leak_proc == NULL) {
        log_error("  [FAIL] Failed to create leak test process");
        return false;
    }

    struct address_space *as = as_create();
    if (as == NULL) {
        log_error("  [FAIL] Failed to create address space for test process");
        return false;
    }

    uint64_t f1 = pmm_allocate_frame();
    uint64_t f2 = pmm_allocate_frame();
    if (f1 == 0 || f2 == 0) {
        log_error("  [FAIL] Failed to allocate frames for mapping test");
        as_destroy(as);
        return false;
    }
    vmm_map_page_in(as->pml4_phys, 0x400000ull, f1, VMM_PAGE_USER | VMM_PAGE_WRITABLE);
    vmm_map_page_in(as->pml4_phys, 0x500000ull, f2, VMM_PAGE_USER | VMM_PAGE_WRITABLE);

    leak_proc->as = as;
    leak_proc->pml4_physical = as->pml4_phys;
    process_destroy(leak_proc);

    uint64_t frames_after = pmm_free_frame_count();
    if (frames_after != frames_before) {
        log_errorf("  [FAIL] Address space reclamation leaked frames: before=%llu, after=%llu",
                   (unsigned long long)frames_before, (unsigned long long)frames_after);
        return false;
    }
    log_info("  [PASS] Address space reclamation verified with zero frame leaks.");
```

---

### 4.3 Changes to `tools/kos.c`

In `tools/kos.c:23-36`:
```c
static const char *const c_sources[] = {
    "boot/limine_requests.c", "arch/x86_64/cpu_control.c", "arch/x86_64/descriptors.c",
    "arch/x86_64/interrupts.c", "arch/x86_64/pci.c", "arch/x86_64/pic.c",
    "drivers/ahci.c", "drivers/block.c", "drivers/driver.c", "drivers/e1000.c", "drivers/framebuffer.c",
    "drivers/keyboard.c", "drivers/ramdisk.c", "drivers/serial.c", "drivers/timer.c", "drivers/virtio_blk.c",
    "fs/block_cache.c", "fs/elf.c", "fs/fat32.c", "fs/initramfs.c", "fs/partition.c", "fs/ramfs.c", "fs/vfs.c", "fs/volume.c",
    "kernel/command.c", "kernel/console.c", "kernel/log.c", "kernel/main.c", "kernel/mutex.c",
    "kernel/panic.c", "kernel/spinlock.c", "kernel/symbol.c", "kernel/syscall.c", "kernel/system.c",
    "kernel/process.c", "kernel/task.c", "kernel/object.c", "kernel/handle.c", "kernel/wait.c",
    "lib/crypto.c", "lib/memory.c",
    "net/arp.c", "net/dhcp.c", "net/dns.c", "net/ethernet.c", "net/http.c", "net/httpd.c", "net/icmp.c",
    "net/ipv4.c", "net/net_device.c", "net/socket.c", "net/socket_subsystem.c", "net/tcp.c", "net/tls.c",
    "kernel/terminal.c", "mm/address_space.c", "mm/heap.c", "mm/pmm.c", "mm/vmm.c",
};
```

---

### 4.4 Automated Self-Test Implementation (`address_space_self_test()`)

Add to `mm/address_space.c` and declare in `include/kos/address_space.h`:
```c
bool address_space_self_test(void) {
    log_info("[as] Running Address Space & Safe User Copy Self-Test...");

    /* 1. as_create() and as_destroy() lifecycle & zero frame leakage */
    uint64_t initial_frames = pmm_free_frame_count();

    struct address_space *as = as_create();
    if (as == NULL) {
        log_error("  [FAIL] as_create() returned NULL");
        return false;
    }
    if (as->pml4_phys == 0 || (as->pml4_phys % KOS_PAGE_SIZE) != 0) {
        log_error("  [FAIL] Invalid PML4 physical address in new address space");
        as_destroy(as);
        return false;
    }
    if (as->pml4_virt == NULL) {
        log_error("  [FAIL] as->pml4_virt is NULL");
        as_destroy(as);
        return false;
    }
    if (atomic_read(&as->refcount) != 1) {
        log_error("  [FAIL] as->refcount is not 1 upon creation");
        as_destroy(as);
        return false;
    }
    if (!list_empty(&as->vma_list)) {
        log_error("  [FAIL] as->vma_list is not empty upon creation");
        as_destroy(as);
        return false;
    }

    /* Verify lower half (0..255) entries are 0 */
    for (uint64_t i = 0; i < 256; i++) {
        if (as->pml4_virt[i] != 0) {
            log_error("  [FAIL] Lower-half PML4 entry is not zeroed");
            as_destroy(as);
            return false;
        }
    }

    /* Test refcount increment & decrement */
    as_ref(as);
    if (atomic_read(&as->refcount) != 2) {
        log_error("  [FAIL] as_ref() did not increment refcount to 2");
        as_destroy(as);
        return false;
    }
    as_unref(as);
    if (atomic_read(&as->refcount) != 1) {
        log_error("  [FAIL] as_unref() did not decrement refcount back to 1");
        as_destroy(as);
        return false;
    }

    /* Map pages & test VMA tracking */
    uint64_t f1 = pmm_allocate_frame();
    uint64_t f2 = pmm_allocate_frame();
    if (f1 == 0 || f2 == 0) {
        log_error("  [FAIL] Failed to allocate frames for mapping");
        as_destroy(as);
        return false;
    }
    vmm_map_page_in(as->pml4_phys, 0x00400000ull, f1, VMM_PAGE_USER | VMM_PAGE_WRITABLE);
    vmm_map_page_in(as->pml4_phys, 0x00500000ull, f2, VMM_PAGE_USER | VMM_PAGE_WRITABLE);

    if (!as_map_region(as, 0x00400000ull, 4096, VMA_READ | VMA_WRITE, ".data")) {
        log_error("  [FAIL] as_map_region failed for .data");
        as_destroy(as);
        return false;
    }
    struct vm_area *vma = as_find_vma(as, 0x00400000ull);
    if (vma == NULL || vma->start_va != 0x00400000ull || (vma->flags & VMA_WRITE) == 0) {
        log_error("  [FAIL] as_find_vma failed to find mapped VMA");
        as_destroy(as);
        return false;
    }

    as_destroy(as);

    uint64_t post_destroy_frames = pmm_free_frame_count();
    if (post_destroy_frames != initial_frames) {
        log_errorf("  [FAIL] Address space destruction leaked frames: before=%llu, after=%llu",
                   (unsigned long long)initial_frames, (unsigned long long)post_destroy_frames);
        return false;
    }
    log_info("  [PASS] as_create() and as_destroy() lifecycle verified with zero leaks.");

    /* 2. copy_from_user & copy_to_user Bounds Checking & Safety */
    char kbuf[128];
    memset(kbuf, 0xAA, sizeof(kbuf));

    if (copy_from_user(kbuf, NULL, 16) != -14 || copy_to_user(NULL, kbuf, 16) != -14) {
        log_error("  [FAIL] NULL pointer copy did not return -EFAULT (-14)");
        return false;
    }
    if (copy_from_user(kbuf, (const void *)0x1000, 0) != 0 || copy_to_user((void *)0x1000, kbuf, 0) != 0) {
        log_error("  [FAIL] Zero-length copy did not return 0");
        return false;
    }
    if (copy_from_user(kbuf, (const void *)0xFFFFFFFF80000000ull, 16) != -14 ||
        copy_from_user(kbuf, (const void *)0xFFFF800000000000ull, 16) != -14 ||
        copy_to_user((void *)0xFFFFFFFF80000000ull, kbuf, 16) != -14) {
        log_error("  [FAIL] Kernel address copy did not return -EFAULT");
        return false;
    }
    if (copy_from_user(kbuf, (const void *)0x0000800000000000ull, 16) != -14 ||
        copy_from_user(kbuf, (const void *)0x00007FFFFFFFFFF0ull, 32) != -14 ||
        copy_from_user(kbuf, (const void *)0xFFFFFFFFFFFFFFF0ull, 32) != -14) {
        log_error("  [FAIL] Canonical limit or overflow address did not return -EFAULT");
        return false;
    }
    if (copy_from_user(kbuf, (const void *)0x00200000ull, 16) != -14 ||
        copy_to_user((void *)0x00200000ull, kbuf, 16) != -14) {
        log_error("  [FAIL] Unmapped user address did not return -EFAULT");
        return false;
    }
    log_info("  [PASS] copy_from_user & copy_to_user error boundaries strictly enforced.");

    /* 3. Valid User Space Copying & Page-Boundary Transfers */
    struct process *test_proc = process_create("as_test_proc");
    if (test_proc == NULL) {
        log_error("  [FAIL] Failed to create test process for user copy");
        return false;
    }
    struct address_space *test_as = as_create();
    if (test_as == NULL) {
        log_error("  [FAIL] Failed to create address space for test process");
        process_destroy(test_proc);
        return false;
    }
    test_proc->as = test_as;
    test_proc->pml4_physical = test_as->pml4_phys;

    uint64_t frame_rw1 = pmm_allocate_frame();
    vmm_map_page_in(test_as->pml4_phys, 0x00400000ull, frame_rw1, VMM_PAGE_USER | VMM_PAGE_WRITABLE);
    uint64_t frame_rw2 = pmm_allocate_frame();
    vmm_map_page_in(test_as->pml4_phys, 0x00401000ull, frame_rw2, VMM_PAGE_USER | VMM_PAGE_WRITABLE);
    uint64_t frame_ro = pmm_allocate_frame();
    vmm_map_page_in(test_as->pml4_phys, 0x00402000ull, frame_ro, VMM_PAGE_USER);

    void *ro_hhdm = vmm_physical_to_virtual(frame_ro);
    const char ro_text[] = "Read Only Protected User Content";
    memcpy(ro_hhdm, ro_text, sizeof(ro_text));

    struct thread *cur_thread = thread_current();
    struct process *saved_proc = cur_thread ? cur_thread->process : NULL;
    if (cur_thread != NULL) {
        cur_thread->process = test_proc;
    }

    const char sample_data[] = "KOS Kernel To User Space Copy Verification Data 1234567890";
    size_t sample_len = strlen(sample_data) + 1;
    if (copy_to_user((void *)0x00400020ull, sample_data, sample_len) != 0) {
        log_error("  [FAIL] copy_to_user failed on valid mapped user page");
        goto cleanup_test_proc;
    }

    char read_back[128];
    memset(read_back, 0, sizeof(read_back));
    if (copy_from_user(read_back, (const void *)0x00400020ull, sample_len) != 0 ||
        strcmp(read_back, sample_data) != 0) {
        log_error("  [FAIL] copy_from_user failed or data mismatch");
        goto cleanup_test_proc;
    }

    if (copy_to_user((void *)0x00402000ull, sample_data, 16) != -14) {
        log_error("  [FAIL] copy_to_user allowed writing to read-only user page");
        goto cleanup_test_proc;
    }

    char ro_read_back[64];
    memset(ro_read_back, 0, sizeof(ro_read_back));
    if (copy_from_user(ro_read_back, (const void *)0x00402000ull, sizeof(ro_text)) != 0 ||
        strcmp(ro_read_back, ro_text) != 0) {
        log_error("  [FAIL] copy_from_user failed on read-only user page");
        goto cleanup_test_proc;
    }

    /* Page-boundary spanning transfer across 0x00400FE0 into 0x00401000 */
    char cross_page_src[64];
    for (size_t i = 0; i < sizeof(cross_page_src); i++) {
        cross_page_src[i] = (char)('A' + (i % 26));
    }
    if (copy_to_user((void *)0x00400FE0ull, cross_page_src, sizeof(cross_page_src)) != 0) {
        log_error("  [FAIL] copy_to_user failed spanning page boundary");
        goto cleanup_test_proc;
    }
    char cross_page_dst[64];
    memset(cross_page_dst, 0, sizeof(cross_page_dst));
    if (copy_from_user(cross_page_dst, (const void *)0x00400FE0ull, sizeof(cross_page_dst)) != 0 ||
        memcmp(cross_page_dst, cross_page_src, sizeof(cross_page_src)) != 0) {
        log_error("  [FAIL] copy_from_user failed or data corrupted across page boundary");
        goto cleanup_test_proc;
    }
    log_info("  [PASS] copy_to_user & copy_from_user verified across page boundaries & permissions.");

    /* 4. strndup_from_user Validation */
    if (strndup_from_user(NULL, 100) != NULL ||
        strndup_from_user((const char *)0xFFFFFFFF80000000ull, 100) != NULL ||
        strndup_from_user((const char *)0x00200000ull, 100) != NULL) {
        log_error("  [FAIL] strndup_from_user failed to reject invalid pointers");
        goto cleanup_test_proc;
    }

    const char test_string[] = "KOS Userspace Safe String Test";
    copy_to_user((void *)0x00400100ull, test_string, sizeof(test_string));
    char *dup_str = strndup_from_user((const char *)0x00400100ull, 128);
    if (dup_str == NULL || strcmp(dup_str, test_string) != 0) {
        log_error("  [FAIL] strndup_from_user failed on valid user string");
        kfree(dup_str);
        goto cleanup_test_proc;
    }
    kfree(dup_str);

    char *clamped_str = strndup_from_user((const char *)0x00400100ull, 3);
    if (clamped_str == NULL || strlen(clamped_str) != 3 || strncmp(clamped_str, "KOS", 3) != 0) {
        log_error("  [FAIL] strndup_from_user did not clamp to max_len properly");
        kfree(clamped_str);
        goto cleanup_test_proc;
    }
    kfree(clamped_str);
    log_info("  [PASS] strndup_from_user validated with length clamping & error handling.");

cleanup_test_proc:
    if (cur_thread != NULL) {
        cur_thread->process = saved_proc;
    }
    process_destroy(test_proc);

    uint64_t final_frames = pmm_free_frame_count();
    if (final_frames != initial_frames) {
        log_errorf("  [FAIL] Test process teardown leaked frames: initial=%llu, final=%llu",
                   (unsigned long long)initial_frames, (unsigned long long)final_frames);
        return false;
    }

    log_info(">>> [PASS] ALL ADDRESS SPACE & SAFE USER COPY SELF-TESTS PASSED (0 frame leaks)! <<<");
    return true;
}
```

#### Shell Command in `kernel/command.c`
```c
static void command_astest(const char *arguments) {
    (void)arguments;
    if (address_space_self_test()) {
        console_printf("[PASS] Address Space & Safe User Copy Self-Test passed.\n");
    } else {
        console_printf("[FAIL] Address Space & Safe User Copy Self-Test failed.\n");
    }
}
```
And add to `command_entries[]`:
```c
    { "astest", "Run address space isolation and user copy self-tests", command_astest },
```

---

## 5. Verification Method

### 5.1 Host Toolchain Rebuild Verification
Execute host compilation of `tools/kos.c`:
```powershell
& "C:\Program Files\LLVM\bin\clang.exe" tools/kos.c -o tools/kos-tool.exe
```
**Pass Criteria**: Executable `tools/kos-tool.exe` generated with 0 errors.

### 5.2 Clean Kernel Compilation Verification
Run kernel build:
```powershell
.\tools\kos-tool.exe build
```
**Pass Criteria**:
- Exit code 0.
- Zero compiler warnings, zero compiler errors (`-Wall -Wextra -Werror`).
- `build/obj/mm_address_space.o` generated.
- `Kernel ELF built with symbols: build/kos.elf`.
- `UEFI ESP staged: build/esp`.

### 5.3 Automated Self-Test Execution in QEMU
Run QEMU:
```powershell
.\tools\kos-tool.exe run
```
At the interactive shell prompt, execute:
```shell
astest
```
**Pass Criteria**:
- Console outputs:
  ```
  [as] Running Address Space & Safe User Copy Self-Test...
    [PASS] as_create() and as_destroy() lifecycle verified with zero leaks.
    [PASS] copy_from_user & copy_to_user error boundaries strictly enforced.
    [PASS] copy_to_user & copy_from_user verified across page boundaries & permissions.
    [PASS] strndup_from_user validated with length clamping & error handling.
  >>> [PASS] ALL ADDRESS SPACE & SAFE USER COPY SELF-TESTS PASSED (0 frame leaks)! <<<
  [PASS] Address Space & Safe User Copy Self-Test passed.
  ```
- `pmm_free_frame_count()` before == after.

### 5.4 Invalidation Conditions
- If calling `copy_from_user(kbuf, NULL, 16)` or `copy_from_user(kbuf, 0xFFFFFFFF80000000, 16)` triggers a kernel `#PF` panic instead of returning `-EFAULT`.
- If repeated executions of `astest` or `process_create()` / `process_destroy()` loops cause `pmm_free_frame_count()` to decrement.
- If `tools\kos-tool.exe build` emits any warning or error.
