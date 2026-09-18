# KOS Architecture

KOS is a 64-bit monolithic operating system designed for x86_64 systems running UEFI firmware.

## Core Subsystems

### Memory Management
- Physical Memory Manager: Frame allocation using bitmap tracking.
- Virtual Memory Manager: Four-level paging with PML4, supervisor protection, and non-executable page enforcement.
- Kernel Heap: Dynamic memory allocator with slab caching and memory recycling.

### Multitasking and Scheduling
- Multiprocessing: Symmetric Multiprocessing support for multicore CPUs via APIC and IOAPIC.
- Thread Scheduler: Preemptive round-robin scheduler managing kernel and user threads.
- Synchronization: Spinlocks, mutexes, and wait queues for concurrency control.

### Userspace and Security
- Ring 3 Isolation: Hardware enforced separation between kernel space and user space.
- Fast Syscall Dispatch: Hardware SYSCALL and SYSRET instructions with user pointer verification.
- ELF Loader: Execution of 64-bit ELF user binaries.

### Storage and Filesystem
- Block Device Drivers: AHCI SATA, NVMe, VirtIO Block, and RAM disk.
- Partition Support: MBR and GPT partition table decoding.
- Caching: LRU block cache layer for high throughput block access.
- Volume Manager: Drive letter assignment for Windows style paths.
- Filesystems: FAT32 read and write engine, Initramfs, and RAMFS.

### Network and Cryptography
- Network Driver: Intel e1000 Gigabit Ethernet PCI driver.
- Network Stack: IPv4, Ethernet, ARP, ICMP, DHCP, DNS, UDP, and TCP.
- Web and Security: HTTP and HTTPS client, lightweight HTTP daemon, TLS 1.3 record layer, X509 certificates, and AES cryptographic primitives.
