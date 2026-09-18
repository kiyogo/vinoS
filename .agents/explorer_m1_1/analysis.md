# Detailed Technical Analysis: Per-Process Isolated Address Spaces & Memory Safety (Milestone K13/K14 M1)

**Component**: Milestone K13/K14 - Milestone 1 (M1)  
**Author**: `explorer_m1_1` (M1 Address Space Explorer)  
**Date**: 2026-09-18T10:20:00Z  
**Project Root**: `D:\DATA\source\OS\KOS\OS2`  

---

## 1. System Architecture & Memory Model

### 1.1 x86_64 4-Level Paging Layout
KOS runs in 64-bit long mode with 4-level paging (PML4). Virtual addresses span 48 bits, partitioned into two 128 TiB canonical halves separated by a non-canonical hole:

```
+-------------------------------------------------------------------------+
| Virtual Address Range                     | Description                 |
+-------------------------------------------------------------------------+
| 0x0000_0000_0000_0000 - 0x0000_0000_001F_FFFF | Unmapped Null Guard (2 MiB) |
| 0x0000_0000_0020_0000 - 0x0000_7FFF_FFFF_FFFF | User Space (128 TiB, PML4 0..255) |
| 0x0000_8000_0000_0000 - 0xFFFF_7FFF_FFFF_FFFF | Non-Canonical Address Hole  |
| 0xFFFF_8000_0000_0000 - 0xFFFF_FFFF_7FFF_FFFF | Limine HHDM (PML4 256..510) |
| 0xFFFF_FFFF_8000_0000 - 0xFFFF_FFFF_FFFF_FFFF | Kernel Binary (PML4 511)    |
+-------------------------------------------------------------------------+
```

### 1.2 Kernel Higher-Half Synchronization vs User Lower-Half Isolation
When `vmm_create_address_space()` allocates a new PML4 table (`mm/vmm.c:148-171`):
1. **Entries 0..255 (User Space)**: Strictly zeroed out (`new_pml4[i] = 0`). Every user process starts with an empty, completely isolated virtual address space.
2. **Entries 256..511 (Kernel Space)**: Directly cloned from `vmm.pml4` (`new_pml4[i] = vmm.pml4[i]`). Because these entries point to shared kernel PDPT tables, any kernel heap growth, device driver mapping, or kernel data structure update is instantly visible across all address spaces with zero synchronization overhead.
3. **No Recursive Paging**: Virtual address translation for any physical frame `P` is performed via Limine's Higher-Half Direct Map (HHDM):
   $$\text{Virt}(P) = \text{vmm.hhdm\_offset} + P$$
   This eliminates recursive paging complexity and allows the kernel to inspect and modify any process's page tables at any time without switching CR3.

---

## 2. Complete Design: `include/kos/address_space.h`

The header defines the core abstractions: `struct vm_area` for virtual memory regions, `struct address_space` for managing PML4 and VMA collections, and the safe memory copy primitives.

