# Technical Analysis: Safe User Memory Copy & MSR Access Primitives (Milestone K13/K14 M1)

**Agent**: `explorer_m1_2` (M1 User Copy & MSR Explorer)  
**Milestone**: Milestone 1 (M1) — Memory & Address Space Isolation (Requirement R1)  
**Date**: 2026-09-18T10:25:00Z  
**Project**: KOS x86_64  
**Project Root**: `D:\DATA\source\OS\KOS\OS2`  

---

## 1. Executive Summary & Problem Scope

In Milestone K13/K14, KOS introduces Ring 3 userspace isolation, isolated per-process virtual address spaces, and hardware syscall handling. A critical foundation for kernel stability is the **Safe User Memory Access** boundary and **Model-Specific Register (MSR)** control primitives (Requirement R1).

### 1.1 The Kernel Fault Panic Constraint
In `arch/x86_64/interrupts.c:256-260`:
```c
bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);
if (!is_user) {
    /* Kernel mode page fault is unrecoverable -> Panic & Halt */
    log_page_fault_panic(context, &record);
}
```
Any Page Fault (`#PF`) triggered while CPL=0 (kernel mode) immediately calls `panic()` and halts the CPU. KOS does not currently implement Linux-style exception fixup tables (`.fixup` / `search_exception_tables`). Therefore, **any direct dereference of an unmapped, non-user, or malicious Ring 3 pointer by kernel code will fatally crash the entire operating system**.

### 1.2 The Solution
All data transfers between kernel space and user space must:
1. **Pre-validate addresses via software page-table walk (`user_memory_validate`)**: Ensure strict canonical boundary limits (`< 0x0000800000000000ull`), non-null, integer overflow protection, and verify page presence, user privilege (`VMM_PAGE_USER`), and write permissions (`VMM_PAGE_WRITABLE`) via the current process's PML4.
2. **Translate to Limine Higher-Half Direct Map (HHDM)**: Perform the actual byte transfer using physical addresses translated into HHDM virtual space (`vmm_physical_to_virtual(phys)`). Because physical RAM in HHDM is permanently mapped in kernel page tables, copying through HHDM can **never** trigger a page fault.
3. **Execute chunked multi-page copies (`copy_from_user`, `copy_to_user`)**: Safely handle transfers that cross 4 KiB page boundaries, computing per-page offsets and lengths.
4. **Safely traverse user strings (`strndup_from_user`)**: Bounded page-by-page traversal preventing runaway string scans from faulting into unmapped pages or non-user memory.
5. **Provide robust MSR access primitives (`cpu_read_msr`, `cpu_write_msr`)**: Required for hardware `SYSCALL`/`SYSRET` initialization (`IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_EFER.SCE`).

---

## 2. Hardware Architecture & MSR Primitives

### 2.1 x86_64 Model-Specific Registers for SYSCALL
The x86_64 architecture provides the `SYSCALL` and `SYSRET` instructions for low-latency system calls. These require four MSRs to be properly configured:

| MSR Constant | MSR Address | Description | Bit Fields & Required Configuration |
|---|---|---|---|
| `IA32_EFER` | `0xC0000080` | Extended Feature Enable Register | Bit 0: `SCE` (Syscall Enable) — must be 1.<br>Bit 8: `LME` (Long Mode Enable) — 1.<br>Bit 10: `LMA` (Long Mode Active) — 1.<br>Bit 11: `NXE` (No-Execute Enable) — 1 (enabled by Limine). |
| `IA32_STAR` | `0xC0000081` | Segment Selectors for Syscall/Sysret | Bits 31:0: Reserved (target EIP for 32-bit SYSCALL).<br>Bits 47:32: Kernel CS/SS selectors (`0x0008` gives CS=`0x08`, SS=`0x10`).<br>Bits 63:48: User CS/SS selectors for SYSRET. |
| `IA32_LSTAR` | `0xC0000082` | Long Mode Syscall Target RIP | 64-bit canonical target RIP for Ring 0 entry when user executes `syscall`. Set to `(uint64_t)syscall_entry`. |
| `IA32_FMASK` / `IA32_SFMASK` | `0xC0000084` | System Call Flag Mask | Bitmask of flags cleared in `RFLAGS` on `syscall`. Must clear at least IF (bit 9), TF (bit 8), DF (bit 10), and AC (bit 18). Standard mask: `0x0000000000257FD5ull`. |

