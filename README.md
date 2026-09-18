# KOS vinoS

KOS is an independent x86_64 desktop operating-system project. Its long-term goal is a polished graphical OS in the broad category of macOS, Windows, and desktop Linux; it is not a clone of, nor binary-compatible with, any of them.

## Key Highlights

- Aore AI OS: Dedicated system architecture for AI agents to run and control the operating system natively, near future goal.
- Interface Installer: Choose custom desktop environments downloaded from the network or use the default offline interface during installation, near future goal.

## Current Features

- 64-bit monolithic kernel on x86_64 with Limine bootloader
- PMM, VMM, and dynamic kernel heap memory management
- SMP multicore support and preemptive thread multitasking
- Hardware interrupts, Ring 3 user mode isolation, and fast syscall path
- AHCI SATA, NVMe, VirtIO Block, and RAM disk drivers
- MBR and GPT partition support with block cache
- Windows style volume manager and FAT32 filesystem
- Intel e1000 network driver and IPv4 network stack with TCP, UDP, DHCP, DNS
- HTTP, HTTPS, TLS 1.3, and cryptography support
- ELF64 binary loader, package manager, and kernel terminal

## Future Goals

- Aore AI core subsystem and agent automation APIs
- Windows application compatibility layer
- Graphical desktop environment and window manager
- Online desktop interface installer
- Native web browser engine

## Target

- Architecture: x86_64
- Firmware: UEFI
- Bootloader: Limine
- Emulator: QEMU

## Build and Run

```sh
clang -std=c17 -O2 -Wall -Wextra -Werror tools/kos.c -o tools/kos-tool.exe
tools\kos-tool.exe bootstrap-limine
tools\kos-tool.exe bootstrap-assets
tools\kos-tool.exe build
tools\kos-tool.exe run
```

## Documentation

- [Scope and Roadmap](docs/roadmap.md)
- [Product Vision](docs/product-vision.md)
- [VFS Volumes](docs/vfs-volumes.md)
- [Networking Roadmap](docs/networking-roadmap.md)
- [Architecture](docs/architecture.md)
- [Boot Process](docs/boot-process.md)
- [Environment Setup](docs/environment.md)

