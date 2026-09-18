# Handoff Report: Explorer Survey 1 — Hardware Context, GDT, TSS, Ring 3 Transitions & Interrupt Stack Management (Milestone K14)

**Agent ID**: `teamwork_preview_explorer_survey_1`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_1`  
**Target Milestone**: K14 (Requirement R2: Ring 3 Privilege Transition & Hardware Context Management)  
**Parent Agent**: `1ae50773-9b0e-4950-a60c-718073a789e9` (`orchestrator_k14_1`)  
**Date**: 2026-09-18  

---

## 1. Observation

1. **GDT & Descriptors**:
   - Location: `arch/x86_64/descriptors.c:17-25`:
     ```c
     static uint64_t gdt_entries[7] KOS_ALIGNED(16) = {
         0x0000000000000000ull, /* 0: Null Descriptor */
         0x00af9a000000ffffull, /* 1: Kernel Code (0x08) */
         0x00cf92000000ffffull, /* 2: Kernel Data (0x10) */
         0x0000000000000000ull, /* 3: TSS Low (0x18) */
         0x0000000000000000ull, /* 4: TSS High (0x20) */
         0x00affa000000ffffull, /* 5: User Code (0x28 | 3 = 0x2B) */
         0x00cff2000000ffffull, /* 6: User Data (0x30 | 3 = 0x33) */
     };
     ```
   - Selector constants in `include/kos/cpu.h:8-14`:
     ```c
     enum {
         KOS_KERNEL_CODE_SELECTOR = 0x08,
         KOS_KERNEL_DATA_SELECTOR = 0x10,
         KOS_KERNEL_TSS_SELECTOR = 0x18,
         KOS_USER_CODE_SELECTOR = 0x2b,
         KOS_USER_DATA_SELECTOR = 0x33,
     };
     ```
   - Bit-level verification:
     - Entry 5 (`0x00affa000000ffffull`): P=1, DPL=3, S=1, Type=0xA (Code, Exec/Read), L=1 (64-bit), D/B=0, G=1. Selector: `(5 << 3) | 3 = 0x2b`.
     - Entry 6 (`0x00cff2000000ffffull`): P=1, DPL=3, S=1, Type=0x2 (Data, Read/Write), L=0, D/B=1, G=1. Selector: `(6 << 3) | 3 = 0x33`.

2. **TSS Setup & Context Switching**:
   - Location: `include/kos/descriptors.h:14-30`: `struct task_state_segment` is 104 bytes, packed.
   - Initialized in `arch/x86_64/descriptors.c:80-92`:
     ```c
     tss_set_rsp0((uint64_t)kos_boot_stack_top);
     tss.ist1 = (uint64_t)(double_fault_stack + DOUBLE_FAULT_STACK_SIZE);
     tss.io_map_base = sizeof(tss);
     gdt_configure_tss_descriptor();
     ...
     tss_load(KOS_KERNEL_TSS_SELECTOR);
     ```
   - Loaded in `arch/x86_64/cpu.asm:30-33`: `ltr ax` (with selector `0x18`).
   - Context switch in `kernel/task.c:261-303` (`schedule()`):
     Calls `task_switch(&previous->context, &next->context);`.
     `tss_set_rsp0()` is **never** called in `schedule()` or `task_switch`.

3. **Ring 3 Entry Mechanism**:
   - Grep search for `enter_user` or any Ring 3 jumping code returned 0 matches in the entire repository.
   - All tasks currently run in Ring 0 starting from `task_start_trampoline`.

4. **Interrupt Context & Stack Switching**:
   - Location: `include/kos/interrupts.h:11-40`: `struct interrupt_context` is 176 bytes (22 quadwords).
   - In `arch/x86_64/interrupt_stubs.asm:35-76`:
     `isr_common` pushes `rax, rbx, rcx, rdx, rbp, rsi, rdi, r8..r15` (120 bytes), error code & vector (16 bytes), while CPU pushed SS, RSP, RFLAGS, CS, RIP (40 bytes). Total = 176 bytes.
     Aligns stack: `mov r12, rsp; and rsp, -16; mov rdi, r12; call interrupt_dispatch; mov rsp, r12`.
     Returns via `pop r15..rax; add rsp, 16; iretq`.
   - Syscall vector `0x80` in `arch/x86_64/interrupts.c:461`: `idt_entries[KOS_SYSCALL_VECTOR].type_attributes = 0xee;` (DPL=3).
   - In `arch/x86_64/interrupts.c:258-268`, page faults from user mode execute:
     `log_error("User space process termination not yet available in K12 -> System halted."); cpu_halt();`
   - In `arch/x86_64/interrupts.c:393`, unhandled exceptions execute `default_exception_panic(context);` (kernel panic and halt).

5. **Build Toolchain**:
   - Command: `.\tools\kos-tool.exe build` succeeds with code 0:
     `Kernel ELF built with 651 embedded symbols: build/kos.elf`
     `UEFI ESP staged: build/esp`

---

## 2. Logic Chain

1. **GDT Selector Soundness (Observation 1)**:
   - GDT entries 5 and 6 define valid 64-bit Ring 3 Code and Data segment descriptors.
   - The selector constants `KOS_USER_CODE_SELECTOR` (`0x2b`) and `KOS_USER_DATA_SELECTOR` (`0x33`) correctly specify index 5 and 6 with RPL=3.
   - Although `sysret` would require reversed ordering (Data before Code), `iretq` pops CS and SS directly from the stack and does not depend on relative descriptor order. Because K14 specifies `iretq`, the existing GDT layout is 100% usable without descriptor reshuffling.

2. **TSS RSP0 Isolation Defect (Observation 2)**:
   - In x86_64, when an interrupt or exception occurs while CPL=3, the CPU automatically changes RSP to the value stored in `TSS.rsp0`.
   - Because `tss_set_rsp0()` is only called once at boot with `kos_boot_stack_top`, any interrupt occurring while executing a user process will write the interrupt frame to `kos_boot_stack_top`.
   - If multiple user threads execute or if a thread is preempted during an interrupt, subsequent interrupts from Ring 3 will overwrite the previous thread's kernel stack frame, leading to stack corruption.
   - Therefore, `schedule()` must call `tss_set_rsp0()` with the kernel stack top of the newly scheduled thread (`(uint64_t)next->stack + next->stack_size`) on every context switch.

3. **Ring 3 Privilege Transition (Observation 3)**:
   - Because no transition mechanism exists, KOS requires a dedicated assembly primitive `enter_userspace(uint64_t entry_point, uint64_t user_rsp)` in `arch/x86_64/cpu.asm`.
   - The primitive must load DS, ES, FS, GS with `0x33` (`KOS_USER_DATA_SELECTOR`), push the 5 quadwords for `iretq` (SS=0x33, RSP=user_rsp, RFLAGS=0x202, CS=0x2b, RIP=entry_point), sanitize all 15 general-purpose registers to avoid kernel information leakage, and execute `iretq`.

4. **Interrupt Handling from Ring 3 (Observation 4)**:
   - The hardware stack frame pushed by x86_64 CPU upon transition from CPL=3 to CPL=0 (SS, RSP, RFLAGS, CS, RIP) matches `struct interrupt_context` offsets 0x88 through 0xA8 exactly.
   - `isr_common` restores registers and executes `iretq`, which restores user RSP and user SS and switches CPL back to 3.
   - Hardware PIC IRQs already support preemption via `task_reschedule_if_needed()`, which seamlessly preempts Ring 3 threads when `TSS.rsp0` is updated per thread.
   - However, fault handlers currently call `cpu_halt()` (Page Fault) or `panic_with_context()` (General Protection Fault, Invalid Opcode). When `(context->cs & 3) == 3`, these must instead invoke `process_exit(process_get_current(), -1)` to safely terminate the offending user process and keep the kernel alive.

---

## 3. Caveats

1. **Per-CPU Multiprocessor Support**: The current TSS is a single global structure (`static struct task_state_segment tss`). This is completely correct for KOS's current uniprocessor architecture. If SMP is introduced in future milestones, per-CPU TSS and GDT instances will be required.
2. **TLS / FS_BASE / GS_BASE**: KOS does not currently use `swapgs` or per-thread `FS_BASE`/`GS_BASE` for Thread Local Storage (TLS). If userspace C runtime requires thread-local storage, `arch_prctl` or `wrmsr` for `IA32_FS_BASE` must be added in subsequent milestones.
3. **Paging Permissions (`PTE_USER`)**: Ring 3 execution requires page table entries mapping user code and data to have the User/Supervisor bit (`1ull << 2`) enabled throughout PML4, PDPT, PD, and PT. This is investigated by Survey 2 (VMM / ELF loader).

---

## 4. Conclusion

The existing KOS architecture is in a strong position for Milestone K14 Requirement R2. The GDT descriptors for Ring 3 and the TSS structure loaded via `ltr` are already present and verified.

The exact architectural changes required for R2 are:
1. **Define Aliases**: Add `GDT_USER_CS` (`0x2b`) and `GDT_USER_DS` (`0x33`) in `include/kos/descriptors.h`.
2. **Update TSS.rsp0 on Context Switch**: In `kernel/task.c` `schedule()`, call `tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull)` on every thread switch.
3. **Implement Ring 3 Transition Primitive**: Add `enter_userspace(entry_point, user_rsp)` in `arch/x86_64/cpu.asm` using `iretq` with register scrubbing.
4. **Implement Address Space Switch**: Add `cpu_write_cr3(cr3)` in `arch/x86_64/cpu_control.c` and call it in `schedule()` when switching between processes with different `pml4_physical`.
5. **Hardening User Exception Dispatch**: In `arch/x86_64/interrupts.c`, update `page_fault_handler()` and `interrupt_dispatch()` so that any fault from Ring 3 (`(context->cs & 3) == 3`) calls `process_exit(cur_proc, -1)` rather than halting or panicking the kernel.

---

## 5. Verification Method

1. **Compilation**:
   ```pwsh
   .\tools\kos-tool.exe build
   ```
   Must complete with code 0 and generate `build/kos.elf`.
2. **GDT / TSS Inspection**:
   Inspect `arch/x86_64/descriptors.c`:
   Verify `gdt_entries[5] == 0x00affa000000ffffull` (User CS = `0x2b`) and `gdt_entries[6] == 0x00cff2000000ffffull` (User DS = `0x33`).
   Inspect `kernel/task.c`:
   Verify that `schedule()` contains `tss_set_rsp0(next_rsp0)`.
3. **QEMU Smoke Test**:
   Run `.\tools\kos-tool.exe run` to launch QEMU:
   - Verify boot proceeds normally without assertion failure.
   - Verify `exception_self_test()` passes.
   - Execute user test process; verify successful transition to Ring 3, `int 0x80` syscall dispatch, and clean termination upon hitting guard page.