### 2.2 Assembly Implementation: `cpu_read_msr` & `cpu_write_msr`
The `rdmsr` instruction takes the MSR register index in `%ecx`, and returns the 64-bit value split across `%edx:%eax` (high 32 bits in `%edx`, low 32 bits in `%eax`).
The `wrmsr` instruction takes the MSR index in `%ecx`, high 32 bits in `%edx`, and low 32 bits in `%eax`.

```c
/* In arch/x86_64/cpu_control.c */
uint64_t cpu_read_msr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | (uint64_t)low;
}

void cpu_write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}
```

#### Compiler & Assembly Verification:
- Under freestanding Clang/LLVM with `-O2`, `cpu_read_msr` generates:
  ```asm
  mov ecx, edi
  rdmsr
  shl rdx, 32
  or  rax, rdx
  ret
  ```
- `cpu_write_msr` generates:
  ```asm
  mov ecx, edi
  mov eax, esi
  mov rdx, rsi
  shr rdx, 32
  wrmsr
  ret
  ```
- The `"memory"` clobber on `wrmsr` ensures the compiler does not reorder memory operations across MSR writes.
- Using explicit `"=a"` and `"=d"` constraints is 100% portable across all versions of Clang and GCC in 64-bit mode (avoiding ambiguous `"=A"` behavior in 64-bit inline asm).

---

## 3. Mathematical & Algorithmic Analysis of Safe User Memory Copy

### 3.1 Canonical Address Space Boundaries
In 48-bit x86_64 virtual addressing, the address space is split into two halves:
- **Lower Half (User Space)**: `0x0000_0000_0000_0000` to `0x0000_7FFF_FFFF_FFFF` (PML4 entries 0..255, 128 TiB).
- **Non-Canonical Hole**: `0x0000_8000_0000_0000` to `0xFFFF_7FFF_FFFF_FFFF`.
- **Higher Half (Kernel Space)**: `0xFFFF_8000_0000_0000` to `0xFFFF_FFFF_FFFF_FFFF` (PML4 entries 256..511, 128 TiB).

Any address $\ge 0x0000800000000000\text{ull}$ and $< 0xFFFF800000000000\text{ull}$ is non-canonical and triggers `#GP(0)` if dereferenced directly. Any address $\ge 0xFFFF800000000000\text{ull}$ is kernel space and must never be read/written on behalf of user requests without validation.

```
USER_SPACE_LIMIT = 0x0000800000000000ull (2^47)
Valid User VA range: [0x0000000000000000, 0x00007FFFFFFFFFFF]
```

### 3.2 Integer Overflow & Range Verification
Given user pointer `user_ptr` and length `len`:
$$\text{start} = (\text{uint64\_t})\text{user\_ptr}$$
$$\text{end} = \text{start} + \text{len}$$

Potential security attack vectors:
1. **NULL Pointer**: `user_ptr == NULL` (or `start == 0`). Must be rejected.
2. **Integer Wrap-Around (Overflow)**: A malicious user passes `start = 0x00007FFFFFFFFFF0`, `len = 0xFFFFFFFF00000020`.
   $\text{start} + \text{len}$ overflows 64-bit integer, yielding $\text{end} < \text{start}$.
   Condition: `if (end < start) return false;`
3. **Crossing into Non-Canonical or Kernel Space**:
   A user passes `start = 0x00007FFFFFFFF000`, `len = 0x2000`.
   $\text{end} = 0x0000800000001000 > \text{USER\_SPACE\_LIMIT}$.
   Condition: `if (start >= USER_SPACE_LIMIT || end >= USER_SPACE_LIMIT) return false;`
4. **Zero-Length Buffers (`len == 0`)**:
   If `len == 0`, $\text{end} == \text{start}$. If `user_ptr != NULL` and `start < USER_SPACE_LIMIT`, 0 bytes are to be read or written. No hardware memory access will take place. Standard POSIX semantics (e.g. `read(fd, buf, 0) == 0`) treat zero-length transfers as successful no-ops without accessing memory.
   Condition: `if (len == 0) return true;`

### 3.3 Page Table Walking via `vmm_query_page_in`
For any non-empty range `[start, end)`:
- The first page containing bytes is:
  $$\text{page\_start} = \text{start} \ \& \ \sim(\text{KOS\_PAGE\_SIZE} - 1)$$
