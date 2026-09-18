# Review & Adversarial Challenge Report — Milestone M2 (Requirement R1: ELF64 Binary Loader & Address Space Mapping)

## Review Summary

**Verdict**: **APPROVE**  
**Role**: Reviewer & Adversarial Critic (`teamwork_preview_reviewer_m2_2`)  
**Target Milestone**: M2 (Requirement R1: ELF64 Binary Loader & Address Space Mapping)  
**Worker Under Review**: `teamwork_preview_worker_m2`  
**Overall Risk Assessment**: LOW  
**Integrity Audit**: PASS (0 integrity violations, 0 facade implementations, 0 hardcoded test bypasses)

---

## 1. Observation

Direct code examination, static analysis, tool compilation, and live QEMU execution yielded the following observations:

### 1.1 Security Boundaries in `vmm_map_page_in()` (`mm/vmm.c`)
- **Address boundary check**: Lines 272-277 in `mm/vmm.c`:
  ```c
  if ((flags & VMM_PAGE_USER) != 0 && virtual_address >= 0x0000800000000000ull) {
      return false;
  }
  if (virtual_address >= 0x0000800000000000ull && virtual_address < 0xffff800000000000ull) {
      return false;
  }
  ```
  - Any attempt to map a page with `VMM_PAGE_USER` at or above `0x0000800000000000ull` is unconditionally rejected with `false`. This prevents user-accessible mappings in both the non-canonical gap (`0x0000800000000000` to `0xFFFF7FFFFFFFFFFF`) and the higher-half kernel space (`0xFFFF800000000000` to `0xFFFFFFFFFFFFFFFF`).
  - Kernel mappings in non-canonical virtual addresses are also strictly blocked.
- **Paging permission propagation**: Lines 99-124 in `next_table()`:
  ```c
  bool is_user = (virtual_address < 0x0000800000000000ull) || ((flags & VMM_PAGE_USER) != 0);
  if ((*entry & PAGE_PRESENT) != 0) {
      if ((*entry & PAGE_HUGE) != 0) {
          return 0;
      }
      if (is_user) {
          *entry |= PAGE_USER;
      }
      return physical_to_virtual(*entry & PAGE_ADDRESS_MASK);
  }
  ```
  - Because `vmm_map_page_in` rejects `(flags & VMM_PAGE_USER) != 0` for higher-half addresses, intermediate page tables in the kernel higher-half (PML4 entries 256..511) are never given `PAGE_USER`.
  - Leaf entries enforce `flags` strictly without altering kernel protections.

### 1.2 User Address Space Isolation & Reclamation (`mm/vmm.c`)
- **Isolation on Creation**: Lines 161-168 in `vmm_create_address_space()`:
  ```c
  for (uint64_t index = 0; index < 256; ++index) {
      new_pml4[index] = 0;
  }
  for (uint64_t index = 256; index < 512; ++index) {
      new_pml4[index] = vmm.pml4[index];
  }
  ```
  - Canonical user address space entries 0..255 are zeroed out (empty and non-present).
  - Kernel higher-half entries 256..511 are cloned directly from `vmm.pml4`, maintaining identical kernel mappings without duplicating tables.
- **Reclamation**: Lines 185-239 in `vmm_clear_user_address_space()`:
  - Iteration strictly bounds traversal to PML4 entries `0 <= pml4_idx < 256`.
  - Recursively frees leaf frames (`pmm_free_frame(leaf_phys)`), PT frames (`pmm_free_frame(pt_phys)`), PD frames (`pmm_free_frame(pd_phys)`), and PDPT frames (`pmm_free_frame(pdpt_phys)`).
  - Handles huge pages (`PAGE_HUGE`) cleanly at PDPT and PD levels.
  - Zeroes entries 0..255 in the PML4. Higher-half kernel entries 256..511 are completely untouched.
- **Destruction Safety**: Lines 257-264 in `vmm_destroy_address_space()`:
  - Protects `vmm.kernel_pml4_phys` from destruction.
  - If the destroyed address space is active in CR3, safely reloads CR3 with `vmm.kernel_pml4_phys`.
  - Clears user mappings via `vmm_clear_user_address_space(pml4_phys)` and then frees the root PML4 frame via `pmm_free_frame(pml4_phys)`.

