# Contributing to vinoS (KOS)

Thank you for your interest in contributing to **vinoS**! As an independent x86_64 desktop operating system project, every contribution—from bug reports and documentation to kernel drivers and GUI subsystems—is deeply appreciated.

---

## Code of Conduct

All contributors are expected to uphold our [Code of Conduct](CODE_OF_CONDUCT.md). Please read it before participating.

---

## How Can I Contribute?

### 1. Reporting Bugs
- Check the [GitHub Issues](https://github.com/kiyogo/vinoS/issues) to see if the problem has already been reported.
- If not, open a new issue using our **Bug Report Template**.
- Include your host OS, QEMU version, Clang version, serial/debug log output, and steps to reproduce.

### 2. Suggesting Enhancements
- Feature requests and architecture proposals are welcome!
- Open an issue using the **Feature Request Template** outlining the motivation, detailed description, and potential API design.

### 3. Pull Requests
- Fork the repository and create a branch from `main`:
  ```bash
  git checkout -b feat/my-new-feature
  ```
- Keep commits atomic and write descriptive commit messages (following [Conventional Commits](https://www.conventionalcommits.org/)):
  - `feat(subsystem): add new functionality`
  - `fix(driver): resolve race condition in AHCI interrupt handler`
  - `docs: update build prerequisites`
  - `test(gui): add framebuffer blur regression test`
- Make sure all QA gates pass before submitting a PR:
  ```bash
  python tools/ci_runner.py smoke
  python tools/storage_ci.py
  ```

---

## Development & Kernel Coding Standards

- **Standard**: C17 (`-std=c17`).
- **Freestanding Environment**: The kernel compiles with `-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-80387`. Do not use standard C library headers (`<stdio.h>`, `<stdlib.h>`); use kernel-provided primitives under `include/` and `lib/`.
- **Formatting**:
  - 4 spaces indentation (no tabs in C source files).
  - Clear variable and function names with subsystem prefixes (e.g., `framebuffer_`, `vmm_`, `pmm_`, `net_`, `fat32_`).
  - Document public header contracts in `include/`.
- **Warnings as Errors**: The build is configured with `-Wall -Wextra -Werror`. Code must compile cleanly without any warnings.
- **Ring 3 Isolation**: Keep user space and kernel space strictly separated. User programs must interact exclusively through documented system calls.

---

## Testing Your Changes

Before submitting your Pull Request, run the local smoke and validation test harnesses:

1. **Smoke Test**:
   ```bash
   python tools/ci_runner.py smoke
   ```
2. **Storage Subsystem Integration**:
   ```bash
   python tools/storage_ci.py
   ```
3. **Interactive GUI / Desktop Mode**:
   ```bash
   python tools/ci_runner.py gui
   ```

Thank you for helping make vinoS an extraordinary desktop operating system!
