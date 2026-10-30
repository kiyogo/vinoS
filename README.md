# KOS vinoS

KOS is an independent x86_64 desktop operating system project. Its long-term goal is to build a polished graphical OS in the broad category of macOS, Windows, and desktop Linux. It is an independent project and is not a clone of, nor binary-compatible with, any existing operating system.

---

## Key Highlights

- **Aore AI OS:** A dedicated system designed for AI agents to control and interact with the OS natively (planned future goal).
- **Interface Installer:** The ability to select custom online interfaces or use a default offline interface during installation (planned future goal).

---

## Current Features

### Kernel & Core
- 64-bit monolithic kernel booted via Limine
- Physical Memory Manager (PMM), Virtual Memory Manager (VMM), and dynamic kernel heap
- Symmetric Multiprocessing (SMP) multicore support and preemptive multitasking
- Ring 3 isolation and fast system call path

### Storage & Filesystems
- Drivers for AHCI, NVMe, VirtIO Block, and RAM disk
- Support for MBR, GPT, block caching, Windows volumes, and FAT32

### Networking & Connectivity
- Intel e1000 network driver
- IPv4 network stack including TCP, UDP, DHCP, and DNS
- Support for HTTP, HTTPS, TLS 1.3, and core cryptographic primitives

### System & Tooling
- ELF64 executable loader
- Native package manager
- Interactive debug terminal

---

## Future Goals

- **Aore AI Core Subsystem:** Automation APIs and native intelligence integration.
- **Compatibility Layer:** Support for running Windows applications.
- **Online Desktop Interface Installer:** Dynamic theme and UI downloads during setup.
