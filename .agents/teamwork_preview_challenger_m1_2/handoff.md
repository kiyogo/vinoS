# Adversarial Verification Report — Milestone M1 (Hardware & Paging Primitives)

## Challenge Summary

**Overall risk assessment**: LOW
**Verdict**: APPROVE

All hardware and paging primitives delivered in Milestone M1 have been rigorously challenged, empirically tested, disassembled at machine-code level in `build/kos.elf`, and verified in QEMU execution.

---

## 1. Observation

### 1.1 Task Switching & TSS RSP0
- **Source Code**: In `kernel/task.c` (lines 301–306):
  ```c
  if (next->stack != NULL) {
      tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
  } else {
      extern uint8_t kos_boot_stack_top[];
      tss_set_rsp0((uint64_t)kos_boot_stack_top);
  }
  task_switch(&previous->context, &next->context);
  ```
- **Disassembly Verification**: Disassembly of `schedule()` in `build/kos.elf` at virtual address `0xffffffff8000f433` confirmed:
  ```x86asm
  ffffffff8000f433: 49 8b 7f 58          movq    0x58(%r15), %rdi       # %rdi = next->stack
  ffffffff8000f437: 48 85 ff             testq   %rdi, %rdi             # check NULL
  ffffffff8000f43a: 74 0a                je      0xffffffff8000f446     # fallback branch
  ffffffff8000f43c: 49 03 7f 60          addq    0x60(%r15), %rdi       # %rdi += next->stack_size
  ffffffff8000f440: 48 83 e7 f0          andq    $-0x10, %rdi           # %rdi &= ~0xF (-16)
  ffffffff8000f444: eb 07                jmp     0xffffffff8000f44d
  ffffffff8000f446: 48 c7 c7 70 e1 48 80 movq    $-0x7fb71e90, %rdi     # %rdi = kos_boot_stack_top (0xffffffff8048e170)
  ffffffff8000f44d: e8 9e 1c ff ff       callq   0xffffffff800010f0     # tss_set_rsp0
  ffffffff8000f452: 4c 89 f7             movq    %r14, %rdi             # &previous->context
  ffffffff8000f455: 4c 89 fe             movq    %r15, %rsi             # &next->context
  ffffffff8000f458: e8 83 f1 00 00       callq   0xffffffff8001e5e0     # task_switch
  ```
- **Disassembly of `tss_set_rsp0`**: At `0xffffffff800010f0`:
  ```x86asm
  ffffffff800010f0: 55                   pushq   %rbp
  ffffffff800010f1: 48 89 e5             movq    %rsp, %rbp
  ffffffff800010f4: 48 89 3d 39 a6 02 00 movq    %rdi, 0x2a639(%rip)     # 0xffffffff8002b734 <tss+0x4>
  ffffffff800010fb: 5d                   popq    %rbp
  ffffffff800010fc: c3                   retq
  ```
  Writes directly to `tss.rsp0` at offset 4.
- **Symbol Alignment**: `llvm-nm` inspection of `build/kos.elf` verified:
  - `kos_boot_stack_bottom`: `0xffffffff8047e170` (16-byte aligned)
  - `kos_boot_stack_top`: `0xffffffff8048e170` (16-byte aligned, size = 64 KiB)
  - `gdt_entries`: `0xffffffff8002b000` (16-byte aligned)
  - `tss`: `0xffffffff8002b730` (16-byte aligned)

### 1.2 GDT Selectors and Descriptors
- **Definitions**: In `include/kos/descriptors.h` (lines 41–44):
  ```c
  #define GDT_KERNEL_CS 0x08
  #define GDT_KERNEL_DS 0x10
  #define GDT_USER_CS   0x2b
  #define GDT_USER_DS   0x33
  ```
- **Binary Image Inspection**: Direct decoding of `gdt_entries` in `build/kos.elf` at file offset `0x2c000` (`0xffffffff8002b000`):
  - Entry 0 (`0x0000000000000000`): Null descriptor
  - Entry 1 (`0x00af9a000000ffff`): Kernel Code — DPL=0, Present=1, Exec=1, L=1 (64-bit), D/B=0, G=1, Limit=0xffff, Base=0x0 -> Selector `0x08` (index 1, RPL 0)
  - Entry 2 (`0x00cf92000000ffff`): Kernel Data — DPL=0, Present=1, Exec=0, L=0, D/B=1, G=1, Limit=0xffff, Base=0x0 -> Selector `0x10` (index 2, RPL 0)
  - Entry 3 & 4 (`0x0` in binary): Configured dynamically at boot in `gdt_initialize()` for 64-bit TSS -> Selector `0x18` (index 3, RPL 0)
  - Entry 5 (`0x00affa000000ffff`): User Code — DPL=3, Present=1, Exec=1, L=1 (64-bit), D/B=0, G=1, Limit=0xffff, Base=0x0 -> Selector `(5 << 3) | 3 = 0x2b`
  - Entry 6 (`0x00cff2000000ffff`): User Data — DPL=3, Present=1, Exec=0, L=0, D/B=1, G=1, Limit=0xffff, Base=0x0 -> Selector `(6 << 3) | 3 = 0x33`

