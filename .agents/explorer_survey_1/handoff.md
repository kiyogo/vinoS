# Technical Survey & Handoff: VMM, Paging & Address Space Isolation (Milestone K13/K14)

**Agent**: `explorer_survey_1` (VMM & Paging Architecture Explorer)  
**Target Milestone**: K13 & K14 (Address Space Isolation, Ring 3 Userspace & ELF Loader)  
**Date**: 2026-09-18T10:05:00Z  
**Repository**: `D:\DATA\source\OS\KOS\OS2`  

---

## 1. Observation

### 1.1 Source File Inventory & Locations
Direct examination of the KOS codebase reveals the memory and process management subsystem is organized as follows:

| Subsystem / Module | File Path | Key Data Structures | Key Functions |
|---|---|---|---|
| **PMM (Physical Memory Manager)** | `mm/pmm.c`<br>`include/kos/pmm.h` | `struct physical_memory_manager` (`usable_bitmap`, `free_bitmap`, `free_frames`, `tracked_usable_frames`) | `pmm_initialize()`, `pmm_allocate_frame()`, `pmm_free_frame()`, `pmm_alloc_pages()`, `pmm_free_pages()`, `pmm_free_frame_count()`, `pmm_allocated_frame_count()` |
| **VMM (Virtual Memory Manager)** | `mm/vmm.c`<br>`include/kos/vmm.h` | `struct virtual_memory_manager` (`hhdm_offset`, `pml4`, `kernel_pml4_phys`, `initialized`) | `vmm_initialize()`, `vmm_create_address_space()`, `vmm_destroy_address_space()`, `vmm_clear_user_address_space()`, `vmm_map_page_in()`, `vmm_unmap_page_in()`, `vmm_query_page_in()`, `vmm_set_page_permissions()`, `vmm_physical_to_virtual()`, `vmm_virtual_to_physical()` |
| **Kernel Heap** | `mm/heap.c`<br>`include/kos/heap.h` | `struct heap_block_header`, `struct heap_allocator` | `heap_initialize()`, `kmalloc()`, `kfree()`, `heap_self_test()`, `heap_mapped_page_count()`, `heap_active_allocation_count()` |
| **Process Subsystem** | `kernel/process.c`<br>`include/kos/process.h` | `struct process` (`pid`, `ppid`, `state`, `pml4_physical`, `threads`, `handles`, `exit_wq`) | `process_subsystem_init()`, `process_create()`, `process_exec()`, `process_exit()`, `process_destroy()`, `process_get_current()`, `process_wait()` |
| **Task / Scheduler** | `kernel/task.c`<br>`include/kos/task.h` | `struct thread` (`tid`, `context`, `process`, `stack`, `state`) | `task_initialize()`, `schedule()`, `task_switch()`, `thread_create()`, `thread_exit()`, `task_yield()` |
| **CPU / GDT / TSS** | `arch/x86_64/descriptors.c`<br>`include/kos/descriptors.h`<br>`arch/x86_64/cpu.asm` | `gdt_entries[7]`, `struct task_state_segment` (`rsp0`, `ist1`) | `gdt_initialize()`, `tss_set_rsp0()`, `tss_get_rsp0()`, `gdt_load()`, `tss_load()`, `enter_userspace()` |
| **CPU Control & Registers** | `arch/x86_64/cpu_control.c`<br>`include/kos/cpu.h` | None | `cpu_read_cr2()`, `cpu_read_cr3()`, `cpu_write_cr3()`, `cpu_invalidate_page()`, `cpu_enable_interrupts()`, `cpu_interrupt_save_disable()`, `cpu_interrupt_restore()` |
| **Interrupts / Page Fault** | `arch/x86_64/interrupts.c`<br>`include/kos/interrupts.h`<br>`include/kos/page_fault.h` | `struct idt_entry`, `struct interrupt_context`, `struct page_fault_record` | `idt_initialize()`, `interrupt_dispatch()`, `page_fault_handler()`, `page_fault_decode()`, `report_page_fault()` |
| **Syscall Dispatcher** | `kernel/syscall.c`<br>`include/kos/syscall.h` | `enum kos_syscall_number` | `kos_syscall_dispatch()`, `kos_syscall_name()` |
| **ELF64 Binary Loader** | `fs/elf.c`<br>`include/kos/elf.h` | `Elf64_Ehdr`, `Elf64_Phdr`, `struct elf_binary_image` | `elf_load_binary()`, `process_setup_user_stack()`, `elf_inspect_image()`, `elf_self_test()` |

