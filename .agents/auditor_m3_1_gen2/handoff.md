# Forensic Integrity Audit Report: Milestone M3 — Ring 3 Transition & Fault Recovery (Gen 2)

**Auditor Agent**: `auditor_m3_1_gen2`  
**Working Directory**: `d:\DATA\source\OS\KOS\OS2\.agents\auditor_m3_1_gen2`  
**Target Milestone**: K14 Milestone M3 (Requirement R2: Ring 3 Privilege Transition & Hardware Context Management)  
**Parent Agent**: `c843668f-19e3-4d81-abc8-ea4359b7dde0` (`parent`)  
**Audit Profile**: General Project / Development Mode  
**Final Verdict**: **CLEAN**

---

## 1. Observation

Direct empirical evidence was gathered across the codebase, compiler toolchain, binary disassembly, and QEMU execution logs.

### 1.1 Static Analysis & Git Diff Inspection
Target files for Milestone M3:
- `arch/x86_64/cpu.asm`
- `include/kos/cpu.h`
- `arch/x86_64/interrupts.c`

Verbatim git diff observed:
```diff
diff --git a/arch/x86_64/cpu.asm b/arch/x86_64/cpu.asm
index bfe71fd..da61e1b 100644
--- a/arch/x86_64/cpu.asm
+++ b/arch/x86_64/cpu.asm
@@ -31,3 +32,49 @@ tss_load:
     mov ax, di
     ltr ax
     ret
+
+; void enter_userspace(uint64_t entry_point, uint64_t user_rsp) __attribute__((noreturn))
+; Arguments:
+;   rdi: entry_point (virtual RIP to execute in user mode)
+;   rsi: user_rsp    (virtual RSP user stack pointer)
+enter_userspace:
+    ; Disable interrupts while preparing the iretq stack frame
+    cli
+
+    ; Load data segment registers (DS, ES, FS, GS) with 0x33 (GDT_USER_DS)
+    mov ax, 0x33
+    mov ds, ax
+    mov es, ax
+    mov fs, ax
+    mov gs, ax
+
+    ; Push 5 quadwords for iretq:
+    ;   [rsp + 32] SS:     0x33 (GDT_USER_DS)
+    ;   [rsp + 24] RSP:    rsi  (user_rsp)
+    ;   [rsp + 16] RFLAGS: 0x202 (IF=1, bit 1=1)
+    ;   [rsp + 8]  CS:     0x2b (GDT_USER_CS)
+    ;   [rsp + 0]  RIP:    rdi  (entry_point)
+    push qword 0x33
+    push rsi
+    push qword 0x202
+    push qword 0x2b
+    push rdi
+
+    ; Zero all general-purpose registers to prevent kernel information leakage
+    xor eax, eax
+    xor ebx, ebx
+    xor ecx, ecx
+    xor edx, edx
+    xor esi, esi
+    xor edi, edi
+    xor ebp, ebp
+    xor r8, r8
+    xor r9, r9
+    xor r10, r10
+    xor r11, r11
+    xor r12, r12
+    xor r13, r13
+    xor r14, r14
+    xor r15, r15
+
+    iretq

diff --git a/include/kos/cpu.h b/include/kos/cpu.h
index a7450f3..f67f79b 100644
--- a/include/kos/cpu.h
+++ b/include/kos/cpu.h
@@ -21,6 +21,7 @@ uint64_t tss_get_ist1(void);
 KOS_NORETURN void cpu_halt(void);
 KOS_NORETURN void cpu_idle(void);
 KOS_NORETURN void cpu_reboot(void);
+KOS_NORETURN void enter_userspace(uint64_t entry_point, uint64_t user_rsp);

diff --git a/arch/x86_64/interrupts.c b/arch/x86_64/interrupts.c
index 4591480..a7c40f4 100644
--- a/arch/x86_64/interrupts.c
+++ b/arch/x86_64/interrupts.c
@@ -252,20 +253,34 @@ void page_fault_handler(struct interrupt_context *context) {
     s_last_page_fault_record = record;
 
     /* 4. Fault discrimination: Kernel mode vs User mode */
-    if (!(context->error_code & PF_ERROR_USER)) {
+    bool is_user = ((context->cs & 3) == 3) || ((context->error_code & 4) != 0);
+    if (!is_user) {
         /* Kernel mode page fault is unrecoverable -> Panic & Halt */
         log_page_fault_panic(context, &record);
     } else {
         /* User mode page fault (Ring 3) */
-        cpu_halt();
+        __atomic_sub_fetch(&s_pf_recursion, 1, __ATOMIC_SEQ_CST);
+
+        log_warnf("User process page fault: CR2=0x%016lX, RIP=0x%016lX, RSP=0x%016lX, error=0x%lX [P=%u W=%u U=%u R=%u I=%u] (%s)",
+                  (unsigned long)cr2,
+                  (unsigned long)context->rip,
+                  (unsigned long)context->rsp,
+                  (unsigned long)context->error_code,
+                  (unsigned int)(record.present ? 1 : 0),
+                  (unsigned int)(record.write ? 1 : 0),
+                  (unsigned int)(record.user ? 1 : 0),
+                  (unsigned int)(record.reserved_bit ? 1 : 0),
+                  (unsigned int)(record.instruction_fetch ? 1 : 0),
+                  record.violation_cause ? record.violation_cause : "access violation");
+
+        struct process *cur = process_get_current();
+        if (cur != NULL) {
+            log_warnf("Terminating user process PID %lu (%s) due to page fault (SIGSEGV -11)",
+                      (unsigned long)cur->pid, cur->name);
+            process_exit(cur, -11);
+            schedule();
+        }
+        schedule();
     }
 }
```

