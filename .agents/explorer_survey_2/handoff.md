# Technical Survey & Handoff Report: Ring 3 Privilege Transition & Syscall ABI (Milestone K13 & K14)

**Agent ID**: `explorer_survey_2`  
**Role**: Ring 3 Privilege Transition & Syscall ABI Explorer  
**Project**: KOS x86_64  
**Date**: 2026-09-18T10:00:00Z  
**Target Requirements**: Requirement R3 (Ring 3 Privilege Transition & Context Switching) & Requirement R4 (Syscall Boundary & Core ABI)

---

## 1. Observation

Direct code inspection of the KOS codebase (`D:\DATA\source\OS\KOS\OS2`) revealed the following exact facts across the CPU descriptor, interrupt, scheduler, and syscall subsystems:

### 1.1 GDT and TSS Layout
In `arch/x86_64/descriptors.c:17-25`:
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
In `include/kos/descriptors.h:41-44` and `include/kos/cpu.h:8-14`:
```c
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_CS   0x2b
#define GDT_USER_DS   0x33
```
- **Entry 1 (`0x08`)**: Kernel Code 64-bit (Access `0x9A`: P=1, DPL=0, S=1, Code, R=1; Flags `0xA`: L=1, D=0, G=1).
- **Entry 2 (`0x10`)**: Kernel Data (Access `0x92`: P=1, DPL=0, S=1, Data, W=1; Flags `0xC`: D=1, G=1).
- **Entries 3 & 4 (`0x18` & `0x20`)**: 128-bit System Descriptor for 64-bit TSS (`0x89`: P=1, DPL=0, Type=9 Available 64-bit TSS). Configured dynamically in `gdt_configure_tss_descriptor()` (`arch/x86_64/descriptors.c:30-40`).
- **Entry 5 (`0x2B`)**: User Code 64-bit (Access `0xFA`: P=1, DPL=3, S=1, Code, R=1; Flags `0xA`: L=1, D=0, G=1).
- **Entry 6 (`0x33`)**: User Data (Access `0xF2`: P=1, DPL=3, S=1, Data, W=1; Flags `0xC`: D=1, G=1).
- **32-bit Compatibility Mode**: Not present in `gdt_entries`.