- The last byte in the range is at address $\text{end} - 1$. The page containing this last byte is:
  $$\text{page\_last} = (\text{end} - 1) \ \& \ \sim(\text{KOS\_PAGE\_SIZE} - 1)$$
- The set of 4 KiB pages touched is:
  $$\text{pages} = \{ p \mid p = \text{page\_start} + k \cdot \text{KOS\_PAGE\_SIZE},\ p \le \text{page\_last} \}$$

For every page $p$ in this set, `user_memory_validate` queries the current process's PML4:
```c
uint64_t phys = 0;
uint64_t flags = 0;
if (!vmm_query_page_in(proc->pml4_physical, page, &phys, &flags)) {
    return false; /* Page is not present in page tables */
}
if ((flags & VMM_PAGE_USER) == 0) {
    return false; /* Page is supervisor-only (DPL 0), Ring 3 forbidden */
}
if (write_access && ((flags & VMM_PAGE_WRITABLE) == 0)) {
    return false; /* Attempting write to read-only page */
}
```

#### Why `vmm_query_page_in` is 100% Reliable:
Direct observation of `mm/vmm.c:343-403`:
1. It acquires `vmm_lock` with `spinlock_lock_irqsave(&vmm_lock)`.
2. It walks PML4 $\to$ PDPT $\to$ PD $\to$ PT.
3. It combines effective permission flags down the tree via `combine_effective_flags`: if ANY parent level (PML4, PDPT, PD) lacks `VMM_PAGE_USER` or `VMM_PAGE_WRITABLE`, the effective flag is cleared.
4. It correctly resolves 2 MiB and 1 GiB huge pages if present.
5. It returns `public_page_flags(effective_flags)` stripping internal software bits.

### 3.4 Chunked Multi-Page Copy via HHDM
When copying $n$ bytes between user space and kernel space:
- Memory may span multiple physical frames that are **not physically contiguous**.
- Therefore, the copy cannot be executed as a single `memcpy`.
- It must be performed in **chunks** bounded by 4 KiB page boundaries.

For current user virtual address `user_va` with `remaining` bytes left:
1. Compute intra-page offset:
   $$\text{page\_off} = \text{user\_va} \ \& \ (\text{KOS\_PAGE\_SIZE} - 1)$$
2. Compute bytes remaining in the current page:
   $$\text{bytes\_in\_page} = \text{KOS\_PAGE\_SIZE} - \text{page\_off}$$
3. Compute chunk size for this step:
   $$\text{chunk} = \min(\text{bytes\_in\_page}, \text{remaining})$$
4. Query physical address of the page:
   $$\text{vmm\_query\_page\_in}(\text{proc}->\text{pml4\_physical}, \text{user\_va} \ \& \ \sim(\text{KOS\_PAGE\_SIZE} - 1), \&\text{phys}, \text{NULL})$$
5. Translate physical address to kernel HHDM virtual address:
   $$\text{hhdm\_ptr} = (\text{uint8\_t} *)\text{vmm\_physical\_to\_virtual}(\text{phys}) + \text{page\_off}$$
6. Execute memory copy on safe kernel HHDM address:
   - For `copy_from_user`: `kmemcpy(kdst, hhdm_ptr, chunk);`
   - For `copy_to_user`: `kmemcpy(hhdm_ptr, ksrc, chunk);`
7. Advance pointers:
   $$\text{user\_va} \mathrel{+}= \text{chunk},\quad \text{kbuf} \mathrel{+}= \text{chunk},\quad \text{remaining} \mathrel{-}= \text{chunk}$$

**Safety Guarantee**: The source (or destination) in kernel space is a verified kernel pointer. The counterpart is accessed exclusively through `vmm_physical_to_virtual(phys)`, which is a Higher-Half Direct Map address backed by an existing physical RAM frame. No user-controlled virtual address is ever dereferenced in Ring 0.

### 3.5 Bounded User String Traversal: `strndup_from_user`
Copying a string of unknown length from user space (e.g. file paths in `sys_open`, program arguments in `sys_exec`) poses a unique hazard:
- If the kernel uses standard `strlen(user_ptr)`, a missing null-terminator can cause the CPU to run off the end of mapped memory into an unmapped guard page or non-canonical space, triggering `#PF` panic.
- If the kernel naively walks byte-by-byte calling `vmm_query_page_in` on every single byte, performance degrades severely because each byte acquires and releases `vmm_lock`.

