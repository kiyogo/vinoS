# KOS Boot Process

KOS uses the Limine bootloader under UEFI to boot the 64-bit kernel ELF image.

## Boot Sequence

1. Firmware Init: UEFI firmware discovers ESP partition and loads Limine bootloader.
2. Kernel Handover: Limine parses boot configuration, maps kernel ELF into high memory, and provides boot information requests.
3. Early Core Setup: Kernel initializes serial output, text console, and GDT and IDT descriptor tables.
4. Memory Setup: Bitmap frame allocator initializes physical RAM. Paging and kernel heap allocator start up.
5. Hardware Discovery: ACPI tables are parsed. APIC and IOAPIC controllers are initialized along with PCI device probing.
6. Multiprocessing: Application Processors are brought online across available CPU cores.
7. Drivers and Storage: Storage controllers and network interfaces initialize. Block devices and volumes are mounted.
8. Scheduler and Userspace: Preemptive multitasking scheduler starts and launches the initial userspace processes.