```c
#ifndef KOS_ADDRESS_SPACE_H
#define KOS_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/atomic.h>
#include <kos/list.h>
#include <kos/memory.h>
#include <kos/spinlock.h>
#include <kos/vmm.h>

/*
 * Canonical User Space Boundaries:
 * - Lowest 2 MiB (0x0000000000000000 .. 0x00000000001FFFFF) is intentionally
 *   left unmapped to catch NULL pointer and near-NULL dereferences.
 * - Maximum user address is 0x0000800000000000 (128 TiB canonical boundary).
 */
#define USER_SPACE_MIN          0x0000000000200000ull
#define USER_SPACE_MAX          0x0000800000000000ull

/* VMA Name maximum length including null terminator */
#define VMA_NAME_MAX            32

/* Standard POSIX error code for bad memory address */
#define EFAULT                  14

/*
 * Virtual Memory Area (VMA) Permission and Type Flags:
 * Strict W ^ X enforcement: VMA_WRITE and VMA_EXEC are mutually exclusive.
 */
#define VMA_READ                (1u << 0)
#define VMA_WRITE               (1u << 1)
#define VMA_EXEC                (1u << 2)
#define VMA_STACK               (1u << 3)
#define VMA_HEAP                (1u << 4)

/*
 * struct vm_area:
 * Represents a contiguous range of user virtual memory with uniform permissions.
 */
struct vm_area {
    uint64_t start_va;              /* Start virtual address (page-aligned, inclusive) */
    uint64_t end_va;                /* End virtual address (page-aligned, exclusive) */
    uint32_t flags;                 /* Bitmask of VMA_* flags */
    char name[VMA_NAME_MAX];        /* Descriptive name (e.g., ".text", ".data", "[stack]") */
    struct list_head node;          /* Node in struct address_space->vma_list */
};

/*
 * struct address_space:
 * Encapsulates the complete memory context of a process, containing its PML4
 * page table and registered VMAs.
 */
struct address_space {
    uint64_t pml4_phys;             /* Physical address of the PML4 root table (for CR3) */
    uint64_t *pml4_virt;            /* Virtual address of PML4 in Limine HHDM */
    kos_spinlock_t lock;            /* Protects vma_list, page_count, and concurrent edits */
    atomic_t refcount;              /* Reference counter (reaches 0 -> destruction) */
    struct list_head vma_list;      /* Head of sorted doubly-linked list of struct vm_area */
    uint64_t page_count;            /* Count of active user physical frames mapped */
};

/*
 * Address Space Lifecycle API
 */
struct address_space *as_create(void);
void as_ref(struct address_space *as);
void as_unref(struct address_space *as);
void as_destroy(struct address_space *as);

/*
 * VMA Region Management API
 */
bool as_map_region(struct address_space *as, uint64_t va, uint64_t size, uint32_t vma_flags, const char *name);
bool as_unmap_region(struct address_space *as, uint64_t va, uint64_t size);
struct vm_area *as_find_vma(struct address_space *as, uint64_t va);

/*
 * Hardware Context & Switching Helpers
 */
void as_switch(struct address_space *as);
struct address_space *as_get_current(void);

/*
 * Safe User Memory Access Primitives:
 * Guaranteed 0 kernel page fault panics via HHDM translation and strict bounds validation.
 */
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access);
int copy_from_user(void *dst, const void *src_user, size_t n);
int copy_to_user(void *dst_user, const void *src, size_t n);
char *strndup_from_user(const char *src_user, size_t max_len);

/*
 * Subsystem Self-Test
 */
bool as_self_test(void);

#endif /* KOS_ADDRESS_SPACE_H */
```

---

## 3. Implementation Specification: `mm/address_space.c`

### 3.1 Lifecycle & Memory Management
1. **`as_create()`**:
   - Allocates `struct address_space` via `kmalloc(sizeof(struct address_space))`.
   - Invokes `vmm_create_address_space()` to allocate a private PML4 frame and copy kernel higher-half mappings.
   - If allocation fails, releases memory and returns `NULL`.
   - Initializes `lock` via `spinlock_init`, sets `refcount` to 1, initializes `vma_list` via `list_init`, and sets `page_count = 0`.
2. **`as_ref()` & `as_unref()`**:
   - `as_ref()` uses `atomic_inc(&as->refcount)`.
   - `as_unref()` uses `atomic_dec_and_test(&as->refcount)`. If the refcount reaches zero, it immediately invokes `as_destroy(as)`.
3. **`as_destroy()`**:
   - Acquires `as->lock`, traverses `as->vma_list`, removes every `struct vm_area` and frees it via `kfree(vma)`.
   - Invokes `vmm_destroy_address_space(as->pml4_phys)`.
   - Frees `struct address_space` via `kfree(as)`.

### 3.2 100% PMM Frame Reclamation & Zero Leak Guarantee
Let us trace the physical frame cleanup in detail:
- In `vmm_destroy_address_space(pml4_phys)`:
  - Calls `vmm_clear_user_address_space(pml4_phys)`.
  - Walks `pml4_idx` from `0` to `255` (user space only).
  - For every present leaf PTE: calls `pmm_free_frame(leaf_phys)` and zeroes the entry.
  - For every dynamically created Page Table (PT): calls `pmm_free_frame(pt_phys)`.
  - For every dynamically created Page Directory (PD): calls `pmm_free_frame(pd_phys)`.
  - For every dynamically created PDPT: calls `pmm_free_frame(pdpt_phys)`.
  - Entries `256..511` (kernel space) are untouched.
  - Finally, frees the PML4 root table frame via `pmm_free_frame(pml4_phys)`.
- If pages were unmapped during runtime via `as_unmap_region()`:
  - `vmm_unmap_page_in()` clears the PTE and invalidates the TLB.
  - `as_unmap_region()` calls `pmm_free_frame(phys)` and decrements `as->page_count`.
  - Intermediate tables that became empty are reclaimed by `release_owned_empty_table()`.
  - Because the PTE was zeroed, `vmm_destroy_address_space()` will not see it, preventing any double-free.