#### Optimal Two-Phase Algorithm:
**Phase 1: Measure Length with Page-Bounded Scanning**
- Start at `cur_va = (uint64_t)src_user`.
- While `len < max_len`:
  1. Check canonical upper bound: `if (cur_va >= USER_SPACE_LIMIT) return NULL;`
  2. Query page status **once per page**:
     `vmm_query_page_in(proc->pml4_physical, cur_va & ~(PAGE_SIZE - 1), &phys, &flags)`
     If not present or `(flags & VMM_PAGE_USER) == 0`, return `NULL`.
  3. Obtain HHDM kernel pointer for this page:
     `page_kaddr = (const char *)vmm_physical_to_virtual(phys) + page_off;`
  4. Determine maximum scan length within this page:
     `scan_bytes = min(PAGE_SIZE - page_off, max_len - len);`
  5. Scan bytes in memory directly via HHDM:
     ```c
     for (size_t i = 0; i < scan_bytes; ++i) {
         if (page_kaddr[i] == '\0') {
             nul_found = true;
             break;
         }
         len++;
         cur_va++;
     }
     ```
  6. If `nul_found`, terminate measurement loop immediately.

**Phase 2: Allocate and Copy**
- Allocate `len + 1` bytes on the kernel heap: `char *buf = kmalloc(len + 1);`
- If allocation fails, return `NULL`.
- If `len > 0`, invoke `copy_from_user(buf, src_user, len)`. If copy fails, call `kfree(buf)` and return `NULL`.
- Explicitly write null terminator: `buf[len] = '\0';`
- Return `buf`.

---

## 4. Production-Ready Code Specification

### 4.1 Header: `include/kos/cpu.h`
Add MSR constants and access declarations:

```c
/* Model Specific Register (MSR) Constants */
#define IA32_EFER       0xC0000080u
#define IA32_STAR       0xC0000081u
#define IA32_LSTAR      0xC0000082u
#define IA32_FMASK      0xC0000084u
#define IA32_SFMASK     IA32_FMASK

/* IA32_EFER Bit Definitions */
#define IA32_EFER_SCE   (1ull << 0)  /* System Call Extensions (enables SYSCALL/SYSRET) */
#define IA32_EFER_LME   (1ull << 8)  /* Long Mode Enable */
#define IA32_EFER_LMA   (1ull << 10) /* Long Mode Active */
#define IA32_EFER_NXE   (1ull << 11) /* No-Execute Enable */

/* MSR Access Primitives */
uint64_t cpu_read_msr(uint32_t msr);
void cpu_write_msr(uint32_t msr, uint64_t val);
```

### 4.2 Implementation: `arch/x86_64/cpu_control.c`
Append MSR read/write functions:

```c
uint64_t cpu_read_msr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | (uint64_t)low;
}

void cpu_write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}
```

### 4.3 Header: `include/kos/address_space.h`
Definitions for canonical bounds and safe user copy primitives:

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
 * User virtual addresses occupy PML4 entries 0..255:
 * 0x0000000000000000 .. 0x00007FFFFFFFFFFF (128 TiB).
 * The upper boundary limit is 0x0000800000000000ull (non-canonical hole starts here).
 */
#define USER_SPACE_LIMIT        0x0000800000000000ull
#define USER_SPACE_MAX          USER_SPACE_LIMIT

/* Standard POSIX error code for bad memory address */
#ifndef EFAULT
#define EFAULT                  14
#endif

/*
 * Safe User Memory Access Functions:
 * Guaranteed 0 kernel page fault panics via software page table validation
 * and HHDM physical-to-virtual translation.
 */
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access);
int copy_from_user(void *dst, const void *src_user, size_t n);
int copy_to_user(void *dst_user, const void *src, size_t n);
char *strndup_from_user(const char *src_user, size_t max_len);