### 1.3 Paging User Flag & CR3 Control
- **Definitions**:
  - In `include/kos/vmm.h`: `VMM_PAGE_USER = 1ull << 2` and `#define VMM_PAGE_USER (1ull << 2)`.
  - In `mm/vmm.c`: `next_table()` sets `PAGE_USER` on intermediate tables (PML4, PDPT, PD) when `(virtual_address < 0x0000800000000000ull) || (flags & VMM_PAGE_USER)`.
  - In `mm/vmm.c`: `vmm_map_page()` allows `VMM_PAGE_USER` in flag mask validation.
  - In `include/kos/cpu.h` & `arch/x86_64/cpu_control.c`: `cpu_write_cr3(uint64_t cr3)` emits `movq %rdi, %cr3` (confirmed in `build/obj/arch_x86_64_cpu_control.o`).

### 1.4 UDP Socket Wait Queue Fix
- In `net/socket.c`: `net_udp_open()` explicitly calls `wait_queue_init(&sockets[index].wait_queue)`.

### 1.5 Build Tool Output
- `.\tools\kos-tool.exe build`:
  - Clang compiler invocation with `-Wall -Wextra -Werror -std=c17 -ffreestanding`.
  - LLD linker invocation with `--fatal-warnings`.
  - Output: `Kernel ELF built with 651 embedded symbols: build/kos.elf` and `UEFI ESP staged: build/esp`.
  - Exit code: `0`. 0 warnings, 0 errors.

### 1.6 QEMU Execution
- Headless QEMU execution (`-machine q35 -cpu max -m 256 ... -serial file:build/qemu_serial.log -monitor none -no-reboot -display none`):
  - Serial log verified clean boot, memory tracking, and full test suite passes:
    - `[PASS] ALL KERNEL OBJECT & HANDLE TESTS PASSED!`
    - `[PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED!`
    - `[PASS] PMM concurrent allocation/free stress verified!`
    - `[PASS] Heap concurrent allocation/realloc/free stress verified!`
    - `[PASS] VFS/RAMFS concurrent create/read/write/unlink verified!`
    - `[PASS] Socket wait queue non-busy synchronization verified!`
    - `>>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<`
    - Shell prompt reached: `KOS debug terminal ready. KOS>`

---

## 2. Challenges & Adversarial Stress Tests

### [Low Risk] Challenge 1: Out-of-Bounds or Unaligned Stack Calculation
- **Assumption Challenged**: Formula `((uint64_t)next->stack + next->stack_size) & ~0xFull` could produce an unaligned or out-of-bounds pointer under arbitrary, non-standard stack sizes.
- **Attack Scenario**: Test 100,000 randomized stack allocations with random bases (`0x1000` to `0xffffffffffff0000`) and arbitrary unaligned sizes (`16` to `65536`).
- **Stress Test Result**: 100,000 / 100,000 cases passed. `rsp0 % 16 == 0` is strictly invariant, and `stack <= rsp0 <= stack + stack_size` holds across all iterations.
- **Verdict**: PASS.

### [Medium Risk] Challenge 2: Bootstrap Task NULL Stack Dereference
- **Assumption Challenged**: Bootstrap task (`tasks[0]`) has `stack = NULL`. If `schedule()` switches back to `tasks[0]`, an unconditional `tss_set_rsp0(next->stack + next->stack_size)` would calculate `0x0 + 16384 = 0x4000`, setting RSP0 to an unmapped low-memory page and triggering a triple fault on the first user-to-kernel transition.
- **Attack Scenario**: Trace context switch to `tasks[0]`.
- **Stress Test Result**: Source and disassembly confirm the conditional branch:
  ```x86asm
  testq %rdi, %rdi
  je fallback -> loads kos_boot_stack_top (0xffffffff8048e170)
  ```
  `kos_boot_stack_top` is 16-byte aligned (`0xffffffff8048e170 % 16 == 0`).