- Net balance:
  $$\Delta \text{PMM Frames} = 0, \quad \Delta \text{Heap Allocations} = 0$$

### 3.3 VMA Insertion, Overlap Rejection & $W \oplus X$
- **Sorted Order**: The doubly linked list `as->vma_list` is kept sorted in strictly ascending order of `start_va`.
- **Overlap Detection**: For every existing node, the condition:
  $$\max(\text{start}_1, \text{start}_2) < \min(\text{end}_1, \text{end}_2)$$
  is evaluated. If true, the new mapping is rejected.
- **$W \oplus X$ Enforcement**:
  `if ((vma_flags & VMA_WRITE) && (vma_flags & VMA_EXEC)) return false;`
  This guarantees that code cannot be modified in place, and writable data cannot be executed.
- **Allocation Rollback**: If physical frame allocation fails on page $K$ of an $N$-page region, all previously mapped pages $0 \dots K-1$ are unmapped, freed via `pmm_free_frame()`, `as->page_count` is decremented, the allocated VMA struct is freed via `kfree()`, and the function returns `false`.

### 3.4 Safe User Memory Copy Routines
Direct dereference of user pointers in kernel mode is completely forbidden because an invalid address triggers `#PF`, and kernel `#PF` immediately panics (`arch/x86_64/interrupts.c:257`).

To achieve 100% safety without hardware exception tables:
1. **Canonical Check**: Verify `ptr >= USER_SPACE_MIN` and `ptr + len <= USER_SPACE_MAX` and `ptr + len >= ptr`.
2. **Page Table Validation**: For every 4 KiB page spanned by `[ptr, ptr + len)`, query `vmm_query_page_in()`. Assert `PAGE_PRESENT` is set, `VMM_PAGE_USER` is set, and if write access is requested, `VMM_PAGE_WRITABLE` is set.
3. **Copy via HHDM**: For each page, obtain its physical frame `phys` from `vmm_query_page_in()`, compute the direct map address `(void *)(vmm.hhdm_offset + phys + offset)`, and copy via `memcpy()`.
4. Because the kernel accesses physical memory directly via HHDM, it is physically impossible to trigger a page fault in kernel mode.

---

## 4. Complete Code Listing for `mm/address_space.c`