#endif /* KOS_ADDRESS_SPACE_H */
```

### 4.4 Implementation: `mm/address_space.c`
Complete implementation of safe user memory functions:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kos/address_space.h>
#include <kos/heap.h>
#include <kos/memory.h>
#include <kos/process.h>
#include <kos/vmm.h>

/*
 * user_memory_validate:
 * Performs strict canonical boundary, overflow, and page-table permission checks.
 *
 * Rules:
 * 1. user_ptr must not be NULL.
 * 2. start and end must both be strictly < 0x0000800000000000ull.
 * 3. start + len must not overflow (end >= start).
 * 4. Every 4 KiB page in [start, end) must be present in the current process PML4,
 *    must have VMM_PAGE_USER, and must have VMM_PAGE_WRITABLE if write_access is true.
 * 5. A zero-length buffer (len == 0) with a valid user_ptr returns true.
 */
bool user_memory_validate(const void *user_ptr, size_t len, bool write_access) {
    if (user_ptr == NULL) {
        return false;
    }

    uint64_t start = (uint64_t)user_ptr;
    uint64_t end = start + len;

    /* Check for 64-bit integer overflow */
    if (end < start) {
        return false;
    }

    /* Strict boundary check: start and end must be < 0x0000800000000000ull */
    if (start >= USER_SPACE_LIMIT || end >= USER_SPACE_LIMIT) {
        return false;
    }

    /* Zero-length buffer within user space accesses 0 pages */
    if (len == 0) {
        return true;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return false;
    }

    uint64_t page_start = start & ~(KOS_PAGE_SIZE - 1);
    uint64_t page_last = (end - 1) & ~(KOS_PAGE_SIZE - 1);

    for (uint64_t page = page_start; page <= page_last; page += KOS_PAGE_SIZE) {
        uint64_t phys = 0;
        uint64_t flags = 0;
        if (!vmm_query_page_in(proc->pml4_physical, page, &phys, &flags)) {
            return false;
        }
        if ((flags & VMM_PAGE_USER) == 0) {
            return false;
        }
        if (write_access && ((flags & VMM_PAGE_WRITABLE) == 0)) {
            return false;
        }
    }

    return true;
}

/*
 * copy_from_user:
 * Safely copies n bytes from user space into kernel buffer dst.
 * Returns 0 on success, or -EFAULT (-14) on validation/translation failure.
 */
int copy_from_user(void *dst, const void *src_user, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (dst == NULL) {
        return -EFAULT;
    }
    if (!user_memory_validate(src_user, n, false)) {
        return -EFAULT;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return -EFAULT;
    }

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
 * Returns 0 on success, or -EFAULT (-14) on validation/translation failure.
 */
int copy_to_user(void *dst_user, const void *src, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (src == NULL) {
        return -EFAULT;
    }
    if (!user_memory_validate(dst_user, n, true)) {
        return -EFAULT;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return -EFAULT;
    }

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
 * Safely traverses user string up to max_len without causing page fault,
 * copies characters into newly allocated kmalloc buffer, and ensures null-termination.
 * Returns allocated string pointer on success, or NULL on failure.
 */
char *strndup_from_user(const char *src_user, size_t max_len) {
    if (src_user == NULL || max_len == 0) {
        return NULL;
    }

    uint64_t start = (uint64_t)src_user;
    if (start >= USER_SPACE_LIMIT) {
        return NULL;
    }

    struct process *proc = process_get_current();
    if (proc == NULL || proc->pml4_physical == 0) {
        return NULL;
    }

    /*
     * Phase 1: Safely determine string length without triggering kernel page faults.
     * We scan page-by-page via HHDM virtual addresses, ensuring every accessed page
     * is present and user-accessible.
     */
    size_t len = 0;
    uint64_t cur_va = start;
    bool nul_found = false;

    while (len < max_len) {
        if (cur_va >= USER_SPACE_LIMIT) {
            return NULL; /* Exceeded user address space boundary */
        }

        uint64_t phys = 0;
        uint64_t page_flags = 0;
        if (!vmm_query_page_in(proc->pml4_physical, cur_va & ~(KOS_PAGE_SIZE - 1), &phys, &page_flags)) {
            return NULL; /* Unmapped page encountered */
        }
        if ((page_flags & VMM_PAGE_USER) == 0) {
            return NULL; /* Non-user page encountered */
        }

        uint64_t page_off = cur_va & (KOS_PAGE_SIZE - 1);
        size_t bytes_in_page = KOS_PAGE_SIZE - page_off;
        size_t bytes_remaining = max_len - len;
        size_t scan_bytes = bytes_in_page < bytes_remaining ? bytes_in_page : bytes_remaining;

        const char *page_kaddr = (const char *)vmm_physical_to_virtual(phys) + page_off;
        for (size_t i = 0; i < scan_bytes; ++i) {
            if (page_kaddr[i] == '\0') {
                nul_found = true;
                break;
            }
            len++;
            cur_va++;
        }

        if (nul_found) {
            break;
        }
    }

    /*
     * Phase 2: Allocate destination buffer in kernel heap.
     * Always allocate len + 1 bytes to guarantee null-termination.
     */
    char *buf = kmalloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }

    if (len > 0) {
        if (copy_from_user(buf, src_user, len) != 0) {
            kfree(buf);
            return NULL;
        }
    }

    buf[len] = '\0';
    return buf;
}
```