### 1.2 TSS and Kernel Stack Setup
In `arch/x86_64/descriptors.c:27-28, 80-92`:
```c
static struct task_state_segment tss KOS_ALIGNED(16);
static uint8_t double_fault_stack[DOUBLE_FAULT_STACK_SIZE] KOS_ALIGNED(16); // 16 KiB

void gdt_initialize(void) {
    tss_set_rsp0((uint64_t)kos_boot_stack_top);
    tss.ist1 = (uint64_t)(double_fault_stack + DOUBLE_FAULT_STACK_SIZE);
    tss.io_map_base = sizeof(tss);
    gdt_configure_tss_descriptor();
    ...
    tss_load(KOS_KERNEL_TSS_SELECTOR); // selector 0x18 via ltr ax
}
```
In `kernel/task.c:307-312`:
```c
if (next->stack != NULL) {
    tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
} else {
    extern uint8_t kos_boot_stack_top[];
    tss_set_rsp0((uint64_t)kos_boot_stack_top);
}
```
In `arch/x86_64/interrupts.c:538-539`:
```c
uint8_t ist = vector == 8 ? 1 : 0;
idt_set_gate((uint8_t)vector, isr_stub_table[vector], ist);
```
- TSS is loaded once at boot via `ltr ax` (`0x18`).
- `tss.rsp0` is dynamically set on every task context switch to the top of `next->stack` (16 KiB dedicated kernel stack per thread).
- IST1 is assigned exclusively to Vector 8 (#DF - Double Fault). Vector 14 (#PF) and hardware IRQs use IST 0 (switching to `tss.rsp0` when coming from Ring 3).

### 1.3 Ring 3 Entry Function
In `arch/x86_64/cpu.asm:40-81`:
```nasm
enter_userspace:
    cli
    mov ax, 0x33
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x33      ; SS (GDT_USER_DS)
    push rsi             ; RSP (user_rsp)
    push qword 0x202     ; RFLAGS (IF=1, bit 1=1)
    push qword 0x2b      ; CS (GDT_USER_CS)
    push rdi             ; RIP (entry_point)

    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq
```
- `enter_userspace` takes 2 arguments: `(rdi: entry_point, rsi: user_rsp)`. It zeroes all GPRs including `rdi` and `rsi`.
- Requirement R3 calls for `enter_user_mode(entry_point, user_stack_top, argc, argv)` where `argc` and `argv` can be passed to user entry in `rdi` and `rsi`.

### 1.4 Context Switch & Address Space Isolation
In `kernel/task.c:301-315`:
```c
if (next->process != NULL && next->process->pml4_physical != 0) {
    if (previous == NULL || previous->process != next->process) {
        cpu_write_cr3(next->process->pml4_physical);
    }
}

if (next->stack != NULL) {
    tss_set_rsp0(((uint64_t)next->stack + next->stack_size) & ~0xFull);
} else {
    extern uint8_t kos_boot_stack_top[];
    tss_set_rsp0((uint64_t)kos_boot_stack_top);
}

task_switch(&previous->context, &next->context);
```
In `arch/x86_64/task_switch.asm:10-35`:
- `task_switch` saves callee-saved registers (`rbx, rbp, r12, r13, r14, r15, rsp, rip, rflags`) into `old` and restores them from `next`.
- Note: If `next->process == NULL`, CR3 is not currently explicitly updated to `kernel_process.pml4_physical`.

### 1.5 Existing Syscall Subsystem
In `include/kos/syscall.h:8-13`:
```c
enum kos_syscall_number {
    KOS_SYSCALL_GETTIME = 1,
    KOS_SYSCALL_SYSINFO = 2,
    KOS_SYSCALL_WRITE = 3,
    KOS_SYSCALL_EXIT = 4,
};
```
In `kernel/syscall.c:8-31`:
- Dispatches based on numbers 1..4. `WRITE` currently returns `UINT64_MAX` (refused).
- MSRs (`IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`, `IA32_EFER`) are not currently touched; no MSR access functions exist in `arch/x86_64/`.
In `arch/x86_64/interrupts.c:428-433, 542`:
- Vector `0x80` is installed with `type_attributes = 0xee` (DPL 3, Interrupt Gate).
- When `int 0x80` is executed from Ring 3, `interrupt_dispatch` extracts:
  `kos_syscall_dispatch(context->rax, context->rdi, context->rsi, context->rdx, context->r10, context->r8, context->r9)`.
- The result is stored back into `mutable_context->rax` and restored via `isr_common`'s `iretq`.
In `kernel/command.c:2210, 2482, 2533, 2609, 2660, 2745, 2836`:
- Six separate tests in the `usertest` suite hardcode `mov rax, 4` (`0x48, 0xC7, 0xC0, 0x04, ...`) to exit test processes via `int 0x80`.

### 1.6 User Mode Exception Handling
In `arch/x86_64/interrupts.c:256-284, 356-424, 467-471`:
- User mode fault detection:
  `bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);`
- In `page_fault_handler`: if `is_user` is true, logs a diagnostic warning and executes:
  `process_exit(cur, -11); schedule();`
- In `interrupt_dispatch`: for any CPU exception (vectors 0..31) without a custom handler, if `(context->cs & 3) == 3`, calls `handle_user_exception(context)`:
  Logs full register dump (`RIP`, `RSP`, `RFLAGS`, all GPRs) and executes:
  `process_exit(cur, -1); schedule();`
- When `process_exit()` is called, it wakes up the parent via `proc->exit_wq`, and terminates the calling thread via `thread_exit()`, switching away to another thread. The kernel never panics on user-mode faults.

---

## 2. Logic Chain

### 2.1 The MSR_STAR and SYSRET Inversion Problem
1. **Hardware Rule (Intel SDM Vol 2B & AMD64 APM Vol 3)**:
   When `SYSRET` with 64-bit operand (`sysretq`) executes from CPL 0 to CPL 3:
   - Target Code Segment (CS) selector: `CS = (STAR[63:48] + 16) | 3`
   - Target Stack Segment (SS) selector: `SS = (STAR[63:48] + 8) | 3`
2. **Mathematical Implication**:
   - `SS_index = (STAR[63:48] + 8) >> 3`
   - `CS_index = (STAR[63:48] + 16) >> 3 = SS_index + 1`
   - Therefore, hardware `sysretq` **strictly requires** that the User Data Segment (SS) precede the 64-bit User Code Segment (CS) in the GDT.
3. **KOS GDT Evaluation**:
   - KOS currently places User Code at GDT Index 5 (`0x28 -> 0x2B`) and User Data at GDT Index 6 (`0x30 -> 0x33`).
   - In KOS's current layout, `CS_index = 5` and `SS_index = 6`. `CS` precedes `SS`!
   - If `STAR[63:48]` were set to `0x20`:
     - `SS = (0x20 + 8) | 3 = 0x28 | 3 = 0x2B` (User Code loaded into SS!).
     - `CS = (0x20 + 16) | 3 = 0x30 | 3 = 0x33` (User Data loaded into CS!).
     This is completely inverted and causes an immediate processor fault.
4. **Resolution Strategy**:
   - **Path A (Recommended - Return via `iretq`)**:
     The `syscall` entry instruction can save user `rip` (from `rcx`), user `rflags` (from `r11`), and user `rsp` (from scratch register), push a standard 5-quadword `iretq` frame (`SS=0x33, RSP, RFLAGS, CS=0x2B, RIP`), and return to userspace via `iretq`.
     - *Advantages*:
       1. Zero GDT reorganization required (retains `0x2B` as User CS and `0x33` as User DS across all documents and headers).
       2. Full atomic restoration of all flags and stack pointer (eliminates vulnerability where user stack pointer is exposed while in CPL 0).
       3. Seamless handling of blocking syscalls (`SYS_SLEEP`, `SYS_READ`) and preemptive context switches.
       4. Matches modern Linux PTI / KPTI syscall return semantics.
   - **Path B (GDT Reorganization for `sysretq`)**:
     If pure `sysretq` instruction execution is mandatory:
     Reorganize GDT to standard layout:
     `0x00` Null, `0x08` Kernel CS, `0x10` Kernel DS, `0x18` Compatibility 32-bit CS, `0x20` User DS (`0x23`), `0x28` User 64-bit CS (`0x2B`), `0x30` TSS Low, `0x38` TSS High.
     `STAR[63:48] = 0x18` -> `STAR+8 = 0x20` (SS = `0x23`), `STAR+16 = 0x28` (CS = `0x2B`).
     *Notice*: User Data selector in this scheme becomes `0x23`, not `0x33`.

### 2.2 Syscall Number Realignment (Requirement R4)
1. Requirement R4 defines the authoritative syscall table:
   - `SYS_EXIT` = 1
   - `SYS_WRITE` = 2
   - `SYS_READ` = 3
   - `SYS_GETTIME` = 4
   - `SYS_SYSINFO` = 5
   - `SYS_YIELD` = 6
   - `SYS_SLEEP` = 7
2. In the existing code (`include/kos/syscall.h`), `KOS_SYSCALL_GETTIME` is 1, `SYSINFO` is 2, `WRITE` is 3, and `EXIT` is 4.
3. Therefore:
   - `include/kos/syscall.h` must be updated to the R4 numbering.
   - `kernel/syscall.c` must implement all 7 functions.
   - **Critical coupling**: The test suite in `kernel/command.c` (6 test cases in `usertest`) currently emits machine code `0x48, 0xC7, 0xC0, 0x04, ...` (`mov rax, 4`). When `SYS_EXIT` changes to 1, these tests must be updated to `mov rax, 1` (`0x01`); otherwise, tests calling exit will mistakenly call `SYS_GETTIME` and fail with a timeout!

### 2.3 User Memory Protection in `sys_write` and `sys_read`
1. When user code calls `SYS_WRITE(fd, buf, count)` or `SYS_READ(fd, buf, count)`, `buf` is a pointer supplied by Ring 3.
2. Direct kernel dereferencing of `buf` is prohibited:
   - `buf` might point to kernel virtual memory (`>= 0xFFFF800000000000` or higher half).
   - `buf + count` might wrap around 64-bit address space.
   - `buf` might point to unmapped user memory, causing a page fault inside kernel mode.
3. Validation rule:
   - `(uint64_t)buf < USER_SPACE_TOP` (`0x0000800000000000ull`).
   - `(uint64_t)buf + count <= USER_SPACE_TOP`.
   - `(uint64_t)buf + count >= (uint64_t)buf` (no unsigned overflow).
   - Copy in small kernel stack buffers (e.g. 256 bytes) or via `copy_from_user` / `copy_to_user`.

### 2.4 Context Switch Hardening
1. In `kernel/task.c:301-305`:
   Currently, CR3 is only switched if `next->process != NULL && next->process->pml4_physical != 0`.
2. If `next->process == NULL` (or if switching to a pure kernel worker thread whose `process` pointer was unassigned), CR3 remains pointing to the previous process's PML4.
3. While kernel mappings are synchronized across all PML4s, leaving an old user PML4 loaded while executing a kernel thread risks TLB pollution and prevents immediate reclamation of freed PML4s.
4. Hardening: If `next->process == NULL || next->process->pml4_physical == 0`, switch CR3 to `kernel_process.pml4_physical`.

---

## 3. Caveats

1. **SMP / Multi-core Concurrency**:
   KOS currently runs on a single bootstrap CPU in QEMU. The static `tss` in `arch/x86_64/descriptors.c` and a global scratch variable for user RSP work reliably in UP (uniprocessor). If SMP is enabled in future milestones, `tss` and the syscall scratch pointer must be made per-CPU (e.g., using `IA32_KERNEL_GS_BASE` and `swapgs`).
2. **Red Zone & ABI Alignment**:
   In AMD64 ABI, user programs must observe the 128-byte red zone. Because `enter_user_mode` sets RSP to `user_stack_top` formatted by `process_setup_user_stack` (which ends on a 16-byte boundary per System V ABI), user code can safely allocate stack frames.
3. **Hardware `sysret` Bug in Intel Processors**:
   On certain Intel processors, `sysret` to a non-canonical address triggers a General Protection Fault in Ring 0 rather than Ring 3, which can cause a kernel privilege escalation if not guarded. Using `iretq` for syscall return completely avoids this vulnerability.

---

## 4. Conclusion & Technical Implementation Plan

### 4.1 Ring 3 Entry Mechanism: `enter_user_mode`
Replace `enter_userspace` in `arch/x86_64/cpu.asm` with `enter_user_mode` (or provide both):

```nasm
; void enter_user_mode(uint64_t entry_point, uint64_t user_rsp, uint64_t argc, uint64_t argv)
; Parameters (System V AMD64 ABI):
;   rdi: entry_point
;   rsi: user_rsp
;   rdx: argc
;   rcx: argv
global enter_user_mode
global enter_userspace
enter_user_mode:
enter_userspace:
    cli

    ; Save argc and argv into non-clobbered registers before setting up iretq frame
    mov r8, rdx          ; r8 = argc
    mov r9, rcx          ; r9 = argv

    ; Load segment registers with User Data Selector (0x33)
    mov ax, 0x33
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Push 5-quadword hardware frame for iretq
    push qword 0x33      ; SS: User Data (0x33)
    push rsi             ; RSP: user stack pointer
    push qword 0x202     ; RFLAGS: IF=1, bit 1=1, IOPL=0
    push qword 0x2b      ; CS: User Code (0x2B)
    push rdi             ; RIP: user entry point

    ; Set up arguments for user entry point (System V AMD64 ABI)
    mov rdi, r8          ; arg1: argc
    mov rsi, r9          ; arg2: argv

    ; Zero all remaining general-purpose registers to avoid kernel information leakage
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq
```

### 4.2 MSR Setup & Syscall Dispatcher

#### 4.2.1 MSR Primitives
Add to `arch/x86_64/cpu_control.c` and `include/kos/cpu.h`:
```c
static inline uint64_t cpu_read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void cpu_write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
```

#### 4.2.2 MSR Constants & Initialization
```c
#define IA32_EFER_MSR   0xC0000080
#define IA32_STAR_MSR   0xC0000081
#define IA32_LSTAR_MSR  0xC0000082
#define IA32_FMASK_MSR  0xC0000084

#define EFER_SCE        (1ull << 0) /* System Call Extensions */

void syscall_subsystem_init(void) {
    /* 1. Enable SCE bit in EFER */
    uint64_t efer = cpu_read_msr(IA32_EFER_MSR);
    cpu_write_msr(IA32_EFER_MSR, efer | EFER_SCE);

    /* 2. Configure STAR:
     * Bits 47:32: Kernel CS (0x08). Kernel SS is 0x08 + 8 = 0x10.
     * Bits 63:48: User base selector.
     */
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0018 << 48);
    cpu_write_msr(IA32_STAR_MSR, star);

    /* 3. Configure LSTAR: Entry point for 64-bit syscall */
    extern void syscall_entry(void);
    cpu_write_msr(IA32_LSTAR_MSR, (uint64_t)syscall_entry);

    /* 4. Configure FMASK: Mask IF (0x200), TF (0x100), DF (0x400), IOPL (0x3000) */
    cpu_write_msr(IA32_FMASK_MSR, 0x3700ull);
}
```

#### 4.2.3 Syscall Entry Assembly Stub (`arch/x86_64/cpu.asm`)
```nasm
global syscall_entry
extern kos_syscall_dispatch
extern tss_get_rsp0

section .bss
align 16
user_rsp_scratch: resq 1

section .text
align 16
syscall_entry:
    ; Hardware has masked IF, TF, DF. Interrupts are disabled.
    ; RCX = user RIP
    ; R11 = user RFLAGS
    ; RSP = user RSP

    ; 1. Save user RSP to scratch variable
    mov [rel user_rsp_scratch], rsp

    ; 2. Switch to current thread's kernel stack (TSS.rsp0)
    call tss_get_rsp0
    mov rsp, rax

    ; 3. Construct iretq stack frame
    push qword 0x33                  ; SS: User DS
    push qword [rel user_rsp_scratch]; RSP: User RSP
    push r11                         ; RFLAGS: Saved user RFLAGS
    push qword 0x2b                  ; CS: User CS
    push rcx                         ; RIP: Saved user RIP

    ; 4. Save callee/caller registers matching standard ABI
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 5. Prepare C dispatch arguments:
    ; User ABI: rax=num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5, r9=arg6
    ; C signature: kos_syscall_dispatch(num, a1, a2, a3, a4, a5, a6)
    ; C calling:   rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, [rsp]=a6
    sub rsp, 16                      ; 16-byte stack alignment
    mov [rsp], r9                    ; 7th arg (arg6) on stack

    mov r9, r8                       ; 6th arg: arg5
    mov r8, r10                      ; 5th arg: arg4 (was in r10)
    mov rcx, rdx                     ; 4th arg: arg3
    mov rdx, rsi                     ; 3rd arg: arg2
    mov rsi, rdi                     ; 2nd arg: arg1
    mov rdi, rax                     ; 1st arg: syscall number

    cld
    call kos_syscall_dispatch
    add rsp, 16                      ; Clean up stack argument

    ; 6. Return value from C handler is in RAX

    ; 7. Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 8. Reload user segments
    mov dx, 0x33
    mov ds, dx
    mov es, dx

    ; 9. Return cleanly to Ring 3 via iretq
    iretq
```

### 4.3 Syscall Table & Implementations (`kernel/syscall.c`)

Update `include/kos/syscall.h`:
```c
enum kos_syscall_number {
    SYS_EXIT    = 1,
    SYS_WRITE   = 2,
    SYS_READ    = 3,
    SYS_GETTIME = 4,
    SYS_SYSINFO = 5,
    SYS_YIELD   = 6,
    SYS_SLEEP   = 7,
};
```

Implement in `kernel/syscall.c`:
1. `sys_exit(int exit_code)`:
   - Call `process_exit(proc, exit_code)`.
2. `sys_write(int fd, const void *buf, size_t count)`:
   - Validate `(uint64_t)buf + count <= 0x0000800000000000ull`.
   - If `fd == 1 || fd == 2`: copy buffer safely and output to `console_write` / `serial_write`. Return bytes written.
3. `sys_read(int fd, void *buf, size_t count)`:
   - Validate `(uint64_t)buf + count <= 0x0000800000000000ull`.
   - If `fd == 0`: read available keys via `keyboard_read_char()`. Return bytes read.
4. `sys_gettime(void)`:
   - Return `timer_get_uptime_ms()`.
5. `sys_sysinfo(void *info)`:
   - Return system information / task count.
6. `sys_yield(void)`:
   - Call `task_yield()`. Return 0.
7. `sys_sleep(uint64_t ms)`:
   - Call `task_sleep(ms)`. Return 0.

### 4.4 Test Suite Synchronization
In `kernel/command.c`:
Update the 6 test stubs from:
```c
0x48, 0xC7, 0xC0, 0x04, 0x00, 0x00, 0x00, /* mov rax, 4 */
```
to:
```c
0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, /* mov rax, 1 (SYS_EXIT) */
```
This ensures that the test suite matches the new syscall numbering.

---

## 5. Verification Method

### 5.1 Build Verification
Execute host tool build:
```powershell
.\tools\kos-tool.exe build
```
- **Expected result**: Exit code 0, 0 compiler warnings/errors with `-Wall -Wextra -Werror`, output `build/kos.elf` generated cleanly.

### 5.2 Test Execution in QEMU
Run `usertest` via terminal or QEMU script:
- Verify Test 1 passes: Ring 3 ELF execution with `SYS_EXIT(42)`.
- Verify Test 3 & 4 pass: Ring 3 access to kernel addresses (`0xFFFFFFFF80000000`, `0xFFFF800000000000`) and guard page (`0x00007FFFFFFED000`) trigger clean SIGSEGV (-11) exit without kernel panic.
- Verify Test 5 passes: Hardware PIT timer preemption inside Ring 3 compute loop completes and returns cleanly.
- Verify Test 6 passes: 50 iterations show 0 frame leaks and 0 heap leaks.

### 5.3 Invalidation Conditions
- If any syscall from Ring 3 panics the kernel instead of returning an error code.
- If an unhandled user exception in Ring 3 causes a kernel triple fault or kernel crash rather than terminating the faulting process.
- If `MSR_STAR` is configured with `sysret` without adjusting the GDT descriptor ordering (User DS before User CS).