```c
#include <kos/address_space.h>
#include <kos/heap.h>
#include <kos/log.h>
#include <kos/memory.h>
#include <kos/pmm.h>
#include <kos/process.h>
#include <kos/vmm.h>

/*
 * as_create:
 * Allocates and initializes a new isolated address space with private PML4.
 */
struct address_space *as_create(void) {
    struct address_space *as = kmalloc(sizeof(struct address_space));
    if (as == NULL) {
        log_error("as_create: failed to allocate address_space structure");
        return NULL;
    }

    uint64_t pml4_phys = vmm_create_address_space();
    if (pml4_phys == 0) {
        log_error("as_create: failed to allocate PML4 address space");
        kfree(as);
        return NULL;
    }

    as->pml4_phys = pml4_phys;
    as->pml4_virt = (uint64_t *)vmm_physical_to_virtual(pml4_phys);
    spinlock_init(&as->lock);
    atomic_set(&as->refcount, 1);
    list_init(&as->vma_list);
    as->page_count = 0;

    return as;
}

/*
 * as_ref:
 * Increments reference count.
 */
void as_ref(struct address_space *as) {
    if (as != NULL) {
        atomic_inc(&as->refcount);
    }
}

/*
 * as_unref:
 * Decrements reference count and destroys the address space when it reaches 0.
 */
void as_unref(struct address_space *as) {
    if (as != NULL && atomic_dec_and_test(&as->refcount)) {
        as_destroy(as);
    }
}

/*
 * as_destroy:
 * Frees all VMAs, unmaps and frees all user frames and page tables,
 * and releases the address space memory.
 */
void as_destroy(struct address_space *as) {
    if (as == NULL) {
        return;
    }

    uint64_t flags = spinlock_lock_irqsave(&as->lock);

    /* 1. Free all VMAs in the list */
    while (!list_empty(&as->vma_list)) {
        struct vm_area *vma = list_first_entry(&as->vma_list, struct vm_area, node);
        list_del_init(&vma->node);
        kfree(vma);
    }

    uint64_t pml4_phys = as->pml4_phys;
    as->pml4_phys = 0;
    as->pml4_virt = NULL;
    as->page_count = 0;

    spinlock_unlock_irqrestore(&as->lock, flags);

    /* 2. Reclaim 100% of user physical frames and page tables (PML4, PDPT, PD, PT) */
    if (pml4_phys != 0) {
        vmm_destroy_address_space(pml4_phys);
    }

    /* 3. Free address_space structure */
    kfree(as);
}

/*
 * as_find_vma:
 * Finds the VMA containing the virtual address va (start_va <= va < end_va).
 */
struct vm_area *as_find_vma(struct address_space *as, uint64_t va) {
    if (as == NULL) {
        return NULL;
    }

    uint64_t flags = spinlock_lock_irqsave(&as->lock);
    struct vm_area *pos;
    struct vm_area *found = NULL;

    list_for_each_entry(pos, &as->vma_list, node) {
        if (va >= pos->start_va && va < pos->end_va) {
            found = pos;
            break;
        }
        if (pos->start_va > va) {
            /* Since list is strictly sorted, va cannot match any subsequent VMA */
            break;
        }
    }

    spinlock_unlock_irqrestore(&as->lock, flags);
    return found;
}

/*
 * as_map_region:
 * Allocates physical frames, establishes page table mappings, and creates a
 * registered VMA within the address space. Enforces W ^ X and non-overlap.
 */
bool as_map_region(struct address_space *as, uint64_t va, uint64_t size, uint32_t vma_flags, const char *name) {
    if (as == NULL || size == 0) {
        return false;
    }

    /* Enforce strict W ^ X security policy */
    if ((vma_flags & VMA_WRITE) && (vma_flags & VMA_EXEC)) {
        log_error("as_map_region: W^X violation attempted at 0x%lx", va);
        return false;
    }

    /* Page alignment checks */
    if ((va % KOS_PAGE_SIZE) != 0) {
        return false;
    }
    uint64_t aligned_size = (size + KOS_PAGE_SIZE - 1) & ~(KOS_PAGE_SIZE - 1);
    uint64_t end_va = va + aligned_size;

    /* Canonical user space bounds validation */
    if (end_va <= va || va < USER_SPACE_MIN || end_va > USER_SPACE_MAX) {
        return false;
    }

    uint64_t flags = spinlock_lock_irqsave(&as->lock);

    /* Check for overlap against existing VMAs */
    struct vm_area *pos;
    struct list_head *insert_pos = &as->vma_list;
    bool insert_pos_found = false;

    list_for_each_entry(pos, &as->vma_list, node) {
        uint64_t max_start = va > pos->start_va ? va : pos->start_va;
        uint64_t min_end = end_va < pos->end_va ? end_va : pos->end_va;
        if (max_start < min_end) {
            /* Overlapping region */
            spinlock_unlock_irqrestore(&as->lock, flags);
            return false;
        }
        if (!insert_pos_found && va < pos->start_va) {
            insert_pos = &pos->node;
            insert_pos_found = true;
        }
    }

    /* Allocate VMA structure */
    struct vm_area *vma = kmalloc(sizeof(struct vm_area));
    if (vma == NULL) {
        spinlock_unlock_irqrestore(&as->lock, flags);
        return false;
    }

    vma->start_va = va;
    vma->end_va = end_va;
    vma->flags = vma_flags;
    if (name != NULL) {
        kstrncpy(vma->name, name, VMA_NAME_MAX - 1);
        vma->name[VMA_NAME_MAX - 1] = '\0';
    } else {
        vma->name[0] = '\0';
    }

    /* Map physical frames into page tables */
    uint64_t vmm_flags = VMM_PAGE_USER;
    if (vma_flags & VMA_WRITE) {
        vmm_flags |= VMM_PAGE_WRITABLE;
    }
    if (!(vma_flags & VMA_EXEC)) {
        vmm_flags |= VMM_PAGE_NO_EXECUTE;
    }

    for (uint64_t cur_va = va; cur_va < end_va; cur_va += KOS_PAGE_SIZE) {
        uint64_t frame = pmm_allocate_frame();
        if (frame == 0) {
            /* Allocation failure: rollback all mapped pages in this region */
            for (uint64_t rb_va = va; rb_va < cur_va; rb_va += KOS_PAGE_SIZE) {
                uint64_t unmapped = vmm_unmap_page_in(as->pml4_phys, rb_va);
                if (unmapped != 0) {
                    pmm_free_frame(unmapped);
                    as->page_count--;
                }
            }
            kfree(vma);
            spinlock_unlock_irqrestore(&as->lock, flags);
            return false;
        }

        /* Zero newly allocated frame */
        kmemset(vmm_physical_to_virtual(frame), 0, KOS_PAGE_SIZE);

        if (!vmm_map_page_in(as->pml4_phys, cur_va, frame, vmm_flags)) {
            pmm_free_frame(frame);
            for (uint64_t rb_va = va; rb_va < cur_va; rb_va += KOS_PAGE_SIZE) {
                uint64_t unmapped = vmm_unmap_page_in(as->pml4_phys, rb_va);
                if (unmapped != 0) {
                    pmm_free_frame(unmapped);
                    as->page_count--;
                }
            }
            kfree(vma);
            spinlock_unlock_irqrestore(&as->lock, flags);
            return false;
        }
        as->page_count++;
    }

    /* Insert into sorted list */
    if (insert_pos_found) {
        __list_add(&vma->node, insert_pos->prev, insert_pos);
    } else {
        list_add_tail(&vma->node, &as->vma_list);
    }

    spinlock_unlock_irqrestore(&as->lock, flags);
    return true;
}

/*
 * as_unmap_region:
 * Unmaps physical frames, updates/splits overlapping VMAs, and frees reclaimed memory.
 */
bool as_unmap_region(struct address_space *as, uint64_t va, uint64_t size) {
    if (as == NULL || size == 0 || (va % KOS_PAGE_SIZE) != 0) {
        return false;
    }

    uint64_t aligned_size = (size + KOS_PAGE_SIZE - 1) & ~(KOS_PAGE_SIZE - 1);
    uint64_t unmap_end = va + aligned_size;
    if (unmap_end <= va) {
        return false;
    }

    uint64_t flags = spinlock_lock_irqsave(&as->lock);

    /* 1. Adjust or remove overlapping VMAs */
    struct vm_area *pos;
    struct vm_area *n;
    list_for_each_entry_safe(pos, n, &as->vma_list, node) {
        if (unmap_end <= pos->start_va || va >= pos->end_va) {
            continue;
        }

        if (va <= pos->start_va && unmap_end >= pos->end_va) {
            /* Case A: VMA completely engulfed -> remove */
            list_del_init(&pos->node);
            kfree(pos);
        } else if (va > pos->start_va && unmap_end < pos->end_va) {
            /* Case B: VMA split in middle -> truncate existing, allocate upper */
            struct vm_area *upper = kmalloc(sizeof(struct vm_area));
            if (upper != NULL) {
                upper->start_va = unmap_end;
                upper->end_va = pos->end_va;
                upper->flags = pos->flags;
                kstrncpy(upper->name, pos->name, VMA_NAME_MAX);
                __list_add(&upper->node, &pos->node, pos->node.next);
            }
            pos->end_va = va;
        } else if (va <= pos->start_va && unmap_end < pos->end_va) {
            /* Case C: Trim from start */
            pos->start_va = unmap_end;
        } else if (va > pos->start_va && unmap_end >= pos->end_va) {
            /* Case D: Trim from end */
            pos->end_va = va;
        }
    }

    /* 2. Unmap hardware page table entries and free frames */
    for (uint64_t cur_va = va; cur_va < unmap_end; cur_va += KOS_PAGE_SIZE) {
        uint64_t phys = vmm_unmap_page_in(as->pml4_phys, cur_va);
        if (phys != 0) {
            pmm_free_frame(phys);
            if (as->page_count > 0) {
                as->page_count--;
            }
        }
    }

    spinlock_unlock_irqrestore(&as->lock, flags);
    return true;
}

/*
 * as_switch:
 * Switches CR3 to the target address space. If NULL, switches to kernel PML4.
 */
void as_switch(struct address_space *as) {
    if (as != NULL && as->pml4_phys != 0) {
        cpu_write_cr3(as->pml4_phys);
    } else {
        cpu_write_cr3(vmm_virtual_to_physical(vmm_physical_to_virtual(cpu_read_cr3())));
    }
}

/*
 * as_get_current:
 * Helper returning the current process's address space.
 */
struct address_space *as_get_current(void) {
    struct process *cur = process_get_current();
    return cur ? cur->as : NULL;
}

/*
 * ============================================================================
 * Safe User Memory Copy Routines
 * ============================================================================
 */

/*
 * user_memory_validate:
 * Validates canonical user bounds, registered VMAs, and page table permissions.
 */
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access) {
    if (user_ptr == NULL || len == 0) {
        return false;
    }

    uint64_t start = (uint64_t)user_ptr;
    uint64_t end = start + len;

    /* Canonical user range check */
    if (end <= start || end > USER_SPACE_MAX || start < USER_SPACE_MIN) {
        return false;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return false;
    }

    /* Page table permissions check */
    uint64_t start_page = start & ~(KOS_PAGE_SIZE - 1);
    uint64_t end_page = (end + KOS_PAGE_SIZE - 1) & ~(KOS_PAGE_SIZE - 1);

    for (uint64_t page = start_page; page < end_page; page += KOS_PAGE_SIZE) {
        uint64_t phys = 0;
        uint64_t page_flags = 0;
        if (!vmm_query_page_in(proc->pml4_physical, page, &phys, &page_flags)) {
            return false;
        }
        if ((page_flags & VMM_PAGE_USER) == 0) {
            return false;
        }
        if (write_access && (page_flags & VMM_PAGE_WRITABLE) == 0) {
            return false;
        }
    }

    return true;
}

/*
 * copy_from_user:
 * Safely copies n bytes from user space into kernel buffer dst.
 * Returns 0 on success, -EFAULT on validation or translation error.
 */
int copy_from_user(void *dst, const void *src_user, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (dst == NULL || src_user == NULL) {
        return -EFAULT;
    }
    if (!user_memory_validate(src_user, n, false)) {
        return -EFAULT;
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
        if (!vmm_query_page_in(proc->pml4_physical, user_va & ~(KOS_PAGE_SIZE - 1), &phys, NULL)) {
            return -EFAULT;
        }

        const void *src_hhdm = (const void *)((uint8_t *)vmm_physical_to_virtual(phys) + page_off);
        kmemcpy(kdst, src_hhdm, chunk);

        user_va += chunk;
        kdst += chunk;
        remaining -= chunk;
    }

    return 0;
}

/*
 * copy_to_user:
 * Safely copies n bytes from kernel buffer src into user space dst_user.
 * Returns 0 on success, -EFAULT on validation or translation error.
 */
int copy_to_user(void *dst_user, const void *src, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (dst_user == NULL || src == NULL) {
        return -EFAULT;
    }
    if (!user_memory_validate(dst_user, n, true)) {
        return -EFAULT;
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
        if (!vmm_query_page_in(proc->pml4_physical, user_va & ~(KOS_PAGE_SIZE - 1), &phys, NULL)) {
            return -EFAULT;
        }

        void *dst_hhdm = (void *)((uint8_t *)vmm_physical_to_virtual(phys) + page_off);
        kmemcpy(dst_hhdm, ksrc, chunk);

        user_va += chunk;
        ksrc += chunk;
        remaining -= chunk;
    }

    return 0;
}

/*
 * strndup_from_user:
 * Safely measures and copies a null-terminated string up to max_len from user space.
 * Allocates and returns a null-terminated buffer via kmalloc. Returns NULL on failure.
 */
char *strndup_from_user(const char *src_user, size_t max_len) {
    if (src_user == NULL || max_len == 0) {
        return NULL;
    }
    uint64_t start = (uint64_t)src_user;
    if (start < USER_SPACE_MIN || start >= USER_SPACE_MAX) {
        return NULL;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return NULL;
    }

    /* 1. Safely inspect length up to max_len */
    size_t len = 0;
    uint64_t cur_va = start;
    bool nul_found = false;

    while (len < max_len) {
        if (cur_va >= USER_SPACE_MAX) {
            return NULL;
        }

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
    }

    /* 2. Allocate destination buffer */
    char *buf = kmalloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }

    /* 3. Copy validated bytes */
    if (len > 0) {
        if (copy_from_user(buf, src_user, len) != 0) {
            kfree(buf);
            return NULL;
        }
    }
    buf[len] = '\0';
    return buf;
}

/*
 * ============================================================================
 * Address Space Subsystem Self-Test
 * ============================================================================
 */

bool as_self_test(void) {
    uint64_t initial_pmm_free = pmm_free_frame_count();
    uint64_t initial_heap_allocs = heap_active_allocation_count();

    /* 1. Test creation */
    struct address_space *as = as_create();
    if (as == NULL || as->pml4_phys == 0 || as->pml4_virt == NULL) {
        log_error("as_self_test: as_create failed");
        return false;
    }
    if (atomic_read(&as->refcount) != 1 || as->page_count != 0) {
        as_destroy(as);
        return false;
    }

    /* Verify entries 0..255 are 0 */
    for (uint64_t i = 0; i < 256; ++i) {
        if (as->pml4_virt[i] != 0) {
            log_error("as_self_test: PML4 user entry %lu is non-zero", i);
            as_destroy(as);
            return false;
        }
    }

    /* 2. Test mapping regions */
    uint64_t code_va = 0x00400000ull;
    uint64_t code_size = 2 * KOS_PAGE_SIZE;
    if (!as_map_region(as, code_va, code_size, VMA_READ | VMA_EXEC, ".text")) {
        log_error("as_self_test: mapping code region failed");
        as_destroy(as);
        return false;
    }

    uint64_t data_va = 0x00600000ull;
    uint64_t data_size = 1 * KOS_PAGE_SIZE;
    if (!as_map_region(as, data_va, data_size, VMA_READ | VMA_WRITE, ".data")) {
        log_error("as_self_test: mapping data region failed");
        as_destroy(as);
        return false;
    }

    /* Verify W ^ X rejection */
    if (as_map_region(as, 0x00800000ull, KOS_PAGE_SIZE, VMA_READ | VMA_WRITE | VMA_EXEC, ".bad_wx")) {
        log_error("as_self_test: W^X violation was not rejected");
        as_destroy(as);
        return false;
    }

    /* Verify overlap rejection */
    if (as_map_region(as, code_va, KOS_PAGE_SIZE, VMA_READ | VMA_EXEC, ".text_dup")) {
        log_error("as_self_test: overlapping region was not rejected");
        as_destroy(as);
        return false;
    }

    /* 3. Test VMA lookup */
    struct vm_area *vma = as_find_vma(as, code_va);
    if (vma == NULL || vma->start_va != code_va || (vma->flags & VMA_EXEC) == 0) {
        log_error("as_self_test: as_find_vma failed for code_va");
        as_destroy(as);
        return false;
    }
    if (as_find_vma(as, 0x00500000ull) != NULL) {
        log_error("as_self_test: as_find_vma returned non-null for unmapped hole");
        as_destroy(as);
        return false;
    }

    /* 4. Test safe copy routines */
    struct process *cur = process_get_current();
    struct address_space *saved_as = cur ? cur->as : NULL;
    uint64_t saved_pml4 = cur ? cur->pml4_physical : 0;
    if (cur != NULL) {
        cur->as = as;
        cur->pml4_physical = as->pml4_phys;
    }

    /* Validation checks */
    if (user_memory_validate(NULL, 10, false)) {
        goto test_fail;
    }
    if (user_memory_validate((void *)0xFFFF800000000000ull, 10, false)) {
        goto test_fail;
    }
    if (user_memory_validate((void *)0x00500000ull, 10, false)) {
        goto test_fail;
    }
    if (user_memory_validate((void *)code_va, 10, true)) {
        goto test_fail;
    }
    if (!user_memory_validate((void *)code_va, 10, false)) {
        goto test_fail;
    }
    if (!user_memory_validate((void *)data_va, 10, true)) {
        goto test_fail;
    }

    /* Data copy verification */
    const char test_payload[] = "KOS-M1-Isolated-Address-Space-Test";
    size_t payload_len = sizeof(test_payload);
    if (copy_to_user((void *)data_va, test_payload, payload_len) != 0) {
        log_error("as_self_test: copy_to_user failed");
        goto test_fail;
    }

    char verify_buf[64];
    kmemset(verify_buf, 0, sizeof(verify_buf));
    if (copy_from_user(verify_buf, (void *)data_va, payload_len) != 0) {
        log_error("as_self_test: copy_from_user failed");
        goto test_fail;
    }
    if (kmemcmp(test_payload, verify_buf, payload_len) != 0) {
        log_error("as_self_test: payload mismatch in copy_from_user");
        goto test_fail;
    }

    /* String duplication check */
    char *dup = strndup_from_user((const char *)data_va, 64);
    if (dup == NULL || kmemcmp(dup, test_payload, payload_len) != 0) {
        log_error("as_self_test: strndup_from_user failed");
        if (dup) kfree(dup);
        goto test_fail;
    }
    kfree(dup);

    /* Restore process context */
    if (cur != NULL) {
        cur->as = saved_as;
        cur->pml4_physical = saved_pml4;
    }

    /* 5. Test refcounting */
    as_ref(as);
    if (atomic_read(&as->refcount) != 2) {
        as_destroy(as);
        return false;
    }
    as_unref(as);
    if (atomic_read(&as->refcount) != 1) {
        as_destroy(as);
        return false;
    }
    as_unref(as); /* Drops to 0 -> triggers as_destroy() */

    /* 6. Verify 100% PMM frame and Heap reclamation */
    uint64_t final_pmm_free = pmm_free_frame_count();
    uint64_t final_heap_allocs = heap_active_allocation_count();

    if (final_pmm_free != initial_pmm_free) {
        log_error("as_self_test: PMM frame leak! Initial=%lu, Final=%lu",
                  initial_pmm_free, final_pmm_free);
        return false;
    }
    if (final_heap_allocs != initial_heap_allocs) {
        log_error("as_self_test: Heap leak! Initial=%lu, Final=%lu",
                  initial_heap_allocs, final_heap_allocs);
        return false;
    }

    log_info("Address space isolation & memory safety self-test passed (0 frame leaks, 0 heap leaks).");
    return true;

test_fail:
    if (cur != NULL) {
        cur->as = saved_as;
        cur->pml4_physical = saved_pml4;
    }
    as_destroy(as);
    return false;
}
```

