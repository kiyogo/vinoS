# Forensic Audit Report — Milestone M1 (Hardware & Paging Primitives)

**Work Product**: Milestone M1 changes (`include/kos/vmm.h`, `mm/vmm.c`, `include/kos/cpu.h`, `arch/x86_64/cpu_control.c`, `include/kos/descriptors.h`, `kernel/task.c`, `net/socket.c`)  
**Profile**: General Project  
**Integrity Mode**: Development (from `ORIGINAL_REQUEST.md`)  
**Verdict**: **CLEAN**

---

### Phase Results
- **Check 1: Hardcoded Output Detection**: **PASS** — No test strings, PASS/FAIL constants, or synthetic verification outputs found in modified files.
- **Check 2: Facade & Stub Detection**: **PASS** — All functions implement authentic x86_64 machine instructions and data operations without trivial returns or mock stubs.
- **Check 3: Inline Assembly Authenticity (`cpu_write_cr3`)**: **PASS** — Genuine `movq %rdi, %cr3` instruction verified via object file disassembly with `memory` clobber.
- **Check 4: Paging Flag Correctness (`VMM_PAGE_USER`)**: **PASS** — `VMM_PAGE_USER` correctly maps to bit 2 (`0x4`); `next_table()` propagates `PAGE_USER` to all intermediate paging levels (PML4E, PDPTE, PDE).
- **Check 5: Hardware TSS RSP0 Context Switch**: **PASS** — `schedule()` computes 16-byte aligned thread stack top and updates `tss.rsp0` before executing `task_switch()`, with fallback to `kos_boot_stack_top`.
- **Check 6: Wait Queue State Initialization (`net_udp_open`)**: **PASS** — `wait_queue_init()` called directly after struct allocation, properly initializing spinlock and list head pointers.
- **Check 7: Build Authenticity**: **PASS** — `tools\kos-tool.exe build` runs Clang C17 with `-Wall -Wextra -Werror` and LLD with `--fatal-warnings`, producing `build/kos.elf` and staging UEFI ESP without warnings or errors.

---

## 1. Observation

### File-by-File Static & Binary Inspections

1. **`include/kos/vmm.h` (lines 8-13)**:
   ```c
   enum {
       VMM_PAGE_WRITABLE = 1ull << 1,
       VMM_PAGE_USER = 1ull << 2,
       VMM_PAGE_NO_EXECUTE = 1ull << 63,
   };

   #define VMM_PAGE_USER (1ull << 2)
   ```
   - Direct observation: Defines bit 2 (`0x4`). In x86_64 architecture, Bit 2 of paging entries is the User/Supervisor (U/S) flag. Setting bit 2 grants CPL 3 user-mode access.

2. **`mm/vmm.c`**:
   - `PAGE_USER = 1ull << 2` defined in line 13.
   - `combine_effective_flags()` (lines 77-79):
     ```c
     if ((parent_flags & VMM_PAGE_USER) == 0) {
         result &= ~VMM_PAGE_USER;
     }
     ```
     Correctly enforces hardware paging hierarchy where supervisor level at any parent clears effective user permissions.
   - `next_table()` (lines 86-114):
     ```c
     static uint64_t *next_table(uint64_t *entry, bool *created, uint64_t virtual_address, uint64_t flags) {
         *created = false;
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

         uint64_t frame = pmm_allocate_frame();
         if (frame == 0) {
             return 0;
         }
         uint64_t *table = physical_to_virtual(frame);
         clear_page_table(table);

         uint64_t entry_flags = frame | PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE;
         if (is_user) {
             entry_flags |= PAGE_USER;
         }
         *entry = entry_flags;
         *created = true;
         return table;
     }
     ```
     Intermediate tables (PML4, PDPT, PD) created or updated for virtual addresses `< 0x0000800000000000ull` (canonical user half) or with `VMM_PAGE_USER` receive bit 2 (`PAGE_USER`).
   - Disassembly of `build/obj/mm_vmm.o` (`vmm_map_page`):
     ```assembly
     324: 41 0f b6 f5          movzbl %r13b, %esi
     328: c1 e6 02             shll   $0x2, %esi       ; shifts is_user to bit 2
     32b: 48 09 c6             orq    %rax, %rsi
     32e: 48 81 ce 03 02 00 00 orq    $0x203, %rsi     ; PAGE_PRESENT | VMM_PAGE_WRITABLE | PAGE_KOS_OWNED_TABLE
     335: 48 89 37             movq   %rsi, (%rdi)     ; *entry = entry_flags
     ...
     360: 45 84 ed             testb  %r13b, %r13b
     363: 74 07                je     0x36c
     365: 48 83 c8 04          orq    $0x4, %rax       ; *entry |= PAGE_USER (0x4)
     369: 48 89 02             movq   %rax, (%rdx)
     ```
     The machine code directly sets `0x4` into the table entries.

