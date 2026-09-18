## 2026-09-18T09:36:00Z
Mission:
Investigate KOS's current ELF loader (fs/elf.c or equivalent), filesystem/initrd, user stack allocation, build system (tools\kos-tool.exe), and user-space test binary creation for Requirements R2 and R5.

Specific Tasks:
1. Examine existing ELF loader code in KOS (e.g. kernel/fs/elf.c, kernel/include/kernel/elf.h, or wherever ELF handling exists).
   - What ELF structures currently exist (Elf64_Ehdr, Elf64_Phdr, etc.)?
   - What validation is done (magic \x7fELF, EI_CLASS == ELFCLASS64, EI_DATA == ELFDATA2LSB, e_type == ET_EXEC / ET_DYN, e_machine == EM_X86_64)?
   - How are PT_LOAD segments currently parsed and loaded?
   - How are memory permissions mapped (PF_R, PF_W, PF_X -> page table flags)?
   - How are p_filesz vs p_memsz handled (zeroing .bss)?
2. Analyze User Process Memory Layout & User Stack:
   - User address space boundary: 0x00000000_00000000 .. 0x00007FFF_FFFFFFFF.
   - Recommended user stack location (e.g. stack top at 0x00007FFF_FFFF0000 or similar, size e.g. 64 KiB, with unmapped guard page beneath it).
   - Stack layout setup per System V AMD64 ABI:
     - 16-byte stack alignment requirements.
     - Pushing string values of argv and envp onto stack.
     - Setting up argv pointers array (NULL terminated), envp pointers array (NULL terminated), and argc on stack.
     - Setting initial RSP so user entry point can read argc in rdi, argv in rsi, or from stack according to ABI.
3. Examine the Build System & Toolchain:
   - How does tools\kos-tool.exe build the kernel? Check build.zig, Makefile, CMakeLists.txt, or whatever tool configuration is used.
   - How can a user-space freestanding ELF64 binary be compiled and included in KOS (e.g. an embedded C or assembly user binary, or built as a separate ELF file packed into initrd/ramfs or embedded via incbin/object)?
   - Can we build a user-space C test program with a minimal crt0 / syscall library?
4. Examine Shell & Test Integration:
   - Where are shell commands defined in KOS (e.g. shell.c or cli.c)?
   - How to add an exec or usertest command that loads and runs the user ELF binary in Ring 3.
   - How to test deliberate faults (e.g., dereferencing NULL or executing privileged instructions in Ring 3) to verify clean termination.
   - How to verify CPL 3 privilege level and 0 memory leaks upon process exit.
5. Document all findings, exact struct layouts, build instructions, and step-by-step implementation plan.
Write your complete report to: D:\DATA\source\OS\KOS\OS2\.agents\explorer_survey_3\handoff.md.