---

### 1.2 PML4 Initialization & Memory Range Observations
1. **Limine Higher-Half Direct Map (HHDM)**:
   - Initialized in `mm/vmm.c:129-146`:
     ```c
     bool vmm_initialize(uint64_t hhdm_offset) {
         uint64_t cr3 = cpu_read_cr3() & PAGE_ADDRESS_MASK;
         if (hhdm_offset == 0 || hhdm_offset % PAGE_SIZE != 0 || cr3 == 0) {
             return false;
         }
         uint64_t flags = spinlock_lock_irqsave(&vmm_lock);
         vmm.hhdm_offset = hhdm_offset;
         vmm.pml4 = physical_to_virtual(cr3);
         vmm.kernel_pml4_phys = cr3;
         vmm.initialized = true;
         spinlock_unlock_irqrestore(&vmm_lock, flags);
         ...
     }
     ```
   - Physical address conversion uses direct addition/subtraction:
     `vmm_physical_to_virtual(phys) = (void *)(vmm.hhdm_offset + phys)`
     `vmm_virtual_to_physical(virt) = (uint64_t)virt - vmm.hhdm_offset`
   - **No recursive paging is used**. All page table manipulation occurs through HHDM virtual addresses.
2. **Virtual Memory Layout**:
   - **User Space Range**: `0x00000000_00000000` .. `0x00007FFF_FFFFFFFF` (128 TiB, corresponding to PML4 entries 0 through 255).
   - **Non-Canonical Hole**: `0x00008000_00000000` .. `0xFFFF7FFF_FFFFFFFF`.
   - **Kernel Higher-Half Range**: `0xFFFF8000_00000000` .. `0xFFFFFFFF_FFFFFFFF` (128 TiB, corresponding to PML4 entries 256 through 511).
     - HHDM mapping: starts at `hhdm_offset` (typically `0xFFFF8000_00000000`, PML4 entry 256).
     - Kernel image: linked at `0xFFFFFFFF_80000000` (`boot/linker.ld:16`), which is in PML4 entry 511 (`511 = (0xffffffff80000000 >> 39) & 0x1ff`).
3. **Cloning Kernel Higher-Half Mappings**:
   - In `mm/vmm.c:148-171` (`vmm_create_address_space`):
     ```c
     uint64_t *new_pml4 = physical_to_virtual(pml4_frame);
     for (uint64_t index = 0; index < 256; ++index) {
         new_pml4[index] = 0;
     }
     for (uint64_t index = 256; index < 512; ++index) {
         new_pml4[index] = vmm.pml4[index];
     }
     ```
   - Entries 0..255 (user space) are strictly zeroed out.
   - Entries 256..511 (kernel space) are copied verbatim from the kernel master PML4 (`vmm.pml4`), synchronizing HHDM, kernel image, kernel heap, and device MMIO across all address spaces.
4. **Page Table Walk & Allocation**:
   - `next_table()` in `mm/vmm.c:97-125` walks down the 4-level paging structure:
     - PML4 (`shift = 39`) -> PDPT (`shift = 30`) -> PD (`shift = 21`) -> PT (`shift = 12`) -> Leaf PTE.
     - If an entry is absent, it allocates a frame using `pmm_allocate_frame()`, zeroes it with `clear_page_table()`, and marks the entry with `PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE`.
     - If the target mapping is in user space (`va < 0x0000800000000000ull` or `flags & VMM_PAGE_USER`), it sets `PAGE_USER` on the entry.
     - Rollback mechanism: `vmm_map_page_in:317-330` automatically frees intermediate tables if allocation fails halfway through the walk via `release_owned_empty_table()`.