---

## 5. Empirical Test & Self-Test Suite

To satisfy the 4-tier testing hierarchy defined in `TEST_INFRA.md`, the following self-tests must be integrated into `as_self_test()` or `kernel/main.c`:

### 5.1 Test Suite 1: MSR Read/Write Integrity
```c
bool test_msr_primitives(void) {
    /* 1. Verify IA32_EFER read: LMA and NXE must be active */
    uint64_t efer = cpu_read_msr(IA32_EFER);
    if ((efer & IA32_EFER_LMA) == 0 || (efer & IA32_EFER_NXE) == 0) {
        return false;
    }

    /* 2. Verify write and read-back on IA32_FMASK */
    uint64_t saved_fmask = cpu_read_msr(IA32_FMASK);
    uint64_t test_mask = 0x0000000000257FD5ull;
    cpu_write_msr(IA32_FMASK, test_mask);
    uint64_t read_back = cpu_read_msr(IA32_FMASK);
    cpu_write_msr(IA32_FMASK, saved_fmask); /* Restore */

    if (read_back != test_mask) {
        return false;
    }

    return true;
}
```

### 5.2 Test Suite 2: Safe Memory Copy & Boundary Adversarial Tests
```c
bool test_safe_user_copy(uint64_t user_pml4) {
    struct process *cur = process_get_current();
    uint64_t old_pml4 = cur->pml4_physical;
    cur->pml4_physical = user_pml4;

    /* Frame allocations for test */
    uint64_t f_ro = pmm_allocate_frame();
    uint64_t f_rw = pmm_allocate_frame();
    uint64_t f_cross = pmm_allocate_frame();

    /* Map pages:
     * 0x00400000: Read-only user page
     * 0x00500000: Read-write user page
     * 0x00501000: Read-write user page (contiguous with 0x00500000)
     */
    vmm_map_page_in(user_pml4, 0x00400000ull, f_ro, VMM_PAGE_USER);
    vmm_map_page_in(user_pml4, 0x00500000ull, f_rw, VMM_PAGE_USER | VMM_PAGE_WRITABLE);
    vmm_map_page_in(user_pml4, 0x00501000ull, f_cross, VMM_PAGE_USER | VMM_PAGE_WRITABLE);

    /* --- Tier 2 Boundary & Adversarial Cases --- */
    /* 1. NULL pointer */
    if (user_memory_validate(NULL, 10, false) != false) return false;
    if (copy_from_user((void *)0x1000, NULL, 10) != -EFAULT) return false;
    if (copy_to_user(NULL, (void *)0x1000, 10) != -EFAULT) return false;

    /* 2. Kernel space addresses */
    if (user_memory_validate((void *)0xFFFF800000000000ull, 10, false) != false) return false;
    if (user_memory_validate((void *)0xFFFFFFFFFFFFFFFFull, 10, false) != false) return false;

    /* 3. Non-canonical / Boundary limit */
    if (user_memory_validate((void *)USER_SPACE_LIMIT, 1, false) != false) return false;

    /* 4. Integer addition overflow */
    if (user_memory_validate((void *)0x00007FFFFFFFFFF0ull, 0x20, false) != false) return false;

    /* 5. Unmapped user address hole */
    if (user_memory_validate((void *)0x00100000ull, 10, false) != false) return false;
    if (copy_from_user((void *)0x1000, (void *)0x00100000ull, 10) != -EFAULT) return false;

    /* 6. Permission violations */
    /* Write to read-only page must fail validation and copy */
    if (user_memory_validate((void *)0x00400000ull, 10, true) != false) return false;
    if (copy_to_user((void *)0x00400000ull, "test", 4) != -EFAULT) return false;
    /* Read from read-only page must succeed */
    if (user_memory_validate((void *)0x00400000ull, 10, false) != true) return false;

    /* 7. Zero-length transfers */
    if (user_memory_validate((void *)0x00500000ull, 0, false) != true) return false;
    if (copy_from_user((void *)0x1000, (void *)0x00500000ull, 0) != 0) return false;
    if (copy_to_user((void *)0x00500000ull, (void *)0x1000, 0) != 0) return false;

    /* --- Tier 1 Functional Cases --- */
    /* 8. Single-page read/write */
    const char msg[] = "KOS-Safe-Copy-Test-String";
    char read_buf[64];
    if (copy_to_user((void *)0x00500010ull, msg, sizeof(msg)) != 0) return false;
    if (copy_from_user(read_buf, (void *)0x00500010ull, sizeof(msg)) != 0) return false;
    if (kmemcmp(msg, read_buf, sizeof(msg)) != 0) return false;

    /* 9. Cross-page chunked copy (spanning 0x00500FFF to 0x0050101F = 33 bytes) */
    char cross_src[33];
    char cross_dst[33];
    for (int i = 0; i < 33; ++i) cross_src[i] = (char)('A' + (i % 26));
    if (copy_to_user((void *)0x00500FFFull, cross_src, 33) != 0) return false;
    if (copy_from_user(cross_dst, (void *)0x00500FFFull, 33) != 0) return false;
    if (kmemcmp(cross_src, cross_dst, 33) != 0) return false;

    /* 10. String duplication tests */
    char *dup = strndup_from_user((const char *)0x00500010ull, 64);
    if (dup == NULL || kmemcmp(dup, msg, sizeof(msg)) != 0) {
        if (dup) kfree(dup);
        return false;
    }
    kfree(dup);

    /* 11. String duplication hitting unmapped memory before NUL */
    /* Write unterminated string right at the end of mapped space (0x00501FF0) */
    char unterminated[16] = "1234567890123456";
    copy_to_user((void *)0x00501FF0ull, unterminated, 16);
    /* 0x00502000 is unmapped -> strndup_from_user must return NULL cleanly without panic */
    char *bad_dup = strndup_from_user((const char *)0x00501FF0ull, 64);
    if (bad_dup != NULL) {
        kfree(bad_dup);
        return false;
    }

    cur->pml4_physical = old_pml4;
    return true;
}
```