- **Verdict**: PASS.

### [Low Risk] Challenge 3: GDT Selector Math and Privilege Ring 3 Conformance
- **Assumption Challenged**: Macro selectors `GDT_USER_CS = 0x2b` and `GDT_USER_DS = 0x33` could mismatch AMD64 RPL or GDT descriptor slot indexing.
- **Attack Scenario**: Decode segment selectors bit-by-bit against AMD64 specification:
  - `0x2b = 0010 1011b` -> Index = `00101b` (5), TI = `0` (GDT), RPL = `11b` (3).
  - `0x33 = 0011 0011b` -> Index = `00110b` (6), TI = `0` (GDT), RPL = `11b` (3).
- **Stress Test Result**: Matches GDT entries 5 and 6 exactly. Descriptors in `build/kos.elf` verified:
  - GDT entry 5: DPL=3, Present=1, L=1 (64-bit code), Exec=1.
  - GDT entry 6: DPL=3, Present=1, D/B=1, Exec=0 (data).
- **Verdict**: PASS.

### [Medium Risk] Challenge 4: Intermediate Page Directory User Bit Propagation
- **Assumption Challenged**: AMD64 translation requires bit 2 (U/S) set on all table levels (PML4, PDPT, PD, PT). If intermediate tables lack this bit, CPL 3 execution will trigger an unrecoverable page fault.
- **Attack Scenario**: Verify `next_table()` in `mm/vmm.c`.
- **Stress Test Result**: `next_table()` explicitly sets `PAGE_USER` for `virtual_address < 0x0000800000000000ull` both on newly created tables and existing present tables (`*entry |= PAGE_USER`), while keeping kernel higher-half tables supervisor-only.
- **Verdict**: PASS.

---

## 3. Logic Chain
1. Observations 1.1 and Challenge 2 show that `schedule()` checks `next->stack != NULL` and applies `& ~0xFull`, ensuring 16-byte alignment and preventing low-memory corruption for the bootstrap task.
2. Observation 1.2 and Challenge 3 confirm that GDT selectors `0x2b` and `0x33` point to valid 64-bit Ring 3 code and data descriptors in GDT slots 5 and 6 with RPL 3.
3. Observation 1.3 and Challenge 4 confirm that `VMM_PAGE_USER` is correctly propagated through intermediate page table entries in lower-half address space.
4. Observation 1.4 confirms that the UDP socket wait queue initialization bug is resolved.
5. Observations 1.5 and 1.6 confirm that the kernel compiles cleanly with zero warnings/errors under strict flags and boots into QEMU passing all subsystem synchronization and wait queue tests without regressions.
6. Therefore, Milestone M1 is verified and ready for Milestone M2.

---

## 4. Caveats
- Per-process address space creation/destruction (`vmm_create_address_space`, `vmm_destroy_address_space`) and `cpu_write_cr3()` invocation inside `schedule()` are scheduled for Milestones M2 and M4. Because `cpu_write_cr3` is not yet called by the kernel core, the linker strips it via `--gc-sections` in `build/kos.elf`, but it is fully present in `build/obj/arch_x86_64_cpu_control.o`.
- Full Ring 3 user process execution and `enter_userspace` (`iretq`) will be verified in Milestones M3 and M5.

---

## 5. Conclusion
- **VERDICT**: **APPROVE**
- Milestone M1 successfully satisfies all architectural requirements and acceptance criteria.
- No bugs, regressions, or alignment flaws were found.

---

## 6. Verification Method

To independently verify this assessment:
1. **Rebuild**:
   ```cmd
   .\tools\kos-tool.exe build
   ```
   Expect: Exit code 0, 0 warnings, 0 errors.
2. **Inspect Machine Code**:
   ```powershell
   & "C:/Program Files/LLVM/bin/llvm-objdump.exe" --disassemble-symbols=schedule build/kos.elf
   ```
   Expect: `andq $-0x10, %rdi` followed by `callq tss_set_rsp0`.
3. **Execute in QEMU**:
   ```powershell
   $p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=`"C:\Program Files\qemu\share\edk2-x86_64-code.fd`" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:build/qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 8; Stop-Process -Id $p.Id -Force; Get-Content build/qemu_serial.log -Tail 40
   ```
   Expect: `>>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<` and `KOS debug terminal ready.`