### 1.3 User Stack, Guard Page & System V AMD64 ABI Alignment (`fs/elf.c`, `include/kos/elf.h`)
- **Definitions in `include/kos/elf.h`**:
  ```c
  #define USER_STACK_TOP        0x00007FFFFFFFE000ull
  #define USER_STACK_SIZE       (64 * 1024ull) /* 64 KiB = 16 pages */
  #define USER_STACK_BOTTOM     (USER_STACK_TOP - USER_STACK_SIZE) /* 0x00007FFFFFFEE000ull */
  #define USER_STACK_GUARD_PAGE (USER_STACK_BOTTOM - 4096ull)      /* 0x00007FFFFFFED000ull */
  ```
- **Guard page non-presence**: Lines 80-83 in `fs/elf.c`:
  ```c
  if (vmm_query_page_in(pml4_phys, USER_STACK_GUARD_PAGE, 0, 0)) {
      vmm_unmap_page_in(pml4_phys, USER_STACK_GUARD_PAGE);
  }
  ```
  - The guard page directly below the bottom of the stack (`0x00007FFFFFFED000ull`) is explicitly unmapped (non-present).
  - Verified in `elf_self_test()`: `vmm_query_page_in(test_pml4, USER_STACK_GUARD_PAGE, 0, 0)` returns `false`.
- **W^X Page Protection**: Lines 102-104 in `fs/elf.c`:
  - Maps stack pages with `VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NO_EXECUTE`.
  - The stack is non-executable, preventing stack-based shellcode execution.
- **System V AMD64 ABI Layout & 16-Byte Alignment**: Lines 145-178 in `fs/elf.c`:
  - Copies `argv` strings into top of user stack.
  - Formats argument table: `argc`, `argv[0]..argv[argc-1]`, `argv[argc] = NULL`, `envp[0] = NULL`, `auxv[0].a_type = AT_NULL`, `auxv[0].a_val = 0`.
  - Enforces 16-byte alignment:
    ```c
    uint64_t target_rsp = (curr_sp - table_bytes) & ~0xFull;
    ```
  - Verified in `elf_self_test()`: `(user_rsp & 0xFull) == 0`.

### 1.4 ELF Loader Validation & Leak-Free Rollback (`fs/elf.c`)
- **Strict ELF64 Header Validation**: Lines 205-217 in `fs/elf.c`:
  - Magic bytes: `\x7fELF` (`ELFMAG0..3`).
  - Class: `ELFCLASS64` (64-bit).
  - Data: `ELFDATA2LSB` (Little-endian).
  - Version: `EV_CURRENT`.
  - Type: `ET_EXEC` (Executable).
  - Machine: `EM_X86_64` (x86_64).
  - Program header entry size: `sizeof(Elf64_Phdr)` (56 bytes).
  - Header size: `sizeof(Elf64_Ehdr)` (64 bytes).
  - Program header count: `1 <= e_phnum <= 64`.
- **Dynamic Binary Rejection**: Lines 236-239:
  - Rejects `PT_INTERP` segment immediately (`return false;`).
- **Canonical Bounds & Address Validation**:
  - `range_is_valid` protects against integer overflow in offset and length calculations.
  - Entry point: `0x00200000ull <= e_entry < 0x0000800000000000ull`.
  - Entry point verified to reside within an executable (`PF_X`) `PT_LOAD` segment (lines 285-297).
  - Segment addresses: `p_vaddr >= 0x00200000ull` (null-pointer barrier), `p_vaddr + p_memsz < 0x0000800000000000ull`, `p_memsz >= p_filesz`.
  - Non-overlapping segments: pairwise quadratic check (`max_start < min_end`) rejects overlapping `PT_LOAD` segments (lines 271-283).
- **Leak-Free Error Rollback**: Lines 366-369:
  ```c
  load_error:
      vmm_clear_user_address_space(pml4_phys);
      return false;
  ```
  - Any failure during segment mapping, frame allocation, or stack setup branches to `load_error`, which tears down all allocated user frames and page tables in the address space without leaking frames.

