# Default GUI controls and kernel-service mapping

This inventory covers the controls in `Demoweb` and the corresponding native
Default GUI. A control is either connected to the named kernel/user-space API,
limited to a stated capability, or explicitly unavailable. Demo-only sample
data is not treated as live system state.

| Demo/native control | Backend and behavior | State / limits |
| --- | --- | --- |
| Desktop, wallpaper, window focus/drag/minimize/maximize, dock, Start menu, context menu | Native GUI renderer and in-process window state | Works as GUI behavior; not a kernel setting. Start search filters only the native apps listed by this build. |
| Taskbar clock/status | `SYS_gettime`; system volume from `SYS_system_info` | Shows uptime, not wall-clock time. |
| File Manager directory listing and navigation | `vino_fs_open`, `vino_fs_stat`, `vino_fs_getdents`, `vino_fs_read`, `vino_fs_close` → VFS | Reads mounted VFS entries. The preview is read-only. Create/write/delete/copy/move are unavailable in the current VFS client. If the kernel reports no installed volume, no drive letter is invented. |
| File Manager “Network status” | Opens the native Network view | Shows NIC state only; it does not mount a network share. |
| Demo Desktop/Documents/Downloads/Music/Pictures/Videos sample folders | No matching per-user home-directory API or seeded user files | Not shown as real folders. Only entries actually returned by VFS are shown. |
| Demo This PC drive capacities, USB devices/eject, NAS/cloud shares | No volume-enumeration, USB-storage, or remote-share UI API in this GUI | Unavailable; no fake drives, sizes, or device names. |
| Network status (interface name, MAC, IPv4, mask, gateway, DNS, DHCP state) | `SYS_system_info` → `net_interface_info` / kernel network state | Read-only values from the kernel. A missing/empty field is shown unavailable; no fallback `eth0`, SSID, or “connected” claim. |
| Network “Request DHCP” | `SYS_system_control(KOS_SYSTEM_CONTROL_CONNECT_NETWORK)` → `net_dhcp_acquire` | Performs a real DHCP request/renewal and then refreshes status. Failure/timeout is reported; it does not toggle a Wi-Fi radio. |
| Wi-Fi SSID/radio on-off, airplane mode, proxy | No Wi-Fi management/scan/configuration API | Unavailable. DHCP on an exposed NIC is the only network action in this GUI. |
| Control Center Network tile | Opens Network view; status comes from `SYS_system_info` | Works as a shortcut and reflects kernel NIC/IP state. |
| Bluetooth toggle/settings | No Bluetooth driver/service API | Explicitly unavailable; no radio state or scan results are simulated. |
| Brightness, audio volume, media-player controls | No backlight/display-control or audio service API | Unavailable; no sliders or playback state are presented as functional. |
| Dark-mode/accent, motion, window-shadow preferences | Native renderer state only | Accent/motion/shadow changes work for this GUI session only. They are not persistent and do not claim to change kernel/driver settings. Wallpaper is loaded from the local package asset. |
| Notifications tray/panel and “clear all” | No kernel notification service/API | Panel states that notifications are unavailable; fabricated download/update/message notices and fake clearing are removed. |
| System information | `SYS_gettime`, `SYS_sysinfo`, and ABI v2 `SYS_system_info` | Uptime, task count, PMM total physical bytes, and free tracked-frame bytes are kernel-reported. CPU utilization, battery, temperature, and wall clock are unavailable. |
| Installed-apps Settings page | Native app table compiled into this package | Lists only apps this GUI can actually open; it is not a general package inventory/uninstaller. |
| Terminal `gui status` | `vino_gui_control(KOS_GUI_CONTROL_QUERY)` → kernel GUI manager | Prints the manager-reported default and installed package IDs; other GUI-manager shell commands are explicitly not implemented here. |
| Restart | Two-step confirmation, then `SYS_system_control(KOS_SYSTEM_CONTROL_RESTART, confirmed)` | Calls the kernel reboot path only after explicit confirmation. |
| Shut down | Two-step confirmation, then `SYS_system_control(KOS_SYSTEM_CONTROL_SHUTDOWN, confirmed)` | Current kernel implementation halts the CPU; platform-level power-off is not guaranteed and is stated in the dialog. |
| Demo browser, email, calendar, store, document editors and generic media apps | No corresponding native service/package in this Default GUI | Not exposed as working shortcuts; demo placeholders are not presented as installed apps. |

## System-info ABI v2

`struct kos_system_info` now includes `total_memory_bytes` and
`free_memory_bytes`, populated from the kernel PMM. The ABI version is bumped
to 2 so a client built for the old structure is rejected instead of silently
interpreting a changed layout.
