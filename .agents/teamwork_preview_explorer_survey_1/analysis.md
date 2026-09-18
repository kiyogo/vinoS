# Milestone K14 Architecture Survey: Hardware Context, GDT, TSS, Ring 3 Transitions & Interrupt Stack Management

**Author**: Explorer Survey 1 (`teamwork_preview_explorer_survey_1`)  
**Date**: 2026-09-18  
**Scope**: Requirement R2 (Ring 3 Privilege Transition & Hardware Context Management) for KOS x86_64  
**Target Codebase**: `d:\DATA\source\OS\KOS\OS2`  

---

## 1. Executive Summary

This investigation provides a comprehensive architectural and code-level audit of KOS x86_64 descriptor tables (GDT, IDT), Task State Segment (TSS), privilege transition mechanisms (Ring 0 to Ring 3), interrupt/exception stack switching, and preemptive context switching for Milestone K14.

### Key Findings Matrix

| Component | Status | Code Location | Assessment & Required Changes |
|---|---|---|---|
| **GDT Descriptors** | Configured | `arch/x86_64/descriptors.c:17-25` | User Code (`0x2b`) & Data (`0x33`) descriptors are present and valid 64-bit descriptors. Need formal alias defines `GDT_USER_CS` and `GDT_USER_DS`. |
| **TSS Definition & Loading** | Initialized | `arch/x86_64/descriptors.c:80-97`, `arch/x86_64/cpu.asm:30-33` | TSS defined (104 bytes), loaded via `ltr` with selector `0x18`. IST1 configured for Double Fault (#DF). Initial `rsp0` points to boot stack. |
| **TSS RSP0 Context Switch** | **DEFECT / MISSING** | `kernel/task.c:261-303` (`schedule()`) | `tss_set_rsp0()` is **never** called during context switches. User threads taking interrupts would all overwrite the static boot stack! |
| **Ring 3 Entry (`iretq`)** | **MISSING** | `arch/x86_64/cpu.asm` | No mechanism exists to transition from Ring 0 to Ring 3. Must implement `enter_userspace(entry_point, user_rsp)` via `iretq`. |
| **Interrupt Stack Switching** | Compatible | `arch/x86_64/interrupt_stubs.asm:35-76` | `struct interrupt_context` (176 bytes) precisely matches CPU 64-bit interrupt frame. Hardware stack switch from Ring 3 to TSS.rsp0 works cleanly. |
| **Ring 3 Fault Containment** | **DEFECT** | `arch/x86_64/interrupts.c:258-269, 393` | User page faults call `cpu_halt()`. Other user exceptions trigger kernel panic. Must convert user faults to `process_exit(proc, -1)`. |
| **Address Space (CR3) Switching** | **MISSING** | `kernel/task.c:300`, `arch/x86_64/cpu_control.c` | No `cpu_write_cr3()`. Scheduler does not switch CR3 when switching between different processes. |

---

## 2. Deep Dive: GDT & Segment Selectors

### 2.1 Existing GDT Implementation
In `arch/x86_64/descriptors.c`, lines 17-25:
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

Existing selector definitions in `include/kos/cpu.h`, lines 8-14:
```c
enum {
    KOS_KERNEL_CODE_SELECTOR = 0x08,
    KOS_KERNEL_DATA_SELECTOR = 0x10,
    KOS_KERNEL_TSS_SELECTOR = 0x18,
    KOS_USER_CODE_SELECTOR = 0x2b,
    KOS_USER_DATA_SELECTOR = 0x33,
};
```

### 2.2 Bit-Level Descriptor Audit
1. **Entry 5: User Code Descriptor (`0x00affa000000ffffull`)**:
   - Limit: `0xfffff` (combined with G=1: 4 KiB units = 4 GiB span, ignored in 64-bit mode except for compatibility).
   - Base: `0x00000000`.
   - Access Byte (`0xfa`):
     - P (Present): 1
     - DPL (Descriptor Privilege Level): `11b` (Ring 3)
     - S (Descriptor Type): 1 (Code/Data)
     - Executable: 1 (Code segment)
     - Conforming: 0 (Non-conforming)
     - Readable: 1 (Readable code segment)
     - Accessed: 0
   - Flags Nybble (`0xa`):
     - Granularity (G): 1 (4 KiB)
     - D/B (Default Size): 0 (Must be 0 when L=1 in AMD64/Intel 64)
     - L (Long Mode): 1 (64-bit code segment!)
     - AVL: 0
   - **Selector**: Index 5. Byte offset = `5 * 8 = 40 = 0x28`. With RPL=3: `0x28 | 3 = 0x2b`.
   - **Assessment**: Correct and valid 64-bit User Code Descriptor.

2. **Entry 6: User Data Descriptor (`0x00cff2000000ffffull`)**:
   - Limit: `0xfffff`. Base: `0x00000000`.
   - Access Byte (`0xf2`):
     - P (Present): 1
     - DPL: `11b` (Ring 3)
     - S: 1 (Code/Data)
     - Executable: 0 (Data segment)
     - Expand-down: 0 (Expand-up)
     - Writable: 1 (Writable data segment)
     - Accessed: 0
   - Flags Nybble (`0xc`):
     - Granularity (G): 1
     - D/B: 1 (Default 32-bit stack/op size for data segment)
     - L: 0 (Must be 0 for data segment)
     - AVL: 0
   - **Selector**: Index 6. Byte offset = `6 * 8 = 48 = 0x30`. With RPL=3: `0x30 | 3 = 0x33`.
   - **Assessment**: Correct and valid Ring 3 User Data Descriptor.

### 2.3 Compatibility Note: `iretq` vs `sysret`
- In AMD64 architecture, the `sysret` instruction loads:
  - User SS = `STAR[63:48] + 8` (with RPL=3)
  - User CS = `STAR[63:48] + 16` (with RPL=3)
  This requires the User Data segment to be placed *before* the User Code segment in the GDT.
- In KOS's current GDT, User Code is at index 5 (`0x28`) and User Data is at index 6 (`0x30`).
- **Critical Conclusion**: Because KOS implements Ring 3 privilege transitions using `iretq` (Requirement R2) and syscalls via software interrupt `int 0x80`, the GDT ordering does not affect correctness. `iretq` pops CS and SS directly from the stack and does not depend on relative descriptor ordering.
- **Recommendation**: Define explicit aliases in `include/kos/descriptors.h` or `include/kos/cpu.h`:
  ```c
  #define GDT_USER_CS KOS_USER_CODE_SELECTOR /* 0x2b */
  #define GDT_USER_DS KOS_USER_DATA_SELECTOR /* 0x33 */
  ```

---

## 3. Deep Dive: Task State Segment (TSS) & RSP0 Management

### 3.1 TSS Definition and Loading
- `struct task_state_segment` is defined in `include/kos/descriptors.h` (lines 14-30):
  ```c
  struct task_state_segment {
      uint32_t reserved0;
      uint64_t rsp0;
      uint64_t rsp1;
      uint64_t rsp2;
      uint64_t reserved1;
      uint64_t ist1; ... uint64_t ist7;
      uint64_t reserved2;
      uint16_t reserved3;
      uint16_t io_map_base;
  } __attribute__((packed));
  ```
- Size: Exactly 104 bytes (`sizeof == 104`, verified by `_Static_assert`).
- GDT Entry: In 64-bit mode, TSS descriptor is a 16-byte system descriptor occupying two entries (`gdt_entries[3]` and `gdt_entries[4]`). Configured in `gdt_configure_tss_descriptor()` with type `0x89` (64-bit TSS available).
- Loading: `tss_load(KOS_KERNEL_TSS_SELECTOR)` in `arch/x86_64/cpu.asm`:
  ```nasm
  tss_load:
      mov ax, di
      ltr ax
      ret
  ```
  Selector `0x18` (index 3, RPL=0) is loaded into the Task Register (TR).
- Double Fault Stack: `tss.ist1` is configured to `double_fault_stack + DOUBLE_FAULT_STACK_SIZE` (16 KiB dedicated stack) for Vector 8 (#DF).

### 3.2 The RSP0 Context Switch Defect
- **Observation**:
  In `arch/x86_64/descriptors.c` line 81:
  `tss_set_rsp0((uint64_t)kos_boot_stack_top);`
  This is the **only** place in the entire kernel where `tss_set_rsp0()` is called.
- In `kernel/task.c`, `schedule()` (lines 261-303) performs:
  ```c
  task_switch(&previous->context, &next->context);
  ```
  It **never** updates `tss.rsp0`!
- **Consequences for Ring 3**:
  1. When a user thread executes in Ring 3, an interrupt (e.g. 1000 Hz timer tick) or exception triggers an x86_64 hardware stack switch to `TSS.rsp0`.
  2. Currently, `TSS.rsp0` continuously points to `kos_boot_stack_top`.
  3. Every user thread in the system will dump its interrupt frame (`struct interrupt_context`) to the single shared `kos_boot_stack_top`.
  4. If a thread is preempted during an interrupt or syscall, its kernel state on the boot stack will be overwritten by the next user thread that takes an interrupt!
- **Required Fix**:
  Each thread has its own kernel stack allocated via `kmalloc(TASK_KERNEL_STACK_SIZE)`:
  `uint64_t stack_top = ((uint64_t)next->stack + next->stack_size) & ~0xFull;`
  In `kernel/task.c` `schedule()`:
  ```c
  if (next->stack != NULL && next->stack_size > 0) {
      uint64_t rsp0 = ((uint64_t)next->stack + next->stack_size) & ~0xFull;
      tss_set_rsp0(rsp0);
  } else {
      tss_set_rsp0((uint64_t)kos_boot_stack_top);
  }
  ```
  Updating `tss_set_rsp0` during `schedule()` guarantees that whenever `next` is running in Ring 3, any privilege elevation (CPL 3 -> CPL 0) will land on `next`'s private kernel stack.

---

## 4. Deep Dive: Ring 3 Entry & Transitions

### 4.1 Current Status
- Currently, all threads created in KOS are kernel threads.
- `thread_create()` initializes `.rip = (uint64_t)task_start_trampoline`, executing in Ring 0.
- There is no code in KOS that currently jumps to Ring 3.

### 4.2 Entry Mechanism: `iretq`
To safely transition the CPU from Ring 0 (kernel mode) to Ring 3 (user mode), KOS must construct a standard 64-bit `iretq` stack frame:

```
Higher Memory
+-----------------------+
| SS (0x33, RPL=3)      |  [RSP + 32]
+-----------------------+
| User RSP              |  [RSP + 24]
+-----------------------+
| RFLAGS (0x202)        |  [RSP + 16]
+-----------------------+
| CS (0x2b, RPL=3)      |  [RSP + 8]
+-----------------------+
| User RIP              |  [RSP + 0]
+-----------------------+
Lower Memory (pointed to by kernel RSP immediately prior to iretq)
```

### 4.3 Detailed Frame Parameters
1. **User SS**: `KOS_USER_DATA_SELECTOR = 0x33` (`0x30 | 3`).
2. **User RSP**: Virtual address of the top of the user stack (System V AMD64 ABI aligned to 16 bytes).
3. **User RFLAGS**:
   - Bit 1: Reserved, must be 1.
   - Bit 9: Interrupt Flag (IF). **Must be 1 (`0x200`)**! If IF=0, user space would run with interrupts disabled, preventing preemption, timer ticks, and keyboard input.
   - Bits 12-13: I/O Privilege Level (IOPL). **Must be 0 (`0x0000`)**! IOPL=0 strictly prohibits user space from executing `in`, `out`, `cli`, `sti`, `hlt`.
   - Bits 14 (NT), 16 (RF), 17 (VM): Must be 0.
   - Base RFLAGS value: `0x202` (IF=1, Reserved=1).
4. **User CS**: `KOS_USER_CODE_SELECTOR = 0x2b` (`0x28 | 3`).
5. **User RIP**: Entry point of the loaded user executable (ELF entry address).

### 4.4 Kernel Information Leak Prevention (Register Scrubbing)
Before executing `iretq`, the kernel **must** zero out all general purpose registers (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rbp`, `r8`..`r15`). Leaving kernel pointers or kernel stack values in registers is a severe security vulnerability.

### 4.5 Data Segment Registers
In x86_64 long mode, `iretq` does not reload `ds`, `es`, `fs`, `gs`. Prior to `iretq`, `ds` and `es` must be loaded with `0x33` (`KOS_USER_DATA_SELECTOR`). `fs` and `gs` should be set to `0x33` or 0.

### 4.6 Recommended Assembly Primitive
In `arch/x86_64/cpu.asm`:
```nasm
global enter_userspace
; void enter_userspace(uint64_t entry_point, uint64_t user_rsp);
; System V AMD64 ABI: rdi = entry_point, rsi = user_rsp
enter_userspace:
    cli
    mov ax, 0x33        ; KOS_USER_DATA_SELECTOR (RPL=3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x33     ; SS: User Data Segment
    push rsi            ; RSP: User Stack Pointer
    push qword 0x202    ; RFLAGS: IF=1, IOPL=0, Bit 1=1
    push qword 0x2b     ; CS: User Code Segment (RPL=3)
    push rdi            ; RIP: User Entry Point

    ; Clear general-purpose registers to avoid kernel data leaks
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor rdi, rdi
    xor rsi, rsi

    iretq
```

---

## 5. Deep Dive: Interrupts & Exceptions from Ring 3

### 5.1 Hardware Stack Switch & Frame Layout
When an interrupt or exception occurs while CPL=3:
1. The CPU detects privilege transition (CPL 3 -> DPL 0 of interrupt gate).
2. The CPU reads `TSS.rsp0` and loads `RSP = TSS.rsp0`.
3. The CPU pushes SS (0x33), RSP, RFLAGS, CS (0x2b), RIP.
4. If an exception produces an error code (e.g. #PF vector 14, #GP vector 13, #DF vector 8), the CPU pushes the 64-bit error code.
5. Control transfers to `isr_stub_<vector>` in `arch/x86_64/interrupt_stubs.asm`.
6. Stubs normalize the stack:
   - `ISR_NO_ERROR`: pushes dummy error code 0, then vector number.
   - `ISR_ERROR_CODE`: pushes vector number.
7. `isr_common` pushes `rax, rbx, rcx, rdx, rbp, rsi, rdi, r8..r15` (15 registers = 120 bytes).
8. Stack layout at this point:
   - 22 quadwords = 176 bytes.
   - Exactly matches `struct interrupt_context` in `include/kos/interrupts.h`:
     `[rsp + 0x00]` .. `[rsp + 0x70]`: GPRs (r15 down to rax)  
     `[rsp + 0x78]`: vector  
     `[rsp + 0x80]`: error_code  
     `[rsp + 0x88]`: rip  
     `[rsp + 0x90]`: cs (`0x2b` for Ring 3)  
     `[rsp + 0x98]`: rflags  
     `[rsp + 0xA0]`: rsp (User stack pointer)  
     `[rsp + 0xA8]`: ss (`0x33` for Ring 3)  

### 5.2 System V ABI Compliance in `isr_common`
In `arch/x86_64/interrupt_stubs.asm` lines 52-58:
```nasm
    mov r12, rsp
    and rsp, -16
    mov rdi, r12
    cld
    call interrupt_dispatch
    mov rsp, r12
```
- `r12` is saved on the stack (at `[rsp + 0x18]`) and is callee-saved across the C call.
- `and rsp, -16` ensures 16-byte stack alignment for GCC/Clang compiled C functions.
- `mov rdi, r12` passes the exact pointer to `struct interrupt_context` as argument 1.
- `mov rsp, r12` restores the exact unaligned pointer before popping registers.
- `pop r15..rax`, `add rsp, 16` (discards vector and error_code), `iretq`.
- **Assessment**: The assembly stub mechanism is 100% correct and fully compatible with Ring 3 transitions.

### 5.3 Audit of Interrupt & Exception Dispatch Paths
Let's evaluate each dispatch stage in `interrupt_dispatch` (`arch/x86_64/interrupts.c`):

1. **Syscall (`vector 0x80`)**:
   - Gate configuration: `idt_entries[KOS_SYSCALL_VECTOR].type_attributes = 0xee;` (DPL=3).
   - Ring 3 can execute `int 0x80` without triggering #GP.
   - Dispatch:
     ```c
     mutable_context->rax = kos_syscall_dispatch(context->rax, context->rdi,
         context->rsi, context->rdx, context->r10, context->r8, context->r9);
     ```
   - On return, `isr_common` pops registers (RAX contains the syscall return value), pops error_code/vector, and executes `iretq`.
   - `iretq` restores user RSP, user SS, user RIP, user CS (0x2b).
   - **Status**: **Fully functional and ready for Ring 3**.

2. **Hardware PIC IRQs (vectors 32 to 47)**:
   - When a hardware interrupt (e.g. IRQ0 timer tick) fires while in Ring 3:
     - Switches to `TSS.rsp0`.
     - Calls registered handler.
     - PIC sends EOI.
     - Checks preemption hook:
       ```c
       if (task_is_multitasking_active() && (context->rflags & (1ull << 9))) {
           task_reschedule_if_needed();
       }
       ```
     - If preemption triggers, `schedule()` calls `task_switch(&previous->context, &next->context)`.
     - `task_switch` saves callee-saved registers (including `r12` and the kernel `rsp` inside the interrupt frame) into `previous->context`.
     - When `previous` is scheduled again, `task_switch` restores kernel `rsp`, returns to `isr_common`, pops registers, and executes `iretq` back to Ring 3!
     - **Status**: **Functionally sound**, **provided** `tss_set_rsp0` is updated in `schedule()`.

3. **Page Fault Handler (#PF, vector 14)**:
   - In `arch/x86_64/interrupts.c` lines 254-269:
     ```c
     if (!(context->error_code & PF_ERROR_USER)) {
         log_page_fault_panic(context, &record);
     } else {
         /* User mode page fault (Ring 3) */
         log_enter_emergency_mode();
         ...
         log_error("User space process termination not yet available in K12 -> System halted.");
         cpu_halt();
     }
     ```
   - **DEFECT**: In K12, a user-mode page fault halted the CPU (`cpu_halt()`).
   - For K14, Acceptance Criteria require:
     - Hitting the user stack guard page cleanly faults and terminates the faulting process without crashing the kernel.
     - Memory protections are strictly enforced: Ring 3 cannot access kernel-only pages without triggering a page fault handled by the kernel.
   - **Required Fix**:
     1. Decrement recursion counter: `__atomic_sub_fetch(&s_pf_recursion, 1, __ATOMIC_SEQ_CST);`
     2. Identify current process: `struct process *cur_proc = process_get_current();`
     3. Terminate process cleanly: `process_exit(cur_proc, -1);` (which invokes `thread_exit(-1)` and reschedules).

4. **Unhandled CPU Exceptions (vectors 0..13, 15..31)**:
   - Lines 392-394:
     ```c
     default_exception_panic(context);
     ```
   - **DEFECT**: Any unhandled exception from Ring 3 (such as General Protection Fault #GP vector 13 from executing privileged instructions `cli`/`hlt`, or Divide-by-zero #DE vector 0, or Invalid Opcode #UD vector 6) triggers `default_exception_panic` which halts the entire kernel!
   - **Required Fix**: Check privilege level of faulting frame:
     ```c
     if ((context->cs & 3) == 3) {
         log_warn("User process (PID %lu, TID %lu) terminated due to unhandled exception: %s (vector 0x%02lx) at RIP 0x%016lx",
                  task_current()->process ? task_current()->process->pid : 0,
                  task_current_tid(),
                  exception_name(context->vector),
                  context->vector,
                  context->rip);
         process_exit(process_get_current(), -1);
     }
     ```

5. **Breakpoint Exception (#BP, vector 3)**:
   - In `arch/x86_64/interrupts.c` line 67, gates are initialized with `IDT_INTERRUPT_GATE = 0x8e` (DPL=0).
   - If user code executes `int 3` (`0xCC`), CPL=3 > DPL=0 triggers a General Protection Fault (#GP, vector 13) instead of #BP!
   - **Recommendation**: Set `idt_entries[3].type_attributes = 0xee;` (DPL=3) so user breakpoints work.

---

## 6. Synthesis & Architectural Recommendations for Milestone K14 (R2)

To satisfy Requirement R2 and all related acceptance criteria, the following cohesive changes must be implemented:

### Step 1: Selector Definitions & Aliases (`include/kos/cpu.h` & `include/kos/descriptors.h`)
Add canonical selector aliases:
```c
#define GDT_KERNEL_CS  KOS_KERNEL_CODE_SELECTOR /* 0x08 */
#define GDT_KERNEL_DS  KOS_KERNEL_DATA_SELECTOR /* 0x10 */
#define GDT_TSS        KOS_KERNEL_TSS_SELECTOR  /* 0x18 */
#define GDT_USER_CS    KOS_USER_CODE_SELECTOR   /* 0x2b */
#define GDT_USER_DS    KOS_USER_DATA_SELECTOR   /* 0x33 */
```

### Step 2: Dynamic TSS RSP0 Updates in Scheduler (`kernel/task.c`)
In `schedule()`:
```c
void schedule(void) {
    ...
    struct task *previous = current_task;
    ...
    current_task = next;

    /* Update TSS RSP0 to current thread's private kernel stack */
    if (next->stack != NULL && next->stack_size > 0) {
        uint64_t next_rsp0 = ((uint64_t)next->stack + next->stack_size) & ~0xFull;
        tss_set_rsp0(next_rsp0);
    } else {
        tss_set_rsp0((uint64_t)kos_boot_stack_top);
    }

    /* Update CR3 if switching between processes */
    if (next->process != NULL && next->process->pml4_physical != 0) {
        uint64_t cur_cr3 = cpu_read_cr3();
        if (cur_cr3 != next->process->pml4_physical) {
            cpu_write_cr3(next->process->pml4_physical);
        }
    }

    task_switch(&previous->context, &next->context);
    cpu_interrupt_restore(flags);
}
```

### Step 3: CR3 Control Primitive (`arch/x86_64/cpu_control.c` & `include/kos/cpu.h`)
Add `cpu_write_cr3`:
```c
void cpu_write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

### Step 4: Ring 3 Transition Routine (`arch/x86_64/cpu.asm`)
Add `enter_userspace(uint64_t entry_point, uint64_t user_rsp)` to `arch/x86_64/cpu.asm` with full register scrubbing, data segment selector loading (0x33), and `iretq` execution.

### Step 5: User Fault Termination in Interrupt Handler (`arch/x86_64/interrupts.c`)
- In `page_fault_handler`: Replace `cpu_halt()` on user-mode faults with `process_exit(process_get_current(), -1)`.
- In `interrupt_dispatch`: For vectors 0-31, check `if ((context->cs & 3) == 3)`: log warning and call `process_exit(process_get_current(), -1)` rather than `default_exception_panic(context)`.
- In `idt_initialize`: Set `idt_entries[3].type_attributes = 0xee;` for user breakpoint support.

### Step 6: User Process Trampoline (`kernel/process.c`)
Implement a trampoline function for user threads:
```c
void user_thread_trampoline(void *arg) {
    struct user_thread_start_args *args = (struct user_thread_start_args *)arg;
    enter_userspace(args->entry_point, args->user_rsp);
}
```

---

## 7. Verification Strategy

1. **Static Build Verification**:
   - `.\tools\kos-tool.exe build` compiles without errors or warnings.
2. **Descriptor Verification**:
   - Boot assertions verify GDT limits, TSS selectors, and `tss_get_rsp0()`.
3. **QEMU Test Verification**:
   - Test a user process executing `int 0x80` syscalls (`uptime`, `sysinfo`).
   - Test timer preemption during user mode execution (verify timer ticks and `task_reschedule_if_needed` return cleanly via `iretq`).
   - Test deliberate fault injection: user mode reading kernel memory (0xFFFF8000...) or dereferencing NULL or hitting the stack guard page; verify that the process is terminated with exit code -1 and kernel remains completely stable.
