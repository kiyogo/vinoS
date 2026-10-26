# vinoS (KOS)

<div align="center">

### Modern 64-Bit Independent x86_64 Desktop Operating System

[![CI](https://github.com/kiyogo/vinoS/actions/workflows/ci.yml/badge.svg)](https://github.com/kiyogo/vinoS/actions)
![Architecture](https://img.shields.io/badge/Architecture-x86__64-007acc.svg?style=flat-square)
![Bootloader](https://img.shields.io/badge/Bootloader-Limine%20UEFI-ff6b6b.svg?style=flat-square)
![Language](https://img.shields.io/badge/Language-C17%20%7C%20ASM%20%7C%20Python-brightgreen.svg?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-blue.svg?style=flat-square)
![Status](https://img.shields.io/badge/Status-Active%20Development-success.svg?style=flat-square)

[English](#english) • [Tiếng Việt](#tiếng-việt) • [Architecture](#system-architecture) • [Getting Started](#getting-started) • [QA & CI](#qa--ci-verification)

---

</div>

<a name="english"></a>
## Overview

**vinoS** (formerly known under the KOS project lineage) is an independent, clean-room 64-bit desktop operating system targeting modern `x86_64` hardware.

Its long-term mission is to deliver a fast, responsive, and visually sophisticated graphical desktop experience in the category of modern operating systems (such as macOS and Windows), without being a binary clone or fork of any existing ecosystem. Built completely from the bare metal up, vinoS features a freestanding monolithic kernel, symmetric multiprocessing (SMP), an acrylic glass compositor, a full TCP/IP networking stack, and native package management.

### Key Highlights

- **Aore AI OS Core**: Dedicated architectural subsystem designed from the kernel up for native autonomous AI agent control, structured system observation, and headless automation.
- **Dynamic Interface Installer**: Modular desktop ecosystem supporting both offline default environments and remote package repository installations via KPKG.
- **Glass Acrylic Compositor**: High-performance linear framebuffer GUI engine with real-time blur caching, double buffering, and smooth cursor overlay integrity.
- **Clean Ring 3 Separation**: Hardware-enforced privilege isolation via `IA32_LSTAR` fast syscall dispatch, 4-level paging, and protected user-space runtimes.

---

<a name="system-architecture"></a>
## System Architecture

```
+--------------------------------------------------------------------------+
|                        USER SPACE (Ring 3)                               |
|   +-----------------------+   +-------------------+   +--------------+   |
|   |  Default Glass GUI    |   | Terminal / Shell  |   |  User Apps   |   |
|   +-----------------------+   +-------------------+   +--------------+   |
|   |  vino GUI Runtime     |   |  Package (KPKG)   |   | vino libc    |   |
+---+-----------------------+---+-------------------+---+--------------+---+
                                | System Calls (IA32_LSTAR)
+-------------------------------v------------------------------------------+
|                        KERNEL SPACE (Ring 0)                             |
|  +--------------------------------------------------------------------+  |
|  | Process & Task Scheduler (SMP Multicore, APIC Timer Preemption)    |  |
|  +--------------------------------------------------------------------+  |
|  | Memory Management: PMM (Bitmap/Buddy) | VMM (4-Level Paging) | Heap |  |
|  +--------------------------------------------------------------------+  |
|  | Virtual File System (VFS): FAT32 | MBR/GPT Partitions | Block Cache |  |
|  +--------------------------------------------------------------------+  |
|  | Network Stack: Intel e1000 | ARP | IPv4 | ICMP | UDP | TCP | TLS1.3 |  |
|  +--------------------------------------------------------------------+  |
|  | Device Drivers: Framebuffer | AHCI | NVMe | VirtIO | PS/2 | Serial |  |
+--+--------------------------------------------------------------------+--+
   | Hardware Abstraction & Boot Protocol
+--v-----------------------------------------------------------------------+
|  Limine Bootloader (UEFI x86_64) | ACPI | IOAPIC / Local APIC             |
+--------------------------------------------------------------------------+
```

---

## Detailed Features

### 1. Kernel Core & Architecture
- **Monolithic 64-Bit Core**: Compiled in C17 freestanding mode with `-mno-red-zone -mcmodel=kernel`.
- **Limine Protocol**: Modern UEFI x86_64 bootloader handoff and memory map retrieval.
- **Multiprocessing (SMP)**: Multicore bootstrap using ACPI MADT tables and Local APIC IPIs.
- **Preemptive Multitasking**: Local APIC timer-driven preemptive scheduler, thread management, mutexes, and spinlocks.
- **Hardware Syscall Path**: Zero-overhead Ring 3 to Ring 0 transitions through model-specific register `IA32_LSTAR` and per-CPU GS context switching.

### 2. Memory Management
- **Physical Memory Manager (PMM)**: High-performance page frame allocation with bitmap and buddy tracking.
- **Virtual Memory Manager (VMM)**: Recursive 4-level page mapping (PML4 -> PDPT -> PD -> PT) with NX (No-Execute) bit protection and user/supervisor separation.
- **Kernel Heap**: Dynamic, self-coalescing heap allocator with slab caches for frequent kernel structures.

### 3. Storage & Filesystem Subsystem
- **Hardware Drivers**:
  - AHCI SATA Controller
  - Non-Volatile Memory Express (NVMe)
  - VirtIO Block Device (virtio-blk)
  - Memory RAM Disks
- **Partition Engine**: Dynamic detection and parsing of both legacy MBR and modern GUID Partition Tables (GPT).
- **VFS & Filesystems**: Multi-volume VFS (`C:`, `D:`, `E:`), unified sector block cache, and a robust FAT32 filesystem driver supporting long filenames, directory traversal, and atomic writes.

### 4. Networking Stack
- **Network Interface**: Intel 82540EM (e1000) PCI network adapter driver with ring-buffer DMA.
- **Protocols**: Ethernet II, ARP cache resolver, IPv4 packet engine, ICMP echo client (ping), UDP, and full TCP state machine (SYN/ACK, retransmission, sliding window).
- **Services & Crypto**: Automated DHCP client, DNS resolver, built-in HTTP server, and an integrated TLS 1.3 cryptographic engine (supporting X.509, ASN.1, AES-GCM, and SHA-256).

### 5. Native Glass GUI & Windowing Compositor
- **Linear Framebuffer**: High-definition rendering engine supporting standard resolutions (e.g., 1024x768, 1280x720, 1920x1080).
- **Double Buffering & Acrylic Blur**: Background wallpaper caching, dirty rectangle clipping, and surface blur filters with cache hit acceleration.
- **Hardware Pointer**: PS/2 mouse driver with smooth overlay cursor, motion acceleration, bounds clamping, and automatic background repair.
- **KPKG Package Subsystem**: Isolated user-space application package deployment and runtime execution.

---

<a name="getting-started"></a>
## Getting Started

### Prerequisites

To compile and run vinoS, ensure you have the following host tools installed:

- **Operating System**: Windows 10/11 (or Linux via QEMU / Clang cross-toolchain)
- **Compiler**: [LLVM Clang](https://releases.llvm.org/) (version 17 or later) with `clang` and `ld.lld`
- **Emulator**: [QEMU](https://www.qemu.org/) (`qemu-system-x86_64`) with OVMF UEFI firmware
- **Scripting**: Python 3.10+

### Building vinoS

vinoS includes a dedicated native host development builder (`tools/kos.c`):

1. **Compile the Host Tool**:
   ```powershell
   clang -O2 tools/kos.c -o build/kos-tool.exe
   ```

2. **Build the Kernel & EFI Image**:
   ```powershell
   .\build\kos-tool.exe build
   ```

3. **Build with CI Smoke Instrumentation**:
   ```powershell
   .\build\kos-tool.exe build --ci-smoke
   ```

### Running Under QEMU

You can run the operating system directly using the Python automation harnesses:

- **Run Smoke Test Gate (Headless CI)**:
  ```powershell
  python tools/ci_runner.py smoke
  ```

- **Run Storage Subsystem Gate**:
  ```powershell
  python tools/storage_ci.py
  ```

- **Launch Interactive Desktop GUI**:
  ```powershell
  python tools/ci_runner.py gui
  ```

---

<a name="qa--ci-verification"></a>
## QA & CI Verification

vinoS enforces strict quality assurance gates. Every pull request and release is verified against our automated checklist harnesses:

| Gate | Command | Description |
| :--- | :--- | :--- |
| **Boot Smoke** | `python tools/ci_runner.py smoke` | Validates single-CPU boot, Limine handshake, VFS multi-volume, DHCP, TCP, block devices, FAT32, and user-space syscall ABI. |
| **Storage CI** | `python tools/storage_ci.py` | Mounts and tests AHCI, NVMe, and VirtIO devices across MBR and GPT partition layouts with boundary stress checks. |
| **GUI Visual CI** | `python tools/gui_visual_ci.py` | Confirms linear framebuffer initialization, blur cache integrity, and cursor input tracking. |
| **Full Stress** | `python tools/ci_runner.py full` | Runs multi-CPU regression, fault injection, memory pressure, and filesystem stress tests. |

---

## Repository Structure

```
vinoS/
├── arch/x86_64/         # CPU architecture, GDT/IDT stubs, APIC, SMP, syscall init
├── assets/gui/          # Desktop GUI wallpapers, icons, and default desktop apps
├── boot/                # Limine boot protocol interface & requests
├── build/               # Generated object files, ESP image, and kernel ELF
├── docs/                # Architectural specifications and subsystem diagrams
├── drivers/             # AHCI, NVMe, VirtIO, e1000, framebuffer, mouse, keyboard
├── fs/                  # VFS, FAT32 driver, partition parsers (MBR/GPT), block cache
├── include/             # Kernel header definitions and UAPI contracts
├── kernel/              # Core kernel, scheduler, syscall dispatch, installer, logs
├── lib/                 # Memory primitives, crypto (AES/TLS), ASN.1, KPKG parser
├── mm/                  # Memory managers: PMM (physical), VMM (virtual), heap
├── net/                 # Networking stack: e1000, ARP, IPv4, ICMP, UDP, TCP, TLS
├── tests/               # Unit, integration, and fault injection test suites
├── tools/               # Host development tool (kos.c), CI runners, packaging
├── user/                # User-space runtime library (vino libc) and apps
└── .github/             # GitHub Actions CI workflows, issue and PR templates
```

---

<a name="tiếng-việt"></a>
## Tiếng Việt

### Giới Thiệu Dự Án

**vinoS** là dự án hệ điều hành máy tính để bàn (Desktop OS) 64-bit độc lập, được nghiên cứu và phát triển hoàn toàn từ đầu cho nền tảng phần cứng `x86_64`.

Mục tiêu dài hạn của vinoS là mang lại trải nghiệm hệ điều hành đồ họa mượt mà, trực quan với thiết kế kính mờ (acrylic glass), đa nhiệm hiệu năng cao và bảo mật cấp phần cứng. vinoS không phải là một bản phân phối của Linux và cũng không sao chép Windows hay macOS, mà sở hữu kiến trúc kernel nguyên khối (monolithic) và thư viện thực thi riêng biệt.

### Các Tính Năng Nổi Bật

- **Lõi AI Aore (Aore AI OS)**: Hệ thống con chuyên dụng ở tầng kernel giúp các AI agent có thể quan sát, tương tác và điều khiển hệ điều hành ở mức native.
- **Trình Cài Đặt Giao Diện Động**: Hỗ trợ lựa chọn giao diện đồ họa mặc định hoặc tải/cập nhật các gói giao diện người dùng tùy biến qua kho lưu trữ KPKG.
- **Giao Diện Đồ Họa Acrylic Glass**: Bộ tổng hợp đồ họa (compositor) framebuffer tốc độ cao với thuật toán làm mờ kính theo thời gian thực (blur cache), bộ đệm kép (double buffer) và con trỏ chuột phần cứng mượt mà.
- **Ngăn Xếp Mạng IPv4 Toàn Diện**: Trình điều khiển card mạng Intel e1000, hỗ trợ ARP, IPv4, ICMP, UDP, TCP đa luồng, DHCP, DNS và máy chủ HTTP/HTTPS tích hợp mã hóa TLS 1.3.

---

## Roadmap

- [x] Limine UEFI 64-bit Bootloader handoff
- [x] PMM, 4-level VMM, and Dynamic Heap Allocator
- [x] Preemptive Multitasking & SMP Multicore Support
- [x] AHCI SATA, NVMe, and VirtIO Block Drivers
- [x] MBR & GPT Partition Discovery with FAT32 Multi-Volume VFS
- [x] Full IPv4 Networking Stack with TCP/UDP & TLS 1.3
- [x] Native Framebuffer Glass Compositor & KPKG Runtime
- [ ] USB xHCI / USB HID Hardware Driver Stack
- [ ] Aore AI Native Kernel Telemetry & Automation Subsystem
- [ ] Windows Application Compatibility Subsystem (WCS)
- [ ] Audio Driver (Intel HD Audio / AC97)

---

## Contributing

Contributions make the open-source community an inspiring place to learn, create, and build. Any contributions you make are **greatly appreciated**.

Please review our [Contributing Guidelines](CONTRIBUTING.md) and [Code of Conduct](CODE_OF_CONDUCT.md) before submitting a pull request.

---

## License

Distributed under the **MIT License**. See [`LICENSE`](LICENSE) for more information.

---

<div align="center">

Developed with care by **kiyogo** and the vinoS Open Source Contributors.

</div>
