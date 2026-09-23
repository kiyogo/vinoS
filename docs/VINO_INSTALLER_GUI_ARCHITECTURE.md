# Vino OS installer and GUI architecture

## Scope

This document records the first implementation slice for Vino OS. It keeps the
existing KOS kernel, storage, VFS, package installer, and CLI intact while
adding explicit Vino concepts on top:

- `Vino CLI` and `Vino Plus` are installer profiles.
- The OS edition is always `default`.
- `Default` and `MVP` are GUI package identities, not OS editions.
- A GUI package is installed through the existing transactional KPKG installer.
- Installer-visible storage comes from real block devices and partitions.

The design intentionally does not synthesize disks, package payloads, downloads,
or a successful installation result when the corresponding backend is absent.

## Implemented components

### GUI registry and manager

`include/kos/gui_manager.h` and `kernel/gui_manager.c` define the registry and
the manager API. The registry currently contains:

| ID | Name | Source | Installer-visible | Recovery |
| --- | --- | --- | --- | --- |
| `default` | Default | local ISO | yes | no |
| `mvp` | MVP | local ISO | no | yes |

The descriptors are also staged into the initramfs as
`C:/GUI/DEFAULT.MANIFEST` and `C:/GUI/MVP.MANIFEST` (the initramfs path is
`GUI/*.MANIFEST`). The manager checks the package path through the real VFS and
passes a real KPKG reader to the existing installer. Therefore `gui install`
returns `GUI_MANAGER_ERR_PACKAGE_MISSING` until a valid `DEFAULT.KPKG` or
`MVP.KPKG` payload is added to the ISO.

The CLI commands are:

```text
gui list
gui installed
gui get-default
gui install <id>
gui remove <id>
gui reinstall <id>
gui start <id>
gui stop <id>
gui restart <id>
gui set-default <id>
gui status <id>
```

Install/remove/reinstall are wired to the package database and transactional
installer. Start/stop/restart currently return an explicit not-implemented
status because a user-space display-session/window-manager backend does not
exist yet.

### Installer flow

`include/kos/installer_flow.h` and `kernel/installer_flow.c` define the state
machine:

```text
WELCOME -> PROFILE_SELECTION -> GUI_SELECTION (Plus only)
         -> DISK_SELECTION -> PARTITION_SETUP -> CONFIRMATION
         -> INSTALLING_OS -> INSTALLING_GUI -> INSTALLING_BOOTLOADER -> FINISHED
```

The engine uses callbacks for base OS installation, GUI installation, and
bootloader installation. This keeps the selection/validation UI independent of
the destructive storage backend. The default UI provides no write callbacks,
so confirmation fails with `NOT_IMPLEMENTED` and cannot modify a disk.

The shared contract layer is in `include/kos/installer_contracts.h` and
`kernel/installer_contracts.c`. It defines the canonical `installer_state`,
`partition_plan`, `install_transaction`, `gui_session`, and
`pending_gui_install` records. It also validates the invariant that every
installer profile targets `os_edition=default`, checks LBA ranges before a plan
can be applied, and records whether CLI fallback remains available after a
partial install. A contract self-test runs during boot.

Disk and partition selection uses the existing block-device registry. Child
partitions must belong to the selected parent disk. A machine with no real
installable disk shows that fact and never creates a fake target.

### Phase 2 storage manager

`include/kos/installer_storage.h` and `kernel/installer_storage.c` expose the
installer storage boundary. Enumeration reports the real block device,
capacity, MBR/GPT table type, partition geometry, parent disk, and detected
FAT32 filesystem. Partition creation and deletion reuse the existing MBR/GPT
parser and scanner; they validate table bounds, LBA arithmetic, overlap, and
parent ownership before writing. All destructive calls require an explicit
confirmation flag. FAT32 formatting is currently the first supported
filesystem operation and flushes the block cache before completing.

The native UI can now create a default target partition on an empty disk,
format an existing or newly-created target as FAT32, and show a review step
before the write. The RAM-disk self-test covers MBR and GPT create/format/
mount/delete flows. `tools/storage_ci.py` repeats the same manager flow on
QEMU disk images through AHCI, VirtIO, and NVMe for both MBR and GPT images.

### Native installer UI

`kernel/gui_canvas.c` provides a small framebuffer canvas and
`kernel/installer_ui.c` provides a keyboard-accessible installer UI. The
palette is based on the Demoweb audit: dark blue background, blue active
surface, light cyan borders, and high-contrast white text. It is an
installer/recovery surface only; it is not the final user-space Default GUI.

Build and entry points:

```text
build/kos-tool.exe build --installer
installer profiles
installer disks
installer ui
```

The normal build remains the existing CLI/test boot path. The installer build
defines `KOS_INSTALLER` and enters the native installer UI after hardware and
VFS initialization.

### Package uninstall ownership

The existing application installer now uses package database file records to
unlink owned files and remove empty package directories. Data markers and data
directories follow the existing `keep_data` option. This is required for GUI
remove/reinstall semantics and avoids leaving stale executable files behind.

## Demoweb mapping

The Demoweb was treated as a visual and interaction reference, not as a source
of kernel code or a package payload.

| Demoweb feature | Vino implementation boundary |
| --- | --- |
| dark/frosted panels | `gui_canvas` colors, fills, and borders now; compositor/window surfaces later |
| start menu/taskbar | future user-space shell/window manager |
| file manager | VFS and file syscalls |
| terminal | existing CLI/terminal |
| settings/control center | future user-space system applications |
| kernel monitor | existing system/network/task information APIs |
| drag, focus, minimize, maximize | future display server/window manager |
| Font Awesome icons | future bitmap/icon asset package; no remote web dependency |

The wallpaper PNG and browser-oriented HTML/CSS/JavaScript remain reference
assets only. They are not copied into the kernel and are not presented as a
native Vino GUI implementation.

## Deliberate follow-up work

The following pieces still need real implementation before claiming a complete
Vino Plus installation:

1. Build and sign actual `DEFAULT.KPKG` and recovery `MVP.KPKG` payloads with
   ELF entry points and all declared assets.
2. Add a user-space display server, input/event routing, window manager, and
   GUI session lifecycle so `start/stop/restart` can launch real processes.
3. Extend the installer storage backend with base-OS file copy, persistent
   install metadata, and bootloader installation. The backend must be
   transactional and preserve CLI fallback when GUI installation fails.
4. Add persistent default-GUI selection and recovery fallback policy.
5. Add mouse/extended-input support and replace the keyboard-only installer
   navigation with pointer events once the input stack supports it.
6. Add remote package transport only after TLS, checksum/signature policy, and
   offline/recovery behavior are defined. The current registry is local-ISO
   only and intentionally does not pretend to download anything.

## Verification

The normal smoke gate was run after these changes and passed:

- kernel boot
- VFS multi-volume checks
- DHCP and TCP checks
- block and FAT32 checks
- installer storage RAM-disk and QEMU MBR/GPT integration checks
- user-space file syscall checks
- smoke completion

The installer variant should be built separately with `build --installer`.
Until the package payload, base-OS copy, metadata, and bootloader backends
above exist, that variant is a storage-aware selection/format UI rather than a
complete OS installer.
