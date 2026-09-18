## 2026-09-18T09:36:28Z
You are the Ring 3 Privilege Transition & Syscall ABI Explorer (explorer_survey_2) for Milestone K13 & K14 in KOS x86_64.
Your working directory is: D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_2
The project root is: D:\DATA\source\OS\KOS\OS2
The authoritative user request is in: D:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md (read section ## Follow-up — 2026-09-18T09:30:36Z).
You must read ORIGINAL_REQUEST.md before starting work.

Mission:
Investigate KOS's current CPU state management, GDT, TSS, IDT, interrupt/exception handling, context switching, and syscall dispatching to provide an authoritative technical survey for Requirements R3 and R4.

Specific Tasks:
1. Examine existing GDT and TSS initialization in KOS (e.g. kernel/arch/x86_64/gdt.c, tss.c, etc.):
   - Current GDT descriptor layout: kernel code (0x08), kernel data (0x10), TSS (0x18/0x20).
   - Required descriptors for User Mode:
     - User Code 64-bit: Selector 0x2B (index 5, RPL 3, DPL 3, L=1, D=0).
     - User Data: Selector 0x33 (index 6, RPL 3, DPL 3).
     - Compatibility mode if any (or 32-bit user code).
     - Verify layout compatibility with MSR_STAR for sysret (sysret expects User Code/Data selectors in specific relationship to STAR[63:48]).
2. Examine TSS and Kernel Stack handling:
   - Where is TSS allocated and loaded (ltr)?
   - How is TSS.rsp0 set up and updated during context switches or thread creation?
   - Are Interrupt Stack Tables (IST) used for Double Fault (#DF), NMI, Page Fault (#PF)?
3. Examine Ring 3 Entry mechanism:
   - Implementation of enter_user_mode(uint64_t entry_point, uint64_t user_rsp, uint64_t argc, uint64_t argv):
   - Setting up iretq stack frame: User SS (0x33), User RSP, RFLAGS (IF=1 bit 9, IOPL=0), User CS (0x2B), RIP (entry_point).
   - Clearing/setting segment registers (DS, ES, FS, GS) and general purpose registers.
4. Examine Context Switching and Thread/Process structures:
   - Look at kernel/task.c, kernel/thread.c, kernel/sched.c, or equivalent in KOS.
   - How does context switch save/restore registers?
   - How should CR3 be switched when switching between processes with different address spaces?
   - How is TSS.rsp0 updated to point to the new process/thread kernel stack?
5. Examine Syscall entry mechanism & ABI:
   - MSR setup: IA32_STAR (0xC0000081), IA32_LSTAR (0xC0000082), IA32_FMASK (0xC0000084, masking IF, TF, etc.), IA32_EFER.SCE (bit 0).
   - Syscall entry assembly (syscall_entry): swapgs (if GS base used), saving user registers, switching to kernel stack, calling C handler syscall_handler(regs).
   - Sysret return assembly: restoring registers, swapgs, sysretq.
   - Software interrupt int 0x80: IDT descriptor with DPL 3 so Ring 3 can execute int 0x80.
   - Core syscall numbers: SYS_EXIT=1, SYS_WRITE=2, SYS_READ=3, SYS_GETTIME=4, SYS_SYSINFO=5, SYS_YIELD=6, SYS_SLEEP=7.
   - Calling convention: System V AMD64 syscall convention (rax=syscall_num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5, r9=arg6; return in rax).
6. Examine Exception Handling for User Mode:
   - Check IDT handlers for Page Fault (#PF) and General Protection Fault (#GP).
   - How does the exception handler detect if the fault occurred in Ring 3 (CS & 3 == 3)?
   - Ensure user faults result in clean process termination with diagnostic log, rather than kernel panic.
7. Document all findings, exact assembly stubs needed, register conventions, and step-by-step implementation plan.
Write your complete report to: D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_2\handoff.md.
Update progress.md as you work.
When finished, send a message back with your summary and handoff path.