---

### 1.3 Page Table Flags & CPU Hardware State
1. **Permission Flags Defined in `include/kos/vmm.h` and `mm/vmm.c`**:
   - `PAGE_PRESENT = 1ull << 0` (Bit 0): Page is present in physical memory.
   - `VMM_PAGE_WRITABLE = 1ull << 1` (Bit 1): Read/write permission.
   - `VMM_PAGE_USER = 1ull << 2` (Bit 2): User accessible (CPL 3).
   - `PAGE_HUGE = 1ull << 7` (Bit 7): Large page (2 MiB in PDE, 1 GiB in PDPTE).
   - `PAGE_KOS_OWNED_TABLE = 1ull << 9` (Bit 9, Ignored/Software bit): Indicates table was dynamically allocated by KOS VMM.
   - `PAGE_KOS_OWNED_MAPPING = 1ull << 10` (Bit 10, Ignored/Software bit): Indicates leaf mapping was allocated by KOS VMM.
   - `VMM_PAGE_NO_EXECUTE = 1ull << 63` (Bit 63): Execute-Disable (NX/XD bit).
2. **EFER.NXE Status**:
   - Direct inspection of Limine bootloader source (`third_party/limine/common/protos/limine_32.asm_x86:49-56`):
     ```asm
     mov ecx, 0xc0000080   ; IA32_EFER MSR
     xor edx, edx
     mov eax, 1 << 8       ; LME
     cmp dword [esp+32], 0 ; nx_available
     je .no_nx
     or eax, 1 << 11       ; NXE (bit 11)
     .no_nx:
     wrmsr
     ```
   - Limine enables `EFER.NXE` during bootstrap.
   - KOS verifies and sets `VMM_PAGE_NO_EXECUTE` on `.data`, `.bss`, boot stack, and framebuffer pages in `kernel/main.c:280-292` without triggering `#PF` reserved-bit faults.
   - **Gap**: KOS currently has no `cpu_read_msr` or `cpu_write_msr` functions in `arch/x86_64/cpu_control.c` or `include/kos/cpu.h`.
3. **Caching Bits**:
   - PCD (Bit 4), PWT (Bit 3), PAT (Bit 7 for PTE / Bit 12 for PDE/PDPTE) are not defined in `vmm.h`.
   - `vmm_map_page_in:269` enforces: `(flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE)) == 0`.
   - All standard pages default to Write-Back (WB) caching.

---

### 1.4 Process & Address Space State
1. **Current `struct process` Representation**:
   - In `include/kos/process.h:27-42`:
     `uint64_t pml4_physical;` is stored directly as a raw 64-bit integer.
   - There is no separate `struct address_space` or `struct vm_area` abstraction.
2. **Address Space Lifecycle in `kernel/process.c`**:
   - `process_create()` (lines 136-144): assigns `proc->pml4_physical = cpu_read_cr3() & ~0xFFFull`. Does NOT create a new address space.
   - `process_exec()` (lines 370-398): calls `vmm_create_address_space()`, loads ELF (`elf_load_binary`), sets up user stack (`process_setup_user_stack`), frees old private PML4 if any, and assigns `proc->pml4_physical = pml4_phys`.
   - `process_destroy()` (lines 243-247): calls `vmm_destroy_address_space(user_pml4)`.
3. **Address Space Destruction Implementation**:
   - In `mm/vmm.c:173-246` (`vmm_clear_user_address_space`):
     - Explicitly loops only from `pml4_idx = 0` to `255`.
     - Recursively traverses present PDPTs, PDs, PTs.
     - For every present leaf PTE: calls `pmm_free_frame(leaf_phys)` and zeroes entry.
     - For every intermediate table: calls `pmm_free_frame(table_phys)` and zeroes parent entry.
     - Never touches indices 256..511 (kernel mappings are completely preserved).
     - If current CR3 matches `pml4_phys`, restores CR3 to `vmm.kernel_pml4_phys`.
   - In `mm/vmm.c:248-264` (`vmm_destroy_address_space`):
     - Calls `vmm_clear_user_address_space(pml4_phys)`.
     - Frees the PML4 root table frame via `pmm_free_frame(pml4_phys)`.