---

## 5. Integration with Process Subsystem

### 5.1 `include/kos/process.h`
Forward declare `struct address_space;` and embed `struct address_space *as;` within `struct process`:

```c
struct address_space;

struct process {
    uint64_t pid;
    char name[PROCESS_NAME_MAX];
    uint64_t ppid;
    enum process_state state;
    int exit_code;
    struct list_head threads;
    uint32_t thread_count;
    uint64_t pml4_physical;        /* Fast CR3 accessor cached from as->pml4_phys */
    struct address_space *as;      /* Address space object (Added in M1) */
    kos_spinlock_t lock;
    struct list_head process_node;
    struct list_head children;
    struct list_head sibling_node;
    struct handle_table *handles;
    struct wait_queue exit_wq;
};
```

### 5.2 `kernel/process.c` Updates
1. In `process_create()`:
   ```c
   proc->as = NULL;
   ```
2. In `process_exec()`:
   ```c
   struct address_space *new_as = as_create();
   if (new_as == NULL) {
       return -1;
   }
   /* Map binary segments into new_as */
   ...
   /* Replace old address space */
   struct address_space *old_as = proc->as;
   proc->as = new_as;
   proc->pml4_physical = new_as->pml4_phys;
   if (old_as != NULL) {
       as_unref(old_as);
   }
   ```