3. **`include/kos/cpu.h` & `arch/x86_64/cpu_control.c`**:
   - `void cpu_write_cr3(uint64_t cr3);` declared in `include/kos/cpu.h:31`.
   - `cpu_write_cr3()` implemented in `arch/x86_64/cpu_control.c:75-77`:
     ```c
     void cpu_write_cr3(uint64_t cr3) {
         __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
     }
     ```
   - Object file disassembly (`llvm-objdump -d --disassemble-symbols=cpu_write_cr3 build/obj/arch_x86_64_cpu_control.o`):
     ```assembly
     0000000000000000 <cpu_write_cr3>:
            0: 55                           pushq   %rbp
            1: 48 89 e5                     movq    %rsp, %rbp
            4: 0f 22 df                     movq    %rdi, %cr3
            7: 5d                           popq    %rbp
            8: c3                           retq
     ```
     Instruction `0f 22 df` is genuine `movq %rdi, %cr3`.

4. **`include/kos/descriptors.h`**:
   - Lines 41-44 define canonical segment selector macros:
     ```c
     #define GDT_KERNEL_CS 0x08
     #define GDT_KERNEL_DS 0x10
     #define GDT_USER_CS   0x2b
     #define GDT_USER_DS   0x33
     ```
   - Matches GDT descriptors configured in `arch/x86_64/descriptors.c:17-25` (`0x28 | 3 = 0x2b` for User Code, `0x30 | 3 = 0x33` for User Data).

5. **`kernel/task.c`**:
   - In `schedule()` (lines 301-306):
     ```c
     if (next->stack != NULL) {
         tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
     } else {
         extern uint8_t kos_boot_stack_top[];
         tss_set_rsp0((uint64_t)kos_boot_stack_top);
     }
     ```
   - Disassembly of `build/obj/kernel_task.o` (`schedule`):
     ```assembly
     123: 49 8b 7f 58          movq 0x58(%r15), %rdi    ; next->stack
     127: 48 85 ff             testq %rdi, %rdi
     12a: 74 0a                je   0x136
     12c: 49 03 7f 60          addq 0x60(%r15), %rdi    ; + next->stack_size
     130: 48 83 e7 f0          andq $-0x10, %rdi        ; & ~0xF (16-byte alignment)
     134: eb 07                jmp  0x13d
     136: 48 c7 c7 00 00 00 00 movq $0x0, %rdi          ; kos_boot_stack_top
     13d: e8 00 00 00 00       callq 0x142              ; tss_set_rsp0
     142: 4c 89 f7             movq %r14, %rdi
     145: 4c 89 fe             movq %r15, %rsi
     148: e8 00 00 00 00       callq 0x14d              ; task_switch
     ```

6. **`net/socket.c`**:
   - In `net_udp_open()` (lines 186-187):
     ```c
     sockets[index] = (struct udp_socket){ .open = true };
     wait_queue_init(&sockets[index].wait_queue);
     ```
   - Disassembly of `build/obj/net_socket.o` (`net_udp_open`):
     ```assembly
     12a: 49 81 c7 68 17 00 00 addq  $0x1768, %r15     ; offset of wait_queue in socket
     131: 4c 89 ff             movq  %r15, %rdi
     134: e8 00 00 00 00       callq 0x139             ; wait_queue_init
     139: 66 45 89 2e          movw  %r13w, (%r14)     ; *socket_id = index
     ```

7. **Build execution via `tools\kos-tool.exe build`**:
   - Command executed: `.\tools\kos-tool.exe build`
   - Exit code: 0
   - Output log:
     ```
     Kernel ELF built with 651 embedded symbols: build/kos.elf
     UEFI ESP staged: build/esp
     ```
   - Verified that Clang compiled all C files under `-Wall -Wextra -Werror` and LLD linked with `--fatal-warnings` without warnings or suppression flags.

---

