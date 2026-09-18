# Dispatch: Explorer Survey 3 (Process/Thread Lifecycle, Scheduler, Memory Reclamation & Tests)

## 2026-09-18T00:24:07Z
Investigate KOS x86_64 Process & Thread subsystem, scheduler integration, resource cleanup/reclamation, and verification harness for Milestone K14:
1. Existing Process/Thread Model: Check kernel/sched.c, kernel/thread.c, kernel/process.c, include/kos/sched.h, include/kos/thread.h, include/kos/process.h. What fields exist in thread_t and process_t? What states are supported (READY, RUNNING, BLOCKED, ZOMBIE, DEAD)?
2. Process/Thread Hierarchy & Lifecycle: How are parent-child relationships tracked? What are the current implementations of process_create, process_exec, process_exit, process_wait, thread_create, thread_exit, thread_sleep, yield?
3. Resource Reclamation: How does a process clean up upon termination? How are page tables freed without leaking physical frames (PMM)? How are kernel stacks, user stacks, handle tables, and open objects reclaimed? How does zombie reaping work?
4. Scheduler Integration: How does the preemptive scheduler handle user threads vs kernel threads? How is CR3 switched when switching between processes?
5. Signals / Synchronization Events: What event or notification primitives exist for IPC/synchronization?
6. Test Harness & Build Toolchain: How does tools\kos-tool.exe build work? How are QEMU tests run? What shell commands currently test process/task subsystems? How can we test user-mode ELF execution, fault handling, guard pages, invalid ELF handling, and leak testing in QEMU?
7. Recommendations and required architectural changes for R3 and Acceptance Criteria.