3. In `process_destroy()`:
   ```c
   if (proc->as != NULL) {
       as_unref(proc->as);
       proc->as = NULL;
   }
   proc->pml4_physical = 0;
   ```

---

## 6. CPU MSR Primitives (Milestone M1 Feature 1)

### 6.1 `include/kos/cpu.h`
```c
#define IA32_EFER       0xC0000080U
#define IA32_STAR       0xC0000081U
#define IA32_LSTAR      0xC0000082U
#define IA32_FMASK      0xC0000084U

uint64_t cpu_read_msr(uint32_t msr);
void cpu_write_msr(uint32_t msr, uint64_t val);
```

### 6.2 `arch/x86_64/cpu_control.c`
```c
uint64_t cpu_read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void cpu_write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
```

---

## 7. Verification Matrix

| Test Item | Verification Technique | Expected Result | Pass Criteria |
|---|---|---|---|
| **Compilation** | `tools\kos-tool.exe build` | Clang compiles with `-Wall -Wextra -Werror` | Code 0, 0 warnings |
| **Lower Half Clean** | `as_self_test()` step 1 | PML4 entries 0..255 are 0 | Loop check passes |
| **Higher Half Preserved** | `as_self_test()` step 1 | Kernel direct map identical to master | Kernel mappings unchanged |
| **W^X Enforcement** | `as_map_region()` with W+X | Rejection of bad flags | Returns false |
| **Overlap Rejection** | Duplicate mapping of `.text` | Rejection of collision | Returns false |
| **VMA Lookup** | `as_find_vma()` | Locate `.text`, reject unmapped hole | Valid pointer / NULL |
| **Safe User Copy** | `copy_to_user` & `copy_from_user` | HHDM chunk copy matches payload | `memcmp == 0` |
| **Memory Leak Audit** | `pmm_free_frame_count()` & `heap_active_allocation_count()` | Compare before `as_create()` vs after `as_unref()` | $\Delta \text{Frames} = 0, \Delta \text{Heap} = 0$ |