## 2. Logic Chain

1. **Integrity Mode Conformance**:
   - `ORIGINAL_REQUEST.md` specifies `Integrity mode: development`. Under development mode, the auditor checks for absence of hardcoded test outputs, dummy stubs, and fabricated logs.
   - None of the 7 modified files contain hardcoded test constants, mocked returns, or empty facades.

2. **Architectural Correctness & Genuine Logic**:
   - **Paging User Bit**: AMD64 translation in user mode (CPL 3) fails with page fault if bit 2 (U/S) is 0 in any translation entry (PML4, PDPT, PD, PT). In `mm/vmm.c`, `next_table()` ensures intermediate tables allocated or traversed for user virtual addresses `< 0x0000800000000000ull` have bit 2 set (`entry |= PAGE_USER`). Disassembly confirms bit 2 (`0x4`) is emitted in machine code.
   - **CR3 Control**: Context switching to per-process address spaces requires reloading CR3. `cpu_write_cr3()` executes `movq %rdi, %cr3` with compiler `memory` clobber, which correctly invalidates non-global TLB entries and enforces memory access ordering across the address space boundary.
   - **TSS RSP0 Switching**: When interrupts or exceptions occur while the CPU is executing in user mode (CPL 3), the CPU switches stack pointer to `TSS.rsp0`. In `kernel/task.c`, `schedule()` updates `tss_set_rsp0()` to `(next->stack + next->stack_size) & ~0xF` before switching context. This guarantees that user-mode interrupts land on the thread's dedicated kernel stack.
   - **Wait Queue Fix**: `net_udp_open()` zeroed the socket struct, corrupting the circular doubly linked list pointers (`head.next` and `head.prev`). Calling `wait_queue_init()` re-establishes the list head pointing to itself and resets the waiter spinlock.

3. **Linker Garbage Collection Analysis**:
   - `cpu_write_cr3()` exists in `build/obj/arch_x86_64_cpu_control.o`. Because Milestone M1 implements the primitive ahead of Milestone M2 (`vmm_create_address_space`) and Milestone M4 (`process_exec`), `--gc-sections` in LLD elides uncalled functions from the final ELF binary until caller sites are integrated. The primitive itself is genuine and compiles without error.

---

## 3. Caveats

- Milestone M1 provides the hardware and paging primitives. Full end-to-end user mode execution (`enter_userspace`), per-process address spaces (`vmm_create_address_space`), and ELF loading (`elf_load_binary`) are scheduled for Milestones M2, M3, and M4 per `PROJECT.md`.
- No other files outside the M1 contract were modified.

---

## 4. Conclusion

The work products delivered for Milestone M1 (`include/kos/vmm.h`, `mm/vmm.c`, `include/kos/cpu.h`, `arch/x86_64/cpu_control.c`, `include/kos/descriptors.h`, `kernel/task.c`, `net/socket.c`) are authentic, complete, robust, and free of integrity violations.

**Verdict**: **CLEAN**

---

## 5. Verification Method

To independently reproduce and verify this audit:

1. **Build Verification**:
   ```pwsh
   .\tools\kos-tool.exe build
   ```
   *Expected outcome*: Exit code 0, 0 compiler warnings, 0 linker warnings.

2. **CR3 Assembly Primitive Verification**:
   ```pwsh
   & "C:\Program Files\LLVM\bin\llvm-objdump.exe" -d --disassemble-symbols=cpu_write_cr3 build/obj/arch_x86_64_cpu_control.o
   ```
   *Expected opcode*: `0f 22 df  movq %rdi, %cr3`.

3. **TSS RSP0 Context Switch Verification**:
   ```pwsh
   & "C:\Program Files\LLVM\bin\llvm-objdump.exe" -d --disassemble-symbols=schedule build/obj/kernel_task.o
   ```
   *Expected sequence*: Stack top calculation with `andq $-0x10, %rdi` followed by `callq <tss_set_rsp0>`.

4. **Page Table User Bit Verification**:
   ```pwsh
   & "C:\Program Files\LLVM\bin\llvm-objdump.exe" -d --disassemble-symbols=vmm_map_page build/obj/mm_vmm.o
   ```
   *Expected sequence*: Bit manipulation with `shll $0x2` and `orq $0x4, %rax`.

5. **Invalidation Condition**:
   Any hardcoded output strings or failure to compile under `-Werror` invalidates this report.
