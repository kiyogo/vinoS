## 2026-09-18T08:15:55Z
Investigate the existing codebase of KOS (d:/DATA/source/OS/KOS/OS2) focusing on Requirement R4 & Verification:
1. Socket Subsystem:
   - What socket data structures currently exist? Is there a `struct socket`?
   - How is socket lifecycle managed (refcount, creation, closing, cleanup)?
   - Are standard operations implemented (`socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `shutdown()`, `getsockopt()`, `setsockopt()`)?
   - Are blocking and non-blocking modes supported? How are timeouts handled?
   - Event notification and Poll: Is there a wait queue or poll mechanism (poll/select readiness notification) or does it busy-wait?
   - Concurrency & Thread safety: How are sockets synchronized (spinlocks, mutexes)?
   - Internal API for syscalls: How will this subsystem be exposed to the syscall layer (Person 5) cleanly without modifying their dispatcher?
2. Existing Shell Commands & Network Tests:
   - What network commands currently exist in the kernel shell? (`netinfo`, `ping`, `udptest`, TCP echo test client/server)?
   - How does QEMU networking run (QEMU startup scripts, flags, e1000 parameters)?