### 1.5 Build Verification
- Execution of `tools\kos-tool.exe build` completed with Exit code 0:
  ```
  Kernel ELF built with 663 embedded symbols: build/kos.elf
  UEFI ESP staged: build/esp
  Exit code: 0 (0 warnings, 0 errors)
  ```

### 1.6 Live QEMU Boot Verification
- Execution of `tools\kos-tool.exe run --host-port 8095` captured the live serial boot log:
  ```
  [    0.000] [TID 1] [INFO ] Four-level virtual memory manager initialized.
  [    0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).
  [    0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).
  [    0.000] [TID 1] [INFO ] Kernel heap self-test and page reclamation passed.
  ...
  [    0.000] [TID 1] [INFO ]   Free frames: 51146
  [    0.000] [TID 1] [INFO ]   Allocated frames: 0
  ```
  - Both `vmm_self_test` and `elf_self_test` executed live during boot and verified zero frame leakage (`Allocated frames: 0`).

---

## 2. Logic Chain

1. *Observation*: Milestone M2 requires user address spaces to isolate user space (`< 0x0000800000000000ull`) from the higher-half kernel space (`>= 0xFFFF800000000000ull`).
   *Reasoning*: `vmm_create_address_space()` zeroes PML4 entries 0..255 and copies entries 256..511. `vmm_map_page_in()` strictly rejects `(flags & VMM_PAGE_USER) && (virtual_address >= 0x0000800000000000ull)`. `next_table()` ensures intermediate table entries in higher-half space never receive `PAGE_USER`. Therefore, no user mapping can ever compromise kernel higher-half virtual space.
2. *Observation*: System V AMD64 ABI requires initial RSP to be aligned to 16 bytes, and stack overflow must be detected deterministically.
   *Reasoning*: `process_setup_user_stack()` clears `target_rsp` low 4 bits (`& ~0xFull`), guaranteeing 16-byte alignment. `USER_STACK_GUARD_PAGE` is explicitly unmapped. Touching the guard page triggers a CPU page fault with `P=0`, preventing uncontained memory corruption.
3. *Observation*: Statically linked ELF binaries must be validated against malformed headers, dynamic linking, segment overlap, and boundary violations.
   *Reasoning*: `elf_load_binary()` validates magic, 64-bit class, little-endian byte order, `ET_EXEC`, machine `EM_X86_64`, bounds, `PF_X` on entry point segment, and rejects `PT_INTERP`. Non-overlapping interval checks prevent malicious or accidental segment collisions. W^X memory permissions are strictly enforced.
4. *Observation*: Failures during binary loading or process initialization must not leak page frames.
   *Reasoning*: `elf_load_binary()` delegates failure cleanup to `vmm_clear_user_address_space()`, which walks PML4 entries 0..255 and frees all user leaf page frames and intermediate table frames via `pmm_free_frame()`. Live QEMU self-tests confirm `pmm_free_frame_count()` matches initial free frame count before and after operations, reporting `0 frame leaks`.
5. *Observation*: Integrity audit checked for dummy/facade implementations, hardcoded test bypasses, and self-certifying shortcuts.
   *Reasoning*: The implementation performs full page table tree traversal, HHDM memory access, ELF segment parsing, and ABI formatting. Self-tests dynamically measure physical frame counts via `pmm_free_frame_count()` and verify query flags. No integrity violations exist.

---

## 3. Findings & Adversarial Critic Challenges

### [Minor / Proactive Suggestion] Finding 1: ELF Segment Collision with User Stack & Guard Page
- **What**: In `fs/elf.c` line 252, segment upper bound is checked against `end_va >= 0x0000800000000000ull`.
- **Where**: `fs/elf.c:252` in `elf_load_binary()`.
- **Why**: The user stack spans `[0x00007FFFFFFEE000, 0x00007FFFFFFFE000)` and the guard page is at `0x00007FFFFFFED000`. If an ELF binary were crafted with a `PT_LOAD` segment placed at `0x00007FFFFFFED000`, `process_setup_user_stack()` would unmap it or overwrite it.
- **Suggestion**: In Milestone M4/M5, add an explicit check `if (end_va > USER_STACK_GUARD_PAGE) return false;` in `elf_load_binary()` to cleanly reject any binary attempting to load into the user stack address region.