---

### 1.5 User-Memory Copy Subsystem State
1. **Current Codebase Gap**:
   - Neither `copy_from_user` nor `copy_to_user` nor `strndup_from_user` exists in the codebase.
   - In `kernel/syscall.c:16-20`:
     ```c
     case KOS_SYSCALL_WRITE:
         /* Pointer validation and a user copy belong to the process VM
          * layer. Refuse this call until that layer is active. */
         (void)arg0;
         return UINT64_MAX;
     ```
2. **Page Fault Panic Behavior**:
   - In `arch/x86_64/interrupts.c:256-260`:
     ```c
     bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);
     if (!is_user) {
         /* Kernel mode page fault is unrecoverable -> Panic & Halt */
         log_page_fault_panic(context, &record);
     }
     ```
   - Any page fault triggered while CPL=0 (kernel mode) immediately triggers a kernel panic and halts the machine.
   - Therefore, a raw dereference of a user-supplied pointer inside kernel mode will crash the entire operating system if the user passes an invalid or unmapped address.

---

### 1.6 Physical Memory Tracking & Leak Prevention
1. **Bitmap Tracking in `mm/pmm.c`**:
   - Tracks 64 GiB using `usable_bitmap` and `free_bitmap`.
   - Monitored counters:
     - `pmm_free_frame_count()`: returns `pmm.free_frames`.
     - `pmm_allocated_frame_count()`: returns `pmm.tracked_usable_frames - pmm.free_frames`.
2. **Self-Test Verification**:
   - `vmm_self_test()` (`mm/vmm.c:546-628`) and `elf_self_test()` (`fs/elf.c:413-476`) both sample `pmm_free_frame_count()` before and after address space creation, mapping, unmapping, and destruction.
   - Both self-tests assert `final_free == initial_free`, proving 0 frame leaks.

---

## 2. Logic Chain

### 2.1 From PML4 Initialization to Address Space Isolation
1. **Observation**: Limine establishes 64-bit mode with kernel mapped at `0xFFFFFFFF_80000000` (PML4 entry 511) and physical RAM directly mapped at `hhdm_offset` (PML4 entries 256+).
2. **Deduction**: Virtual address translation for any physical frame `P` can be achieved by `(void *)(vmm.hhdm_offset + P)` without recursive page tables.
3. **Observation**: In `vmm_create_address_space()`, PML4 entries 0..255 are initialized to 0, while entries 256..511 are copied from `vmm.pml4`.
4. **Deduction**: Each user process receives a dedicated, isolated lower-half address space (`0x00000000_00000000` .. `0x00007FFF_FFFFFFFF`), while retaining identical higher-half kernel mappings (`0xFFFF8000_00000000` .. `0xFFFFFFFF_FFFFFFFF`).
5. **Deduction**: Because entries 256..511 point to the shared kernel PDPTs, kernel address space updates (e.g. heap expansion, driver mappings) are immediately and automatically visible in all process address spaces without synchronization overhead.

### 2.2 From Address Space Destruction to Zero Memory Leaks
1. **Observation**: `vmm_clear_user_address_space()` strictly bounds its iteration to `pml4_idx < 256`.
2. **Deduction**: Intermediate page tables and physical frames belonging to the kernel higher half (entries 256..511) are never dereferenced, unmapped, or freed during user address space cleanup.
3. **Observation**: Every leaf PTE in entries 0..255 has its physical address passed to `pmm_free_frame()`, and every dynamically allocated table frame (PT, PD, PDPT) is freed via `pmm_free_frame()`. Finally, the PML4 root frame is freed.
4. **Conclusion**: Address space destruction safely reclaims 100% of user physical pages and page table frames without leaking memory or corrupting kernel mappings.