### 1.2 Binary Symbol Verification (`llvm-nm`)
Command executed:
```pwsh
& "C:/Program Files/LLVM/bin/llvm-nm.exe" build/kos.elf | Select-String "enter_userspace"
```
Tool output:
```
ffffffff8001ec39 T enter_userspace
```

### 1.3 Machine Code Disassembly Verification (`llvm-objdump`)
Command executed:
```pwsh
& "C:/Program Files/LLVM/bin/llvm-objdump.exe" -d --disassemble-symbols=enter_userspace build/kos.elf
```
Tool output:
```
build/kos.elf:	file format elf64-x86-64

Disassembly of section .text:

ffffffff8001ec39 <enter_userspace>:
ffffffff8001ec39: fa                   	cli
ffffffff8001ec3a: 66 b8 33 00          	movw	$0x33, %ax
ffffffff8001ec3e: 8e d8                	movl	%eax, %ds
ffffffff8001ec40: 8e c0                	movl	%eax, %es
ffffffff8001ec42: 8e e0                	movl	%eax, %fs
ffffffff8001ec44: 8e e8                	movl	%eax, %gs
ffffffff8001ec46: 6a 33                	pushq	$0x33
ffffffff8001ec48: 56                   	pushq	%rsi
ffffffff8001ec49: 68 02 02 00 00       	pushq	$0x202                  # imm = 0x202
ffffffff8001ec4e: 6a 2b                	pushq	$0x2b
ffffffff8001ec50: 57                   	pushq	%rdi
ffffffff8001ec51: 31 c0                	xorl	%eax, %eax
ffffffff8001ec53: 31 db                	xorl	%ebx, %ebx
ffffffff8001ec55: 31 c9                	xorl	%ecx, %ecx
ffffffff8001ec57: 31 d2                	xorl	%edx, %edx
ffffffff8001ec59: 31 f6                	xorl	%esi, %esi
ffffffff8001ec5b: 31 ff                	xorl	%edi, %edi
ffffffff8001ec5d: 31 ed                	xorl	%ebp, %ebp
ffffffff8001ec5f: 4d 31 c0             	xorq	%r8, %r8
ffffffff8001ec62: 4d 31 c9             	xorq	%r9, %r9
ffffffff8001ec65: 4d 31 d2             	xorq	%r10, %r10
ffffffff8001ec68: 4d 31 db             	xorq	%r11, %r11
ffffffff8001ec6b: 4d 31 e4             	xorq	%r12, %r12
ffffffff8001ec6e: 4d 31 ed             	xorq	%r13, %r13
ffffffff8001ec71: 4d 31 f6             	xorq	%r14, %r14
ffffffff8001ec74: 4d 31 ff             	xorq	%r15, %r15
ffffffff8001ec77: 48 cf                	iretq
```