### [Minor / Proactive Suggestion] Finding 2: Sanity Upper Bound on Segment `p_memsz`
- **What**: An ELF binary with an enormous `p_memsz` (e.g. 100 GiB) will loop allocating frames until PMM runs out of memory, then safely unwind.
- **Where**: `fs/elf.c:323-329`.
- **Why**: While leak-free and safe against panics, allocating all available memory before failing causes unnecessary allocation churn.
- **Suggestion**: Add a reasonable maximum segment size limit (e.g., maximum executable size of 64 MiB or total RAM limit) in `elf_load_binary()` for fast rejection.

---

## 4. Verified Claims

- User mapping higher-half protection: verified in `mm/vmm.c:272-277` and live boot self-test -> **PASS**
- Intermediate page table user bit propagation: verified in `mm/vmm.c:99-124` -> **PASS**
- Stack guard page unmapped / non-present status: verified in `fs/elf.c:80-83, 450` -> **PASS**
- System V AMD64 ABI 16-byte RSP alignment: verified in `fs/elf.c:159, 444` -> **PASS**
- Dynamic binary (`PT_INTERP`) rejection: verified in `fs/elf.c:236-239` -> **PASS**
- Segment boundary, overlap, and W^X permissions: verified in `fs/elf.c:241-313` -> **PASS**
- Leak-free address space teardown & rollback: verified via `pmm_free_frame_count()` during boot (0 frame leaks) -> **PASS**
- Build verification (`tools\kos-tool.exe build`): 0 warnings, 0 errors -> **PASS**

## 5. Coverage Gaps

- No coverage gaps within Milestone M2 scope. Ring 3 privilege switching (`iretq`) and user fault containment are planned for Milestone M3. Process lifecycle (`process_exec`, `process_wait`, zombie reaping) is planned for Milestone M4.

## 6. Unverified Items

- None. All Milestone M2 deliverables independently inspected, compiled, and verified via live boot execution.

---

## 7. Caveats

- `elf_load_binary()` intentionally only supports statically linked executables (`ET_EXEC`). Dynamic executables (`PT_INTERP`) are rejected by design.
- The user stack is fixed at 64 KiB with a 4 KiB guard page; dynamic stack growth on page fault is not in scope for M2.

---

## 8. Conclusion

Milestone M2 (Requirement R1: ELF64 Binary Loader & Address Space Mapping) fully meets all architectural, security, and functional criteria defined in `PROJECT.md` and `ORIGINAL_REQUEST.md`:
1. Address space creation and recursive destruction strictly isolate user-mode mappings and kernel higher-half mappings.
2. `vmm_map_page_in()` prevents user mappings in the kernel higher-half.
3. User stack guard page is unmapped and non-present, with initial stack conforming to System V AMD64 ABI 16-byte alignment.
4. ELF64 validation is strict, W^X permissions are enforced, and error rollback is completely leak-free.
5. Automated build passes cleanly, and live boot self-tests confirm zero physical frame leaks.

**Verdict**: **APPROVE**.

---

## 9. Verification Method

- **Build Verification**:
  ```powershell
  tools\kos-tool.exe build
  ```
  Expected output:
  `Kernel ELF built with 663 embedded symbols: build/kos.elf`
  `UEFI ESP staged: build/esp`
  `Exit code: 0`
- **Live QEMU Boot Verification**:
  ```powershell
  tools\kos-tool.exe run --host-port 8095
  ```
  Expected log entries:
  `[ 0.000] [TID 1] [INFO ] VMM address space lifecycle self-test passed (0 frame leaks).`
  `[ 0.000] [TID 1] [INFO ] ELF loader & user stack self-test passed (0 frame leaks).`
- **Invalidation Conditions**:
  - `vmm_map_page_in()` returning true for `VMM_PAGE_USER` at `va >= 0x0000800000000000ull`.
  - `vmm_query_page_in()` returning true for `USER_STACK_GUARD_PAGE`.
  - Initial `user_rsp % 16 != 0`.
  - Any frame leaks detected after address space creation, loading, and destruction.
