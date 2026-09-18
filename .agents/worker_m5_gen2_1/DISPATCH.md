## 2026-09-18T06:57:27Z

<USER_REQUEST>
You are Worker M5 (Gen 2 Replacement) for Milestone K14 in KOS x86_64 OS.

Working directory: d:\DATA\source\OS\KOS\OS2\.agents\worker_m5_gen2_1
All your progress, logs, and handoff must be written to your working directory.

MANDATORY FIRST STEP:
Read the following authoritative specification files before taking any action:
1. `d:\DATA\source\OS\KOS\OS2\.agents\ORIGINAL_REQUEST.md` (specifically Section "Acceptance Criteria")
2. `d:\DATA\source\OS\KOS\OS2\.agents\orchestrator_k14_gen2\PROJECT.md`
3. `d:\DATA\source\OS\KOS\OS2\.agents\teamwork_preview_explorer_survey_3\handoff.md`
4. `d:\DATA\source\OS\KOS\OS2\.agents\worker_m4_gen2\handoff.md`

WRITE OWNERSHIP:
You have exclusive write ownership of:
- `kernel/command.c`
- `include/kos/command.h` (if needed)
- Any test ELF files / ramfs helpers needed for user testing
Do not modify core kernel architecture files (`arch/`, `mm/`, `include/kos/vmm.h`, `include/kos/descriptors.h`) unless strictly required.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

TASK OBJECTIVE:
Implement Milestone M5: Comprehensive Verification & QEMU Tests (Acceptance Criteria verification).

DETAILED REQUIREMENTS:
1. Implement the `usertest` shell command in `kernel/command.c`:
   - Register `"usertest"` in `command_registry` and add it to `command_help`.
   - The command handler `command_usertest(const char *arguments)` must support running individual tests (`usertest valid`, `usertest corrupted`, `usertest kernel_mem`, `usertest stack_overflow`, `usertest preemption`, `usertest leak`) and running all tests when no argument or `"all"` is given.
   - Also, hook `command_usertest("all")` (or an automated boot self-test) during boot or make it easy to run via QEMU automation so the full test suite executes and reports output to serial COM1 log.

2. Test Suite Details (All 6 Tests Required):
   - **Test 1 (`valid`) - Valid ELF64 binary execution**:
     Create or provide a minimal, genuine statically linked ELF64 user binary (or in-memory valid ELF image with entry point that does an exit or `int 0x80` syscall / returns code 42). Load via `process_exec` or child process, execute in Ring 3, verify child terminates with exit code 42, parent process waits via `process_wait()`, retrieves exit code 42, and child is cleanly reaped to `DEAD` with 0 leaks.
   - **Test 2 (`corrupted`) - Corrupted / invalid ELF binary rejection**:
     Attempt to load malformed ELF images (e.g. wrong magic bytes, non-x86_64 machine, invalid entry point outside segment, out-of-bounds `PT_LOAD`). Verify that `elf_load_binary` / `process_exec` returns an error code, cleanly rolls back allocated page tables without kernel panic, and leaks 0 frames.
   - **Test 3 (`kernel_mem`) - Kernel memory isolation enforcement**:
     A Ring 3 user process attempts to access higher-half kernel memory (e.g., dereferencing `0xFFFFFFFF80000000` or `0xFFFF800000000000`). Verify that hardware paging triggers a user page fault, `page_fault_handler()` catches it (`(cs & 3) == 3`), logs the violation, terminates the process with SIGSEGV (-11), and the kernel scheduler continues running normally without any kernel panic or halt.
   - **Test 4 (`stack_overflow`) - User stack guard page containment**:
     A Ring 3 user process accesses or recurses into the unmapped guard page at the bottom of the user stack (`0x00007FFFFFFED000ull`). Verify that hardware paging triggers a page fault on the guard page address, the kernel detects user fault, terminates the process with SIGSEGV (-11), and the kernel remains fully stable without crashing.
   - **Test 5 (`preemption`) - TSS/RSP0 Ring 3 hardware preemption**:
     A Ring 3 user process runs an execution loop. A hardware timer interrupt (PIT 1000 Hz) fires while CPL=3. Verify that the CPU safely switches stack to the thread's kernel stack via TSS RSP0, handles the timer tick, reschedules/preempts, and resumes without kernel corruption.
   - **Test 6 (`leak`) - 50-iteration zero memory leak test**:
     Loop 50 times: create process, allocate address space, load ELF/stack, exit, wait, and reap. Measure `pmm_free_frame_count()` and heap tracked bytes before and after the 50 iterations. Verify that baseline PMM free frames and heap allocations are 100% preserved (0 frame leaks, 0 heap leaks).

3. BUILD & LIVE QEMU EXECUTION:
   - Compile using `.\tools\kos-tool.exe build`. Ensure 0 compiler warnings and 0 compiler errors.
   - Execute QEMU headless, run `usertest`, and capture serial output.
   - Confirm all 6 tests report `[PASS]` in the serial log and the system reaches `KOS debug terminal ready. KOS> `.

4. REPORT:
   - Write full findings, test code diffs, command outputs, and serial execution logs to `d:\DATA\source\OS\KOS\OS2\.agents\worker_m5_gen2_1\handoff.md`.
   - Send a message to caller with a summary of changes and verification results.
</USER_REQUEST>