### 1.4 Fault Dispatcher Machine Code Disassembly
Disassembly of `page_fault_handler` at `0xffffffff80001494` in `build/kos.elf`:
```
ffffffff80001494: 41 f6 c7 04          	testb	$0x4, %r15b
ffffffff80001498: 75 13                	jne	0xffffffff800014ad <page_fault_handler+0xcd>
ffffffff8000149a: 48 8b 83 90 00 00 00 	movq	0x90(%rbx), %rax
ffffffff800014a1: 83 e0 03             	andl	$0x3, %eax
ffffffff800014a4: 83 f8 03             	cmpl	$0x3, %eax
ffffffff800014a7: 0f 85 33 01 00 00    	jne	0xffffffff800015e0 <page_fault_handler+0x200>
ffffffff800014ad: f0 ff 0d 5c 03 03 00 	lock decl 0x3035c(%rip)  # dec s_pf_recursion
...
ffffffff80001518: e8 63 cd 00 00       	callq	0xffffffff8000e280 <process_get_current>
ffffffff80001542: 48 89 df             	movq	%rbx, %rdi
ffffffff80001545: be f5 ff ff ff       	movl	$0xfffffff5, %esi       # imm = -11 (SIGSEGV)
ffffffff8000154a: e8 41 d3 00 00       	callq	0xffffffff8000e890 <process_exit>
ffffffff8000154f: e8 4c e7 00 00       	callq	0xffffffff8000fca0 <schedule>
```
Kernel mode faults jump to `0xffffffff800015e0` which executes `log_enter_emergency_mode`, `log_emergency_puts`, and `panic_with_context`.

Disassembly of `interrupt_dispatch` at `0xffffffff80001a89`:
```
ffffffff80001a89: 48 8b 83 90 00 00 00 	movq	0x90(%rbx), %rax
ffffffff80001a90: 89 c1                	movl	%eax, %ecx
ffffffff80001a92: f7 d1                	notl	%ecx
ffffffff80001a94: f6 c1 03             	testb	$0x3, %cl
ffffffff80001a97: 0f 85 5f ff ff ff    	jne	...
```
Tests `(cs & 3) == 3`. If user mode: logs register dump, invokes `process_get_current()`, `process_exit(cur, -1)`, and `schedule()`. If kernel mode: executes `panic_with_context()`.

### 1.5 QEMU Boot Verification
Boot log snippet:
```
[    1.720] [TID 1] [INFO ] [waittest] 5. Process Wait & Automatic Resource Cleanup (process_wait)...
[    1.720] [TID 1] [INFO ]   Child process PID 6 created, handle 0x00010000 created, object refcount=2
[    1.750] [TID 1] [INFO ]   process_wait returned 1 in 20 ms: exit_code=77 (expect 77), child state=ZOMBIE
[    1.780] [TID 1] [INFO ]   [PASS] Process termination resource cleanup verified!
[    1.790] [TID 1] [INFO ] >>> [PASS] ALL WAIT QUEUE & RESOURCE CLEANUP TESTS PASSED! <<<
[    1.960] [TID 1] [INFO ] >>> [PASS] ALL SUBSYSTEM SYNCHRONIZATION TESTS PASSED! <<<
[    1.970] [TID 1] [INFO ] KOS debug terminal ready.
KOS> 
```
System boots cleanly with 0 regressions.

---

## 2. Logic Chain

1. **Absence of Hardcoded/Dummy Constructs**:
   - Inspection of `cpu.asm` and `interrupts.c` reveals no fake returns, stubbed responses, or artificial test passes.
   - The changes are substantive additions to assembly routines and C exception handlers directly serving Milestone M3.

2. **Authenticity of Privilege Transition**:
   - `enter_userspace` implements the standard AMD64 architecture transition to Ring 3.
   - It sets GDT data segments (DS, ES, FS, GS) to `0x33` (`GDT_USER_DS`, GDT index 6 with RPL=3).
   - It constructs a 5-quadword hardware interrupt return frame on the stack:
     - `SS`: `0x33` (`GDT_USER_DS`)
     - `RSP`: `rsi` (`user_rsp`)
     - `RFLAGS`: `0x202` (bit 1 reserved, bit 9 IF=1 to enable interrupts in user mode)
     - `CS`: `0x2b` (`GDT_USER_CS`, GDT index 5 with RPL=3)
     - `RIP`: `rdi` (`entry_point`)
   - `iretq` restores these registers in an atomic hardware transaction, switching CPL from 0 to 3.

3. **Register Sanitization**:
   - In AMD64, writing to 32-bit registers (e.g. `xor eax, eax`) zeroes the entire 64-bit register (`RAX`), saving code bytes.
   - Registers `RAX`, `RBX`, `RCX`, `RDX`, `RSI`, `RDI`, `RBP`, `R8`, `R9`, `R10`, `R11`, `R12`, `R13`, `R14`, and `R15` are all zeroed.
   - Register `RSP` is loaded directly by `iretq` with the target `user_rsp`.
   - All 16 general-purpose registers are thus in a controlled state, preventing kernel memory addresses or stack contents from leaking to Ring 3.

