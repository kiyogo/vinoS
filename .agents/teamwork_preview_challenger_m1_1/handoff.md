# Handoff Report — Challenger M1-1 (Adversarial Verification)

## 1. Observation

1. **Kernel Compilation Verification**:
   - Command: `tools\kos-tool.exe build`
   - Output:
     ```
     Kernel ELF built with 651 embedded symbols: build/kos.elf
     UEFI ESP staged: build/esp
     ```
   - Exit code: `0` (0 warnings, 0 errors with `-Wall -Wextra -Werror` and `--fatal-warnings`).

2. **QEMU Smoke Test Execution**:
   - Command:
     ```powershell
     $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 40
     ```
   - Result in `build/qemu_serial.log`:
     - Line 41: `[    1.050] [TID 1] [INFO ] UDP loopback socket self-test passed.`
     - Line 146: `[    1.540] [TID 1] [INFO ] >>> [PASS] ALL KERNEL OBJECT & HANDLE TESTS PASSED! <<<`
     - Line 179: `[    1.790] [TID 1] [INFO ] >>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<`
     - Line 194: `[    1.940] [TID 1] [INFO ] >>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<`
     - Lines 196-197:
       ```
       [    1.960] [TID 1] [INFO ] KOS debug terminal ready.
       KOS> 
       ```

3. **Flag Validation in `mm/vmm.c`**:
   - Lines 130-133:
     ```c
     if (virtual_address % PAGE_SIZE != 0 || physical_address % PAGE_SIZE != 0
         || (flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE)) != 0) {
         return false;
     }
     ```
   - Lines 257-259 (`vmm_set_page_permissions_unlocked`):
     ```c
     if (!vmm.initialized || (permissions & ~(VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE)) != 0) {
         return false;
     }
     ```
   - Leaf assignment at line 164:
     ```c
     *entry = (physical_address & PAGE_ADDRESS_MASK) | PAGE_PRESENT | PAGE_KOS_OWNED_MAPPING | flags;
     ```

4. **Intermediate Table Allocation & Flagging in `mm/vmm.c`**:
   - Lines 86-98:
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

5. **CR3 Write and TLB Invalidation in `arch/x86_64/cpu_control.c`**:
   - Lines 75-77:
     ```c
     void cpu_write_cr3(uint64_t cr3) {
         __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
     }
     ```

6. **TSS RSP0 Context Switch Update in `kernel/task.c`**:
   - Lines 301-306:
     ```c
     if (next->stack != NULL) {
         tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
     } else {
         extern uint8_t kos_boot_stack_top[];
         tss_set_rsp0((uint64_t)kos_boot_stack_top);
     }
     ```

7. **UDP Socket Wait Queue Fix in `net/socket.c`**:
   - Lines 186-187:
     ```c
     sockets[index] = (struct udp_socket){ .open = true };
     wait_queue_init(&sockets[index].wait_queue);
     ```

## 2. Logic Chain

1. **Flag Validation Security (Observation 3)**:
   - Valid public mapping flags are defined as `VMM_PAGE_WRITABLE` (`1ull << 1`), `VMM_PAGE_USER` (`1ull << 2`), and `VMM_PAGE_NO_EXECUTE` (`1ull << 63`).
   - The expression `(flags & ~(VMM_PAGE_WRITABLE | VMM_PAGE_USER | VMM_PAGE_NO_EXECUTE)) != 0` evaluates to true whenever any unpermitted flag (including internal flags such as `PAGE_PRESENT` bit 0, `PAGE_HUGE` bit 7, `PAGE_KOS_OWNED_TABLE` bit 9, or arbitrary reserved bits) is supplied by the caller.
   - Any such invalid flag combination causes `vmm_map_page()` or `vmm_set_page_permissions()` to reject the call immediately, returning `false` without modifying page tables.

