# KOS (vinoS)

**KOS** is an independent, modern x86_64 desktop operating system project designed from scratch. It aims to deliver a high-performance, secure, and user-friendly desktop experience combining familiar workflow paradigms with next-generation AI operating system capabilities.

---

## 🌟 Highlights & Strategic Vision

* **🤖 Aore Architecture (AI-Native OS - Near-future Goal):** KOS is developing the **Aore** subsystem — a dedicated OS runtime and interface layer engineered specifically for Artificial Intelligence. Aore empowers autonomous AI agents to interact with, understand, and control operating system resources with native precision, security, and low latency.
* **💽 Familiar Windows-Style Drive Mapping:** Features a native volume management layer mapping block devices and partitions to familiar Windows-style drive letters (`C:\`, `D:\`), flattening the learning curve for desktop users while laying the foundation for a seamless Windows application compatibility layer.
* **🎨 Modular Desktop Environment & Online/Offline Installer:** During OS installation, users can customize their experience by choosing their preferred graphical interface (Desktop Environment / Themes) fetched automatically from online registries, or select the built-in default environment for instant offline setup.

---

## 🚀 Tính Năng Hiện Tại (Current Core Features & Strengths)

The repository has completed foundational milestones spanning low-level hardware control, storage stacks, networking, and userspace runtime:

### 1. Kernel Core & Multitasking
* **Architecture:** 64-bit Monolithic kernel targeting `x86_64` on UEFI firmware via the **Limine** bootloader.
* **Memory Management:** Multi-tiered memory subsystem featuring PMM (Bitmap frame allocator), VMM (4-level PML4 paging, NX-bit, supervisor protection), and a dynamic reclaiming kernel heap.
* **SMP & Multithreading:** Symmetric Multiprocessing (SMP) multicore initialization, preemptive round-robin thread scheduler, spinlocks, mutexes, and wait queues.
* **Security & Ring 3:** Hardware interrupt dispatching (APIC/IOAPIC/PIC), Ring 3 user mode isolation, `SYSCALL`/`SYSRET` fast syscall fastpath, user-pointer bounds checking (`uaccess`), and fault injection framework.

### 2. Storage Stack & Filesystems
* **Device Drivers:** AHCI SATA, NVMe, VirtIO Block (`virtio-blk`), and RAM Disk drivers with universal DMA page allocation.
* **Partitions & Caching:** Full support for MBR and GPT partition tables, backed by a unified LRU Block Cache.
* **Filesystem & Volumes:** Windows-style Volume Manager (`C:\`, `D:\`), FAT32 read/write implementation, CPIO Initramfs, and RAMFS.

### 3. Network Stack & Cryptography
* **NIC Drivers:** Intel e1000 Gigabit Ethernet PCI driver.
* **Protocols:** Complete freestanding IPv4 stack including Ethernet II, ARP, ICMP, DHCP client, DNS resolver, and TCP socket subsystem.
* **Web & Crypto Engine:** HTTP/HTTPS client, lightweight HTTP daemon (`httpd`), TLS 1.3 record layer, X.509 certificate parser, ASN.1 decoder, and cryptographic primitives (AES-GCM, SHA-256/384).

### 4. Userspace, Package Management & Diagnostics
* **Binary Execution:** ELF64 binary parser and loader.
* **Package System:** `KPKG` package format and on-disk package database (`pkg_db`).
* **Kernel Console & QA:** Interactive framebuffer & serial debug terminal, comprehensive regression test suite, syscall fuzzing, and stress frameworks.

---

## 🎯 Mục Tiêu Trong Tương Lai Gần (Near-Future Roadmap)

* **🧠 Aore Core Subsystem:** Standardized AI-to-OS communication protocol, secure agent sandboxing, and autonomous task automation APIs.
* **🪟 Windows Compatibility Layer:** Subsystem and binary translation/emulation layer allowing essential Windows applications and tools to execute natively on KOS.
* **🖥️ GUI Desktop & Window Manager:** Hardware-accelerated 2D/3D compositor, window management, font rasterizer, and modern desktop UI widgets.
* **📦 Dynamic Online DE Installer:** Interactive graphical installer supporting network discovery and installation of registered desktop environments.
* **🌐 Native Web Browser Engine:** Built-in web client engine and rich developer toolchain.

---

## ⚙️ Target Specifications

* **Architecture:** `x86_64`
* **Firmware:** UEFI (OVMF / EDK2)
* **Bootloader:** Limine
* **Emulator:** QEMU (`q35`, `smp >= 4`, `m >= 512M`)
* **Language:** Freestanding C17 with architecture-specific x86_64 Assembly (NASM)

---

## 📁 Repository Layout

```text
arch/x86_64/   CPU-specific routines: GDT/IDT, interrupts, SMP, syscall stubs
boot/          Limine boot boundary and linker script
drivers/       Storage (AHCI, NVMe, VirtIO), Net (e1000), Display, Timer, Keyboard
fs/            VFS, Volume Manager (C:, D:), FAT32, Block Cache, Partition, ELF
include/       Public kernel headers and UAPI syscall definitions
kernel/        Core kernel, scheduler, process, handles, installer, QA framework
lib/           Crypto (TLS, X.509, AES, SHA), ASN.1, and utility routines
mm/            PMM (Physical memory), VMM (Paging), and Kernel Heap
net/           Network stack (Ethernet, ARP, IPv4, ICMP, TCP, DHCP, DNS, HTTP)
docs/          Architecture design, roadmaps, and phase documentations
tools/         Host build tools, test runners, and QEMU launcher
```

---

## 🔨 Build and Run

### 1. Compile Host Build Tool
Compile the C development tool once from the repository root:

```sh
clang -std=c17 -O2 -Wall -Wextra -Werror tools/kos.c -o tools/kos-tool.exe
tools\kos-tool.exe check
```

### 2. Bootstrap Limine & Assets
```sh
tools\kos-tool.exe bootstrap-limine
tools\kos-tool.exe bootstrap-assets
```

### 3. Build Kernel & ESP Image
```sh
tools\kos-tool.exe build
```

### 4. Run in QEMU
Launch headless with interactive serial console:
```sh
tools\kos-tool.exe run
```

Or run with graphical framebuffer display:
```sh
tools\kos-tool.exe run --display
```

### 5. Automated CI & Test Suite
```sh
python tools/ci_runner.py
```

---

## 📚 Documentation

- [Scope and Roadmap](docs/roadmap.md)
- [Product Vision](docs/product-vision.md)
- [VFS Volumes and Drive-Letter Paths](docs/vfs-volumes.md)
- [Desktop OS Capability Audit](docs/desktop-os-gap-analysis.md)
- [Networking & Security Roadmap](docs/networking-roadmap.md)
- [Architecture Overview](docs/architecture.md)
- [Boot Process](docs/boot-process.md)
- [Environment Setup](docs/environment.md)
- [Coding Conventions](docs/coding-conventions.md)