### 2.3 From Kernel Page Fault Panic to Safe User Copy Design
1. **Observation**: Any page fault occurring in kernel mode (`CS & 3 == 0`) triggers an immediate unrecoverable kernel panic and halts the system (`arch/x86_64/interrupts.c:257`).
2. **Observation**: KOS does not currently have an exception fixup table (`extable`) to catch and recover from kernel page faults.
3. **Deduction**: If `copy_from_user` or `copy_to_user` directly dereferences a raw user virtual pointer, an unmapped user address or intentional attack will panic the kernel.
4. **Solution**: User memory copy functions must perform software validation before accessing memory:
   - Verify pointer canonical bounds: `ptr != NULL`, `ptr < 0x0000800000000000ull`, `ptr + len <= 0x0000800000000000ull`, and `ptr + len >= ptr` (no overflow).
   - Validate each 4 KiB page using `vmm_query_page_in()`: verify page is present, user-accessible (`VMM_PAGE_USER`), and writable if destination.
   - Perform the actual byte copy chunk-by-chunk using the physical address translated via HHDM:
     `void *kaddr = (void *)((uint8_t *)vmm_physical_to_virtual(phys) + page_offset);`
     `memcpy(dst, kaddr, chunk);`
5. **Conclusion**: Copying through HHDM operates entirely on valid kernel direct-mapped virtual addresses, making it physically impossible to trigger a page fault in kernel mode, ensuring 100% memory safety.

---

## 3. Caveats

1. **Shared Memory / Copy-on-Write (CoW)**:
   - Currently, `vmm_clear_user_address_space()` assumes every leaf page mapped in user space is uniquely owned by the process and calls `pmm_free_frame()` on it.
   - If Milestones K14/K15 introduce `fork()` with CoW pages or shared memory mappings, a physical frame reference counter (`struct page` or frame refcount array) will be required so frames are freed only when their refcount reaches 0.
2. **EFER MSR Access**:
   - `EFER.NXE` is verified to be enabled by Limine, but KOS currently lacks inline MSR read/write helpers (`cpu_read_msr`, `cpu_write_msr`). These should be added to `arch/x86_64/cpu_control.c` and `include/kos/cpu.h` to allow runtime MSR queries and support Syscall MSR setup (`IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`).
3. **Preemption & Address Space Switching**:
   - `task_switch()` in `kernel/task.c:303` already checks `next->process->pml4_physical` and writes `CR3`. When introducing `struct address_space`, callers should access `proc->as->pml4_phys` or retain `proc->pml4_physical` as a fast cached accessor for the scheduler context switch hot path.

---

## 4. Conclusion

### 4.1 Architecture Assessment
KOS's memory management foundation (PMM + VMM) is clean, robust, and well-designed:
- **PMM** provides reliable bitmap-based frame allocation with lock-irqsave synchronization and frame accounting.
- **VMM** already implements complete 4-level x86_64 page table management, HHDM translation, address space cloning (copying PML4[256..511] and zeroing PML4[0..255]), user space unmapping, and address space destruction with 0 leaks.
- **Descriptors & Transitions**: GDT has User CS (`0x2B`) and User DS (`0x33`). TSS has `rsp0` switching. `enter_userspace()` performs `iretq` with clean register wiping. Vector 0x80 IDT gate is configured with DPL 3 (`0xee`).
- **Missing Elements for Milestone K13/K14**:
  1. Formal `struct address_space` and `struct vm_area` abstraction linking to `struct process`.
  2. Safe user copy primitives (`copy_from_user`, `copy_to_user`, `strndup_from_user`, `user_memory_validate`) using HHDM chunk copying to prevent kernel page fault panics.
  3. MSR read/write helpers (`cpu_read_msr`, `cpu_write_msr`) in `cpu_control.c` / `cpu.h`.
  4. Expanded core syscall handlers (`sys_exit`, `sys_write`, `sys_read`, `sys_gettime`, `sys_sysinfo`, `sys_yield`, `sys_sleep`) in `kernel/syscall.c`.