4. **Fault Isolation vs Kernel Panic**:
   - User page faults trigger `PF_ERROR_USER` (bit 2 of error code) and have `(context->cs & 3) == 3`.
   - In `page_fault_handler`, checking `((context->cs & 3) == 3) || ((context->error_code & 4) != 0)` accurately isolates user faults from kernel faults.
   - On user faults, `s_pf_recursion` is decremented, diagnostic logs are recorded, and `process_exit(cur, -11)` (SIGSEGV) is invoked followed by `schedule()`.
   - On kernel faults, `log_page_fault_panic()` is invoked, calling `panic_with_context()` and halting the system, preserving kernel crash detection.
   - Similarly, in `interrupt_dispatch` and `default_exception_panic()`, unhandled user exceptions are routed to `process_exit(cur, -1)` and `schedule()`, while kernel exceptions panic.

5. **Binary & Build Authenticity**:
   - The build tool `tools\kos-tool.exe build` compiles `arch/x86_64/cpu.asm` using NASM and links it into `build/kos.elf`.
   - The symbol `enter_userspace` is confirmed in the ELF symbol table at `0xffffffff8001ec39`.
   - Machine instructions in the ELF match the NASM source line-for-line.

---

## 3. Caveats

1. **Userspace Signal Handling**: Milestone M3 routes all unhandled exceptions and page faults in user mode directly to process termination (`process_exit`). Delivery of signals to in-process user signal handlers is not part of M3 and will be addressed in future milestones.
2. **SMP TSS Scaling**: The current TSS and GDT configuration is uniprocessor-scoped. Per-CPU TSS instances will be required if multicore SMP is introduced.

---

## 4. Conclusion

Milestone M3 implementation satisfies all integrity, architectural, and security requirements:
- `enter_userspace` in `arch/x86_64/cpu.asm` correctly implements hardware context switching via `iretq` with RPL=3 selectors (`0x2b`, `0x33`), RFLAGS (`0x202`), and complete 15-register scrubbing.
- `include/kos/cpu.h` contains the accurate `KOS_NORETURN` declaration.
- `arch/x86_64/interrupts.c` genuinely isolates user page faults and exceptions, routing them to `process_exit()` and `schedule()` while preserving kernel panic for kernel faults.
- Build and execution verification confirms genuine compilation and zero regressions.

**Final Verdict: CLEAN** (No integrity violations detected).

---

## 5. Verification Method

To independently reproduce and verify this audit:

1. **Verify Build**:
   ```pwsh
   .\tools\kos-tool.exe build
   ```
   *Expected: Exit code 0, 0 compiler warnings/errors, embedded symbols generated in `build/kos.elf`.*

2. **Verify Embedded Symbol**:
   ```pwsh
   & "C:/Program Files/LLVM/bin/llvm-nm.exe" build/kos.elf | Select-String "enter_userspace"
   ```
   *Expected: Symbol `enter_userspace` present with address `ffffffff8001ec39 T`.*

3. **Verify Disassembly of `enter_userspace`**:
   ```pwsh
   & "C:/Program Files/LLVM/bin/llvm-objdump.exe" -d --disassemble-symbols=enter_userspace build/kos.elf
   ```
   *Expected: Instructions `cli`, `movw $0x33, %ax`, segment loading, 5 pushes (`0x33`, `%rsi`, `$0x202`, `$0x2b`, `%rdi`), 15 xor register clears, and `iretq`.*

4. **Verify Disassembly of Fault Discrimination**:
   ```pwsh
   & "C:/Program Files/LLVM/bin/llvm-objdump.exe" -d --start-address=0xffffffff80001490 --stop-address=0xffffffff80001555 build/kos.elf
   ```
   *Expected: `testb $0x4, %r15b`, `cmpl $0x3, %eax`, `callq process_exit`, `callq schedule`.*

5. **Verify QEMU Live Boot**:
   ```pwsh
   $proc = Start-Process -FilePath "C:/Program Files/qemu/qemu-system-x86_64.exe" -ArgumentList "-machine q35 -cpu max -m 256 -drive if=pflash,format=raw,readonly=on,file=""C:/Program Files/qemu/share/edk2-x86_64-code.fd"" -drive if=pflash,format=raw,file=build/edk2-vars.fd -drive format=raw,file=fat:rw:build/esp -serial file:qemu_serial.log -monitor none -no-reboot -display none" -PassThru; Start-Sleep -Seconds 7; Stop-Process -Id $proc.Id -Force; Get-Content qemu_serial.log -Tail 30; Remove-Item qemu_serial.log -Force
   ```
   *Expected: All self-tests pass, reaching `KOS debug terminal ready.`.*