2. **Intermediate Table User/Supervisor Propagation (Observation 4)**:
   - In AMD64 4-level paging, user-mode (CPL=3) access requires bit 2 (User/Supervisor) to be 1 in PML4E, PDPTE, PDE, and PTE.
   - For user-space virtual addresses (`virtual_address < 0x0000800000000000ull`, lower half), `is_user` evaluates to `true`. Newly created intermediate directory tables (PML4E, PDPTE, PDE) receive `PAGE_USER`. Existing intermediate tables are dynamically updated with `*entry |= PAGE_USER`.
   - For higher-half kernel addresses (`virtual_address >= 0xFFFF800000000000ull`), `is_user` evaluates to `false` unless explicitly instructed via `VMM_PAGE_USER`. Higher-half intermediate tables remain strictly supervisor-only (`PAGE_USER = 0`).
   - Hardware MMU protection ensures that Ring 3 cannot access higher-half kernel pages, while user-space translation succeeds down to the leaf PTE.
   - At the leaf PTE, permissions are governed strictly by the requested `flags` (`*entry = ... | flags`). If `flags` does not specify `VMM_PAGE_USER`, the leaf PTE remains supervisor-only, and any Ring 3 access generates a #PF protection violation as required.

3. **CR3 Write and TLB Flushing (Observation 5)**:
   - Per AMD64 Architecture Programmer's Manual Volume 2 (§5.5.1, §5.5.2) and Intel SDM Vol 3A (§4.10.4.1), executing `mov %reg, %cr3` unconditionally invalidates all non-global TLB entries and all paging-structure caches on the processor core.
   - The `__asm__ volatile` declaration with `: "memory"` clobber acts as a compiler memory barrier, preventing the compiler from scheduling memory accesses across the address-space switch.

4. **TSS RSP0 Kernel Stack Switch (Observation 6)**:
   - On every thread context switch in `schedule()`, `tss_set_rsp0()` updates `tss.rsp0` with `((uint64_t)next->stack + next->stack_size) & ~0xFull`, establishing the 16-byte aligned kernel stack top for `next`.
   - When interrupts (e.g. PIT timer at 100 Hz) or exceptions occur while CPL=3, x86_64 CPU hardware automatically reads `tss.rsp0` and pushes the hardware interrupt frame (`SS`, `RSP`, `RFLAGS`, `CS`, `RIP`) onto the running thread's dedicated kernel stack.
   - For the initial bootstrap task (where `stack == NULL`), fallback to `kos_boot_stack_top` prevents zero-stack dereferences during early initialization.

5. **UDP Socket Wait Queue Regression Fix (Observation 2 & 7)**:
   - Zero-initialization of `struct udp_socket` in `net_udp_open()` previously left `wait_queue.head` uninitialized (`next = 0, prev = 0`).
   - During `net_udp_recv_timeout()`, `wait_event_timeout` traversed uninitialized list pointers, causing page faults / null dereferences during UDP loopback testing.
   - Adding `wait_queue_init(&sockets[index].wait_queue)` properly links the circular list head (`head.next = &head`, `head.prev = &head`) and initializes the spinlock.
   - Empirical execution confirmed that `UDP loopback socket self-test passed.` and all subsequent synchronization tests passed without crashing.

## 3. Caveats

- Milestone M1 provides the hardware and paging primitives (`VMM_PAGE_USER`, `next_table` propagation, `cpu_write_cr3`, `tss_set_rsp0`, `GDT_USER_*`). The per-process address space allocator (`vmm_create_address_space`), ELF64 loader (`elf_load_binary`), and Ring 3 entry (`enter_userspace`) are designated for Milestones M2-M4 per `PROJECT.md`.
- No modifications were made to implementation files during this review.

## 4. Conclusion

- **Verdict**: **APPROVE**
- Milestone M1 implementation strictly meets all architectural and hardware requirements.
- The build succeeds cleanly with 0 warnings and 0 errors under Clang `-Wall -Wextra -Werror` and LLD `--fatal-warnings`.
- The kernel boots past `net_udp_loopback_self_test` and all synchronization / wait queue self-tests, successfully reaching the shell prompt `KOS>`.

## 5. Verification Method

- **Automated Build**:
  ```powershell
  tools\kos-tool.exe build
  ```
  Expected output: `Kernel ELF built with 651 embedded symbols: build/kos.elf` and exit code 0.
- **QEMU Smoke Test**:
  ```powershell
  $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 40
  ```
  Expected markers:
  - `UDP loopback socket self-test passed.`
  - `>>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<`
  - `KOS debug terminal ready.`
  - `KOS> `
- **Invalidation Condition**: Any compilation error/warning or failure to boot to the `KOS>` prompt invalidates this approval.