---

### 4.2 Proposed Data Structures & Headers

#### Proposed Header: `include/kos/address_space.h`
```c
#ifndef KOS_ADDRESS_SPACE_H
#define KOS_ADDRESS_SPACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <kos/atomic.h>
#include <kos/list.h>
#include <kos/spinlock.h>
#include <kos/vmm.h>

/* Virtual Memory Area flags */
#define VMA_READ    (1u << 0)
#define VMA_WRITE   (1u << 1)
#define VMA_EXEC    (1u << 2)
#define VMA_STACK   (1u << 3)
#define VMA_HEAP    (1u << 4)

struct vm_area {
    uint64_t start_va;          /* Start virtual address (page-aligned) */
    uint64_t end_va;            /* End virtual address (exclusive, page-aligned) */
    uint32_t flags;             /* VMA_* flags */
    const char *name;           /* Segment name (e.g. ".text", ".data", "[stack]") */
    struct list_head node;      /* Link in address_space->vma_list */
};

struct address_space {
    uint64_t pml4_phys;         /* Physical address for CR3 */
    uint64_t *pml4_virt;        /* Virtual pointer in HHDM */
    kos_spinlock_t lock;        /* Protects VMA list and mappings */
    atomic_t refcount;          /* Reference count */
    struct list_head vma_list;  /* List of struct vm_area */
    uint64_t page_count;        /* Count of mapped user frames */
};

/* Address Space Lifecycle */
struct address_space *as_create(void);
void as_ref(struct address_space *as);
void as_unref(struct address_space *as);
void as_destroy(struct address_space *as);

/* VMA & Region Management */
bool as_map_region(struct address_space *as, uint64_t va, uint64_t size, uint32_t vma_flags, const char *name);
bool as_unmap_region(struct address_space *as, uint64_t va, uint64_t size);
struct vm_area *as_find_vma(struct address_space *as, uint64_t va);

/* Safe User Memory Copy Functions */
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access);
int copy_from_user(void *dst, const void *src_user, size_t n);
int copy_to_user(void *dst_user, const void *src, size_t n);
char *strndup_from_user(const char *src_user, size_t max_len);

#endif /* KOS_ADDRESS_SPACE_H */
```

