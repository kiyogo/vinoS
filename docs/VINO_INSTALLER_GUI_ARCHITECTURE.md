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

The generated packages and detached checksums are staged into the same
initramfs included by the bootable ISO and recovery media:
`C:/GUI/{DEFAULT,MVP}.KPKG` and `.KPKG.SHA256`. Each package embeds its manifest;
the build-time descriptors are not separate runtime files. The existing KPKG
installer validates each member hash and records package ownership. Both
packages declare an x86-64 ELF entry point, version, architecture, and isolated
`/system/gui/<id>` namespace. A detached whole-package SHA-256 is checked before
installation. Packages currently declare `signature_type=none` because this
repository has no signing key; checksums provide integrity, not publisher
authenticity.

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
vino gui install <id>
```

Install/remove/reinstall are wired to the package database and transactional
installer. Start/stop/restart use the process/session manager and user-space
display service; a failed Default launch can try MVP before leaving the CLI
available.

### Installer flow

`include/kos/installer_flow.h` and `kernel/installer_flow.c` define the state
machine:

```text
WELCOME -> PROFILE_SELECTION -> GUI_SELECTION (Plus only)
         -> DISK_SELECTION -> PARTITION_SETUP -> CONFIRMATION
         -> INSTALLING_OS -> INSTALLING_BOOTLOADER
         -> INSTALLING_GUI (Plus only) -> FINISHED
