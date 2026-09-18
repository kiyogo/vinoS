# KOS Development Environment

## Prerequisites

- Host Compiler: LLVM Clang with C17 support and LLD linker.
- Assembler: NASM 2.15 or newer.
- Emulator: QEMU x86_64 emulator with Q35 machine support and EDK2 OVMF UEFI firmware.
- Python: Python 3 for test automation and CI test execution.

## Build and Test Tools

- kos-tool: C command line development utility located in tools/kos.c.
- ci_runner: Headless CI test runner located in tools/ci_runner.py.