#### Proposed Safe Copy Implementation Sketch (in `mm/address_space.c` or `mm/vmm.c`)
```c
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access) {
    if (user_ptr == NULL || len == 0) {
        return false;
    }
    uint64_t start = (uint64_t)user_ptr;
    uint64_t end = start + len;
    /* Strict canonical user space boundary check */
    if (end < start || end > 0x0000800000000000ull) {
        return false;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return false;
    }

    uint64_t start_page = start & ~(KOS_PAGE_SIZE - 1);
    uint64_t end_page = (end + KOS_PAGE_SIZE - 1) & ~(KOS_PAGE_SIZE - 1);

    for (uint64_t page = start_page; page < end_page; page += KOS_PAGE_SIZE) {
        uint64_t phys = 0;
        uint64_t flags = 0;
        if (!vmm_query_page_in(proc->pml4_physical, page, &phys, &flags)) {
            return false;
        }
        if ((flags & VMM_PAGE_USER) == 0) {
            return false;
        }
        if (write_access && (flags & VMM_PAGE_WRITABLE) == 0) {
            return false;
        }
    }
    return true;
}

int copy_from_user(void *dst, const void *src_user, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (!user_memory_validate(src_user, n, false)) {
        return -14; /* -EFAULT */
    }

    struct process *proc = process_get_current();
    uint64_t user_va = (uint64_t)src_user;
    uint8_t *kdst = (uint8_t *)dst;
    size_t remaining = n;

    while (remaining > 0) {
        uint64_t page_off = user_va & (KOS_PAGE_SIZE - 1);
        size_t chunk = KOS_PAGE_SIZE - page_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint64_t phys = 0;
        vmm_query_page_in(proc->pml4_physical, user_va & ~(KOS_PAGE_SIZE - 1), &phys, NULL);
        const void *src_hhdm = (const void *)((uint8_t *)vmm_physical_to_virtual(phys) + page_off);

        memcpy(kdst, src_hhdm, chunk);
        user_va += chunk;
        kdst += chunk;
        remaining -= chunk;
    }
    return 0;
}

int copy_to_user(void *dst_user, const void *src, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (!user_memory_validate(dst_user, n, true)) {
        return -14; /* -EFAULT */
    }

    struct process *proc = process_get_current();
    uint64_t user_va = (uint64_t)dst_user;
    const uint8_t *ksrc = (const uint8_t *)src;
    size_t remaining = n;

    while (remaining > 0) {
        uint64_t page_off = user_va & (KOS_PAGE_SIZE - 1);
        size_t chunk = KOS_PAGE_SIZE - page_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint64_t phys = 0;
        vmm_query_page_in(proc->pml4_physical, user_va & ~(KOS_PAGE_SIZE - 1), &phys, NULL);
        void *dst_hhdm = (void *)((uint8_t *)vmm_physical_to_virtual(phys) + page_off);

        memcpy(dst_hhdm, ksrc, chunk);
        user_va += chunk;
        ksrc += chunk;
        remaining -= chunk;
    }
    return 0;
}

char *strndup_from_user(const char *src_user, size_t max_len) {
    if (src_user == NULL || max_len == 0) {
        return NULL;
    }
    uint64_t start = (uint64_t)src_user;
    if (start >= 0x0000800000000000ull) {
        return NULL;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return NULL;
    }

    /* First pass: measure string length safely */
    size_t len = 0;
    uint64_t cur_va = start;
    bool nul_found = false;

    while (len < max_len) {
        uint64_t phys = 0;
        uint64_t flags = 0;
        if (!vmm_query_page_in(proc->pml4_physical, cur_va & ~(KOS_PAGE_SIZE - 1), &phys, &flags)) {
            return NULL;
        }
        if ((flags & VMM_PAGE_USER) == 0) {
            return NULL;
        }

        uint64_t page_off = cur_va & (KOS_PAGE_SIZE - 1);
        const char *ch_ptr = (const char *)((uint8_t *)vmm_physical_to_virtual(phys) + page_off);

        if (*ch_ptr == '\0') {
            nul_found = true;
            break;
        }
        len++;
        cur_va++;
        if (cur_va >= 0x0000800000000000ull) {
            return NULL;
        }
    }

    if (!nul_found && len == max_len) {
        /* Null terminator not found within max_len */
    }

    char *buf = kmalloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    if (copy_from_user(buf, src_user, len) != 0) {
        kfree(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}
```

---

### 4.3 Step-by-Step Implementation Plan for Milestone K13/K14

1. **Step 1: CPU Control & MSR Helpers**:
   - In `arch/x86_64/cpu_control.c` and `include/kos/cpu.h`:
     Add `uint64_t cpu_read_msr(uint32_t msr)` and `void cpu_write_msr(uint32_t msr, uint64_t val)`.
   - Add constants: `IA32_EFER = 0xC0000080`, `IA32_STAR = 0xC0000081`, `IA32_LSTAR = 0xC0000082`, `IA32_FMASK = 0xC0000084`.
   - Verify `EFER.NXE` programmatically in `vmm_initialize()`.

2. **Step 2: Struct `address_space` and VMA Subsystem**:
   - Create `include/kos/address_space.h` and implement `as_create()`, `as_destroy()`, `as_ref()`, `as_unref()`.
   - Embed `struct address_space *as;` in `struct process` (in `include/kos/process.h`).
   - Retain `proc->pml4_physical` as a fast accessor synchronized with `proc->as->pml4_phys`.