```

The engine uses separate `install_base_os()`, `install_gui()`, and
`install_bootloader()` callbacks. This keeps selection and validation separate
from disk writes. The base OS and Limine UEFI files are installed before the
selected GUI package so the machine has a CLI boot path if GUI installation
fails.

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

### Phase 3 base OS installer

`kernel/installer_engine.c` implements the three installer operations without
combining the CLI and GUI editions. A new default target is an EFI System
partition (GPT EFI System GUID or MBR type `0xEF`) formatted as FAT32. The
installer writes `VINO/BOOT/KOS.ELF`, the original `INITRAMFS.CPIO`, and all
remaining initramfs system files below `VINO/SYSTEM`. It writes a SHA-256 list,
flushes the block cache, invalidates cached sectors, and verifies each copied
file by reading it back. `VINO/SYSTEM/OS.META`, including
`os_edition=default` and `install_state=committed`, is written last and read
back before the base OS transaction is accepted.

The build includes Limine's x86-64 UEFI application and both generated GUI
packages in the installer initramfs.
`install_bootloader()` writes it and a Vino OS Limine configuration to
`EFI/BOOT`, then verifies their checksums. The GUI package installer targets
`VINO/SYSTEM/GUI/<id>` and stores package data under
`VINO/SYSTEM/GUI/DATA/<id>` on the installed system volume. Outside a
persistent installed volume, packages use the equivalent `D:/system/gui`
namespace. Missing or checksum-invalid packages fail without reporting a
successful install; the installed CLI remains bootable.

### Native installer UI

`kernel/gui_canvas.c` and `kernel/gui_service.c` adapt the kernel framebuffer
and keyboard drivers to validated display/input syscalls. `user/lib/gui.c` and
`user/lib/widgets.c` provide user-space surface, input-event, drawing, and
window/panel/button/dropdown/list/dialog/progress-bar abstractions. Installer,
Default, and MVP render in user processes; the kernel retains the hardware
drivers and service boundary only. UI processes are marked read-only: VFS
write/open-for-write/unlink/mkdir/rename syscalls are denied, while disk
changes must go through the privileged installer or package-manager service.
The Installer palette is based on the Demoweb audit: dark blue background,
blue active surface, light cyan borders, and high-contrast white text.

Build and entry points:

```text
build/kos-tool.exe build --installer
installer profiles
installer disks
installer ui
```

The normal build remains the existing CLI/test boot path. The installer build
defines `KOS_INSTALLER`, boots to the CLI without running the full regression
suite, and exposes the native installer through `installer ui`. After a
successful install, the Finish screen syncs storage and reboots into the CLI.

Before writing Limine files, `install_bootloader()` checks the committed OS
metadata and verifies that both the installed kernel and initramfs match the
base OS SHA-256 manifest. Any missing or altered boot payload aborts bootloader
installation; the UI reports the failure and does not reboot. Successful CLI
installs show Finish for five seconds (Enter reboots immediately; Esc returns
to the live CLI).

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
| dark/frosted panels | layered navy/blue panels with cyan accents, borders, and shadow treatment in Default |
| start menu/taskbar | native user-space desktop, start menu, full-width bottom bar, dock, and launcher shortcuts |
| file manager | read-only directory navigation and text preview through VFS file syscalls |
| terminal | native user-space shell with VFS, uptime, network, and app-launch commands |
| settings/control center | in-session accent, motion, and panel-density settings |
| kernel monitor | live network/device snapshot provided by the system-info syscall |
| focus, minimize, maximize, move | keyboard-driven window manager and focus/selection states; the current input UAPI exposes keyboard events only, so pointer hover/click is not claimed |
| Font Awesome icons | future bitmap/icon asset package; no remote web dependency |

The wallpaper PNG and browser-oriented HTML/CSS/JavaScript remain reference
assets only. They are not copied into the kernel and are not presented as a
native Vino GUI implementation.

### Phase 7 — Native Default desktop

`assets/gui/default_gui_app.c` is the `DEFAULT.KPKG` ELF entry point. The
standalone `assets/gui/mvp_gui_app.c` is compiled separately as the
`MVP.KPKG` ELF entry point; MVP does not load Default or hide its features.
The Default process draws the desktop, status bar, dock, start/context menus,
and Terminal, Files, Settings, and Network windows through the user-space GUI
display service. Its VFS-backed file browser is read-only and supports
directory navigation and text preview. The terminal runs a small built-in shell and does not forward
keystrokes to the kernel console. Network details come from a read-only
`SYS_system_info` snapshot.

Default keeps the Demoweb navy/cyan palette and uses a full-width bottom bar
with Start, running-app dock indicators, and network status. Window stacking is
bounded to the four built-in apps, animated content follows its window bounds,
and the terminal reserves a separate prompt row. Interaction remains
keyboard-driven (`S`, `C`, `Tab`, `M`, `Z`, `X`, and app shortcuts) until the
input service exposes pointer events; there is no browser or WebView.

The QEMU reboot integration test supports `--profile plus` to exercise the
Plus → Default package install → Limine reboot → Default GUI autostart path,
then exits the GUI and verifies the CLI fallback remains available.

The package builder still emits the manifest, ownership list, per-file hashes,
ELF entry point, and detached package SHA-256. On boot, the manager reads the
committed OS profile and `VINO/SYSTEM/GUI/SELECTED.TXT`; only
`installer_profile=plus` with `gui_id=default` requests Default at startup.
Vino CLI continues to boot to the CLI. If Default cannot start, the manager
tries the separately installed MVP recovery GUI, then leaves the CLI as the
final fallback. No browser, WebView, HTML, CSS, JavaScript, or remote asset is
used by the desktop.

Input is currently keyboard-only: number shortcuts launch apps; `S` opens the
start menu; `C` opens the context menu; `Tab` cycles focus; `M` minimizes; `Z`
maximizes; `G` then `H/J/K/L` moves a window; `X` closes; and `Esc` returns
focus to the desktop. Selection highlight serves as focus/hover feedback until
a pointer driver and pointer input events exist. Settings are currently
session-only, the file browser is read-only, and Network is a status view.

### Phase 8 — Standalone MVP recovery desktop

MVP is a separately built, smaller native application with its own KPKG,
manifest, ownership list, ELF, and SHA-256 metadata. It has a lightweight
launcher, DHCP connect/status, a read-only VFS file browser with text preview,
local Default/MVP install or repair, selection of Default for the next boot,
recovery retry, and confirmed restart/shutdown actions. Selecting CLI exits
MVP back to the kernel terminal. Its source and package are independent of
Default; the installer also puts the recovery package in the installed
system's separate `system/gui/mvp` namespace before committing Default as the
selected GUI.

If selected Default cannot be started, including a missing executable, Plus
boot and explicit `gui start/restart default` attempts try MVP first and retain
the CLI as the final fallback. MVP can request a one-shot retry of Default
after repair. The file browser is intentionally read-only. Remote install is
visible but fail-closed: no remote repository/provider, pinned package signing
key, or configured TLS CA root currently exists, so the kernel rejects remote
executables. Shutdown currently syncs storage and halts CPUs; ACPI firmware
power-off is not implemented. Package integrity uses detached SHA-256, not a
release signature.

The disposable-disk QEMU integration can exercise the independent recovery
path:

```powershell
python tools/boot_reboot_ci.py --profile plus --exercise-mvp-fallback
python tools/boot_reboot_ci.py --profile cli --exercise-mvp-install
```

After the Plus reboot it removes Default from that test disk, requests Default
again, verifies that the separately installed MVP session starts, then exits
to the CLI. The CLI-profile run installs MVP from the ISO package, starts its
standalone session, and returns to CLI. Both runs use disposable target disks
and do not touch a host disk.

### Phase 9 — GUI manager runtime

The kernel command dispatcher supports `vino gui list`, `installed`,
`available`, `install`, `remove`, `reinstall`, `start`, `stop`, `restart`,
`set-default`, `get-default`, and `status`. `available` includes only
packages whose local package and detached checksum verify. `status` reports
the most recent matching session ID, process ID, state, and exit code.

The selected GUI ID is read from and synced to
`VINO/SYSTEM/GUI/SELECTED.TXT`. Plus startup launches the selected GUI; a
failed Default launch gets one MVP attempt, then returns to CLI. Selecting
MVP does not recurse into another GUI fallback. Removing the selected GUI
clears the boot selection before unlinking package-owned files. Removing an
active session stops and reaps its process first. Registry initialization
rejects overlapping GUI install namespaces/package IDs so one GUI package
cannot claim or remove another GUI's files. Runtime fallback is one-shot and
never reboots the machine, so a failing GUI cannot cause a GUI reboot loop.

### Phase 10 — Remote GUI interfaces

`include/kos/gui_repository.h` defines the `gui_repository`,
`remote_package_provider`, and `package_verifier` callback contracts.
Repositories use stable IDs; transport endpoints are deliberately not embedded
in manifests or pending state. A provider returns a package artifact and must
provide its matching release callback. There is no progress callback until a
provider can report measured transfer progress.

`pending_gui_install` carries the GUI/repository IDs and expected version,
checksum, signature metadata, and terminal error. The contract can represent a
future MVP/network handoff, but no repository or remote provider is registered.
Unavailable callbacks return `NOT_CONFIGURED`; they do not fabricate a URL,
download, signature success, or progress. The shared verifier interface has a
real SHA-256 implementation for integrity checking; signature verification
remains unconfigured and remote executable installation remains fail-closed.

## Deliberate follow-up work

Remaining work beyond the Phase 10 remote interfaces:

1. Add a release signing key and signature verification policy; current package
   payloads are unsigned and carry SHA-256 integrity metadata only.
2. Add pointer events and real pointer hover/click/drag; this build intentionally
   uses keyboard focus and window-move controls because the input ABI is key-only.
3. Persist user settings and add safe, writable file-manager operations.
4. Persist package ownership records across reboot; until that is implemented,
   local repair reinstalls a verified ISO package when the runtime registry is
   empty.
5. Configure and audit a trusted catalog, TLS CA roots, pinned signing key,
   signature policy, and offline/recovery behavior before registering a real
   remote provider. Remote installs currently fail closed.
6. Add ACPI S5 power-off after validating hardware/QEMU support.

## Verification

The installer variant is built with `build --installer`; this also builds and
stages both GUI packages into its initramfs. Storage-manager RAM and QEMU
MBR/GPT coverage remains in the Phase 2 validation path. The end-to-end
Vino Plus install/reboot/Default-GUI acceptance still requires a disposable
QEMU target-disk run; a successful compile alone does not claim that acceptance.