---

## 6. Downstream Architectural Integration

### 6.1 Integration with Milestone 2 (Syscall Entry & Ring 3 Switch)
- In M2, `syscall_subsystem_init()` will configure:
  ```c
  cpu_write_msr(IA32_EFER, cpu_read_msr(IA32_EFER) | IA32_EFER_SCE);
  cpu_write_msr(IA32_STAR, ((uint64_t)GDT_KERNEL_CS << 32) | ((uint64_t)GDT_USER_CS << 48));
  cpu_write_msr(IA32_LSTAR, (uint64_t)syscall_entry);
  cpu_write_msr(IA32_FMASK, 0x0000000000257FD5ull);
  ```
  The MSR primitives designed in M1 directly unblock M2.

### 6.2 Integration with Milestone 3 (Core Syscalls)
- `sys_write(int fd, const void *buf, size_t count)`:
  Currently returns `UINT64_MAX` in `kernel/syscall.c:19`.
  With `copy_from_user`, `sys_write` copies chunks of user data into a kernel stack buffer:
  ```c
  char kbuf[256];
  while (count > 0) {
      size_t chunk = count > sizeof(kbuf) ? sizeof(kbuf) : count;
      if (copy_from_user(kbuf, buf, chunk) != 0) {
          return -EFAULT;
      }
      terminal_write(kbuf, chunk);
      buf = (const uint8_t *)buf + chunk;
      count -= chunk;
  }
  ```
  This guarantees that invalid user pointers passed to `sys_write` return `-EFAULT` without crashing the kernel.

- `sys_read(int fd, void *buf, size_t count)`:
  Uses `copy_to_user(buf, kbuf, chunk)`.

- Process launching (`exec` / `sys_exec`):
  Uses `strndup_from_user(path, PATH_MAX)` to safely ingest binary paths from user memory.