3. **Step 3: Safe User Memory Copy Routines**:
   - Implement `user_memory_validate()`, `copy_from_user()`, `copy_to_user()`, `strndup_from_user()`.
   - Implement comprehensive self-test `user_copy_self_test()` verifying rejection of `NULL`, out-of-bounds addresses, kernel space addresses (`>= 0x0000800000000000ull`), integer overflow addresses, unmapped pages, and verifying successful chunk copy across page boundaries.

4. **Step 4: Update Process Subsystem & Lifecycle**:
   - In `kernel/process.c`:
     - In `process_create()`: initialize `proc->as = NULL;` (or create address space for user processes).
     - In `process_exec()`: use `as_create()`, map ELF segments into VMAs, set up user stack, assign `proc->as`, update `proc->pml4_physical`.
     - In `process_destroy()`: call `as_unref(proc->as)`.

5. **Step 5: Upgrade Syscall Layer**:
   - In `kernel/syscall.c`:
     - Upgrade `KOS_SYSCALL_WRITE`: use `copy_from_user()` to copy string buffer into a kernel scratch buffer before printing via `console_write()` / `serial_write()`.
     - Implement `SYS_EXIT` (1), `SYS_WRITE` (2), `SYS_READ` (3), `SYS_GETTIME` (4), `SYS_SYSINFO` (5), `SYS_YIELD` (6), `SYS_SLEEP` (7) conforming to Milestone K13/K14 specification.

6. **Step 6: User-Mode Smoke Test (`usertest` Command)**:
   - Implement shell command `usertest` in `kernel/command.c`.
   - The test loads an embedded user-mode ELF binary (or constructs a test user address space with a minimal Ring 3 code sequence calling `int 0x80`), enters Ring 3, performs syscalls, exits, and verifies:
     - Ran in CPL 3.
     - Intercepted page fault on deliberate bad address without kernel crash.
     - Reaped zombie process cleanly.
     - PMM frame count before == after (0 leaks).

---

## 5. Verification Method

### 5.1 Automated Build Verification
Verify kernel compilation with zero warnings and zero errors:
```powershell
.\tools\kos-tool.exe build
```
- **Expected Outcome**: `build/kos.elf` generated successfully with embedded symbol table, return code 0.
- **Observed Test Run**: Verified on `2026-09-18T09:59:27Z`: `tools\kos-tool.exe build` completed with return code 0, 817 embedded symbols, UEFI ESP staged.

### 5.2 Address Space Lifecycle & Leak Verification
Verify by checking self-tests in `mm/vmm.c:vmm_self_test()` and `fs/elf.c:elf_self_test()`:
1. `initial_free = pmm_free_frame_count();`
2. `as = as_create();`
3. Map multiple pages (code, data, stack) with `vmm_map_page_in()`.
4. Validate pages with `vmm_query_page_in()`.
5. `as_unref(as);`
6. `final_free = pmm_free_frame_count();`
7. Assert `final_free == initial_free`.

### 5.3 User Pointer Safety Verification
Test cases for `copy_from_user` and `copy_to_user`:
1. `copy_from_user(kbuf, NULL, 10)` -> returns `-EFAULT`, 0 kernel panic.
2. `copy_from_user(kbuf, (void *)0xFFFFFFFF80000000ull, 10)` -> returns `-EFAULT` (rejects kernel address), 0 kernel panic.
3. `copy_from_user(kbuf, (void *)0x00007FFFFFFFFFF0ull, 32)` -> returns `-EFAULT` (crosses canonical user boundary), 0 kernel panic.
4. `copy_from_user(kbuf, (void *)0x00400000ull, 100)` on unmapped user page -> returns `-EFAULT`, 0 kernel panic.
5. `copy_from_user(kbuf, valid_user_mapped_page, 100)` -> returns `0`, data matches byte-for-byte.

### 5.4 Invalidation Conditions
The findings in this report would be invalidated if:
- KOS migrates from Limine bootloader to a bootloader that does not provide Higher-Half Direct Map (HHDM).
- Hardware architecture changes from x86_64 4-level paging (PML4) to 5-level paging (PML5) without updating `PAGE_ADDRESS_MASK` and indexing shifts.
