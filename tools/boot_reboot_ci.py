"""Exercise the graphical installer, Limine install, reboot, and disk boot in QEMU."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
QEMU = Path(r"C:/Program Files/qemu/qemu-system-x86_64.exe")
OVMF_CODE = Path(r"C:/Program Files/qemu/share/edk2-x86_64-code.fd")
OVMF_VARS_TEMPLATE = Path(r"C:/Program Files/qemu/share/edk2-i386-vars.fd")
TARGET_SIZE = 512 * 1024 * 1024


class QmpMonitor:
    def __init__(self, port, process, timeout=30):
        deadline = time.monotonic() + timeout
        self.socket = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before its QMP monitor became ready")
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if self.socket is None:
            raise RuntimeError("Timed out waiting for QEMU QMP monitor")
        self.socket.settimeout(timeout)
        self.reader = self.socket.makefile("r", encoding="utf-8", newline="\n")
        greeting = json.loads(self.reader.readline())
        if "QMP" not in greeting:
            raise RuntimeError("QEMU did not provide a QMP greeting")
        self.execute("qmp_capabilities")

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self.socket.sendall((json.dumps(request) + "\r\n").encode("utf-8"))
        while True:
            line = self.reader.readline()
            if not line:
                raise RuntimeError("QEMU closed its QMP connection")
            response = json.loads(line)
            if "event" in response:
                continue
            if "error" in response:
                raise RuntimeError(f"QMP {command} failed: {response['error']}")
            if "return" in response:
                return response["return"]

    def hmp(self, command_line):
        return self.execute("human-monitor-command", {"command-line": command_line})

    def key(self, key_name):
        self.hmp(f"sendkey {key_name}")

    def type_text(self, text):
        for character in text:
            key_name = {" ": "spc", "-": "minus"}.get(character, character)
            if not (key_name in ("spc", "minus") or key_name.isalnum()):
                raise ValueError(f"Unsupported QEMU keyboard character: {character!r}")
            self.key(key_name)
            time.sleep(0.04)

    def close(self):
        try:
            self.reader.close()
        finally:
            self.socket.close()


def wait_for_log(log_path, marker, process, timeout, occurrences=1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited while waiting for {marker!r}")
        try:
            contents = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            contents = ""
        if contents.count(marker) >= occurrences:
            return contents
        time.sleep(0.25)
    raise RuntimeError(f"Timed out waiting for {marker!r} in QEMU serial log")


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def run(esp_dir, profile="cli", exercise_mvp_fallback=False,
        exercise_mvp_install=False, exercise_gui_manager=False):
    if profile not in ("cli", "plus"):
        raise ValueError(f"Unsupported installer profile: {profile}")
    if exercise_mvp_fallback and profile != "plus":
        raise ValueError("MVP fallback exercise requires the Plus recovery package")
    if exercise_mvp_install and profile != "cli":
        raise ValueError("MVP ISO-install exercise requires the CLI profile")
    if exercise_gui_manager and profile != "plus":
        raise ValueError("GUI manager exercise requires both installed Plus packages")
    if exercise_gui_manager and exercise_mvp_fallback:
        raise ValueError("GUI manager and MVP fallback exercises use separate disk scenarios")
    required = (QEMU, OVMF_CODE, OVMF_VARS_TEMPLATE,
                esp_dir / "EFI" / "BOOT" / "BOOTX64.EFI",
                esp_dir / "EFI" / "BOOT" / "limine.conf",
                esp_dir / "boot" / "kos.elf",
                esp_dir / "boot" / "initramfs.cpio")
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError("Missing QEMU boot-test input(s): " + ", ".join(missing))

    BUILD.mkdir(exist_ok=True)
    log_fd, log_name = tempfile.mkstemp(prefix="boot-reboot-ci-", suffix=".log",
                                        dir=BUILD)
    os.close(log_fd)
    log_path = Path(log_name)

    with tempfile.TemporaryDirectory(prefix="vino-phase4-", dir=BUILD) as temporary:
        temporary = Path(temporary)
        media_dir = temporary / "installer-esp"
        shutil.copytree(esp_dir, media_dir)
        target_path = temporary / "target.img"
        with target_path.open("wb") as target:
            target.truncate(TARGET_SIZE)
        vars_path = temporary / "ovmf-vars.fd"
        shutil.copyfile(OVMF_VARS_TEMPLATE, vars_path)
        monitor_port = free_tcp_port()

        command = [
            str(QEMU), "-machine", "q35", "-cpu", "max", "-m", "512",
            "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
            "-drive", f"if=pflash,format=raw,file={vars_path}",
            "-drive", f"if=none,format=raw,id=target,file={target_path}",
            "-device", "ich9-ahci,id=ahci0",
            "-device", "ide-hd,drive=target,bus=ahci0.0,id=target-device,bootindex=1",
            "-drive", f"if=none,format=raw,id=installer-media,file=fat:rw:{media_dir}",
            "-device", "qemu-xhci,id=install-xhci",
            "-device", "usb-storage,drive=installer-media,bus=install-xhci.0,"
                       "id=installer-media-device,bootindex=2",
            "-serial", "stdio", "-monitor", "none",
            "-qmp", f"tcp:127.0.0.1:{monitor_port},server=on,wait=off",
            "-display", "none",
        ]
        print("[QEMU] Starting Limine installer with a disposable 512 MiB target disk")
        with log_path.open("w", encoding="utf-8", errors="replace") as serial_log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=serial_log,
                                       stderr=subprocess.STDOUT)
            monitor = None
            try:
                monitor = QmpMonitor(monitor_port, process)
                wait_for_log(log_path,
                    "Vino CLI ready; open the disk installer with `installer ui`.",
                    process, 90)
                time.sleep(1.5)

                print(f"[QEMU] Launching installer UI and selecting Vino {profile.upper()}")
                monitor.type_text("installer ui")
                monitor.key("ret")
                time.sleep(0.7)
                if profile == "plus":
                    monitor.key("backspace")
                    time.sleep(0.2)
                    monitor.key("j")
                    monitor.key("ret")
                    wait_for_log(log_path, "Installer: selected profile=plus",
                                 process, 20)
                    monitor.key("ret")
                    wait_for_log(log_path, "Installer: selected gui=default",
                                 process, 20)
                    monitor.key("ret")
                else:
                    monitor.key("ret")
                disk_log = wait_for_log(log_path, "Installer: selected disk=",
                                        process, 20)
                selection = disk_log.rsplit("Installer: selected disk=", 1)[1].splitlines()[0]
                if f"capacity={TARGET_SIZE} bytes" not in selection:
                    raise RuntimeError(
                        "Safety stop: installer selected a disk other than the disposable "
                        f"QEMU target ({selection.strip()})")

                print("[QEMU] Creating and formatting the target ESP")
                monitor.key("c")
                time.sleep(0.4)
                monitor.key("c")
                wait_for_log(log_path, "Installer: selected partition=", process, 30)

                print("[QEMU] Installing OS and Limine boot files")
                monitor.key("ret")
                wait_for_log(log_path, "Base Vino OS committed on ", process, 90)
                wait_for_log(log_path,
                    "Limine UEFI bootloader committed; Vino OS is bootable in CLI mode.",
                    process, 90)
                if profile == "plus":
                    wait_for_log(log_path,
                        "[installer] GUI 'default' installed under Vino OS edition=default.",
                        process, 90)
                time.sleep(1.5)

                print("[QEMU] Detaching installer media and rebooting into the target disk")
                monitor.execute("device_del", {"id": "installer-media-device"})
                time.sleep(1.0)
                monitor.key("r")
                wait_for_log(log_path, "KOS booting...", process, 90, occurrences=2)
                if profile == "plus":
                    wait_for_log(log_path,
                        "Vino Plus: starting selected GUI 'default'.", process, 90)
                    wait_for_log(log_path,
                        "GUI session 1: started 'default' in process", process, 90)
                    time.sleep(2.5)
                    post_start_log = log_path.read_text(
                        encoding="utf-8", errors="replace")
                    if "GUI session 'default' exited with" in post_start_log:
                        raise RuntimeError("Default GUI exited during startup")
                    print("[QEMU] Default GUI started after the installed Vino Plus reboot")
                    monitor.key("q")
                    wait_for_log(log_path,
                        "GUI session 'default' stopped; CLI remains available", process, 30)
                    wait_for_log(log_path,
                        "Vino CLI ready; open the disk installer with `installer ui`.",
                        process, 30, occurrences=2)
                    monitor.type_text("vino gui stats")
                    monitor.key("ret")
                    stats_log = wait_for_log(log_path,
                        "GUI buffers: backbuffer=on wallpaper_cache=on", process, 30)
                    frame_counts = re.findall(r"GUI frames: redraws=(\d+)", stats_log)
                    if not frame_counts or int(frame_counts[-1]) > 4:
                        raise RuntimeError(
                            "Default GUI kept redrawing while idle; expected at most 4 frames "
                            f"across startup and the 2.5-second idle window (got {frame_counts})")
                    print("[PASS] Vino Plus -> Default install -> Limine reboot -> Default GUI -> CLI fallback")
                    print("[PASS] GUI idle redraw bound and backbuffer/wallpaper cache instrumentation")
                    if exercise_gui_manager:
                        print("[QEMU] Exercising GUI inventory, session, restart, and persistent default")
                        monitor.type_text("vino gui list")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "default - Default v1.0.0 [iso] installed", process, 30)
                        wait_for_log(log_path,
                            "mvp - MVP v1.0.0 [iso] installed", process, 30)
                        monitor.type_text("vino gui installed")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "default - Default [selected for boot]", process, 30)
                        wait_for_log(log_path, "mvp - MVP", process, 30)
                        monitor.type_text("vino gui available")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "default - Default v1.0.0 [installed]", process, 30)
                        wait_for_log(log_path,
                            "mvp - MVP v1.0.0 [installed]", process, 30)
                        monitor.type_text("vino gui get-default")
                        monitor.key("ret")
                        wait_for_log(log_path, "] default", process, 30)
                        monitor.type_text("vino gui status default")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI default: installed, selected for boot; last_session=1 process_id=",
                            process, 30)
                        monitor.type_text("vino gui restart default")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI session 2: started 'default' in process", process, 30)
                        time.sleep(1.0)
                        manager_log = log_path.read_text(encoding="utf-8", errors="replace")
                        if "GUI session 'default' exited with" in manager_log:
                            raise RuntimeError("Default GUI crashed during manager restart")
                        monitor.key("q")
                        wait_for_log(log_path,
                            "GUI session 'default' stopped; CLI remains available",
                            process, 30, occurrences=2)
                        wait_for_log(log_path, "gui restart default: OK", process, 30)
                        monitor.type_text("vino gui set-default mvp")
                        monitor.key("ret")
                        wait_for_log(log_path, "gui set-default mvp: OK", process, 30)
                        monitor.type_text("vino gui get-default")
                        monitor.key("ret")
                        wait_for_log(log_path, "] mvp", process, 30)
                        monitor.type_text("vino gui installed")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "mvp - MVP [selected for boot]", process, 30)
                        monitor.type_text("reboot")
                        monitor.key("ret")
                        wait_for_log(log_path, "KOS booting...", process, 90,
                                     occurrences=3)
                        wait_for_log(log_path,
                            "Vino Plus: starting selected GUI 'mvp'.", process, 90)
                        wait_for_log(log_path,
                            "GUI session 1: started 'mvp' in process", process, 90)
                        time.sleep(1.0)
                        manager_log = log_path.read_text(encoding="utf-8", errors="replace")
                        if "GUI session 'mvp' exited with" in manager_log:
                            raise RuntimeError("Persistently selected MVP GUI crashed after reboot")
                        monitor.key("c")
                        wait_for_log(log_path,
                            "GUI session 'mvp' stopped; CLI remains available", process, 30)
                        monitor.type_text("vino gui remove default")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI Manager: removed 'default'; Vino OS remains bootable in CLI mode.",
                            process, 30)
                        monitor.type_text("vino gui installed")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "mvp - MVP [selected for boot]", process, 30,
                            occurrences=2)
                        monitor.type_text("vino gui reinstall default")
                        monitor.key("ret")
                        wait_for_log(log_path, "gui reinstall default: OK", process, 30)
                        print("[PASS] GUI commands -> session restart -> persistent MVP selection -> reboot -> package isolation")
                    if exercise_mvp_fallback:
                        print("[QEMU] Removing Default on the disposable target and requesting it again")
                        monitor.type_text("vino gui remove default")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI Manager: removed 'default'; Vino OS remains bootable in CLI mode.",
                            process, 30)
                        monitor.type_text("vino gui start default")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "Default GUI failed to start (GUI is not installed); launching MVP recovery.",
                            process, 30)
                        wait_for_log(log_path,
                            "GUI session 2: started 'mvp' in process", process, 30)
                        time.sleep(1.0)
                        mvp_log = log_path.read_text(encoding="utf-8", errors="replace")
                        if ("GUI session 'mvp' exited with" in mvp_log
                                or "GUI session 'mvp' stopped; CLI remains available" in mvp_log):
                            raise RuntimeError("The standalone MVP GUI exited during startup")
                        monitor.key("c")
                        wait_for_log(log_path,
                            "GUI session 'mvp' stopped; CLI remains available", process, 30)
                        wait_for_log(log_path,
                            "gui start default: OK", process, 30)
                        print("[PASS] Default removed -> independent MVP fallback -> CLI shell")
                else:
                    wait_for_log(log_path,
                        "Vino CLI ready; open the disk installer with `installer ui`.",
                        process, 90, occurrences=2)
                    print("[PASS] Limine rebooted from the installed ESP into the Vino CLI shell")
                    if exercise_mvp_install:
                        print("[QEMU] Installing MVP from the ISO package in CLI mode")
                        monitor.type_text("vino gui install mvp")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI Manager: installing 'mvp' from C:/GUI/MVP.KPKG",
                            process, 30)
                        wait_for_log(log_path, "gui install mvp: OK", process, 30)
                        monitor.type_text("vino gui start mvp")
                        monitor.key("ret")
                        wait_for_log(log_path,
                            "GUI session 1: started 'mvp' in process", process, 30)
                        time.sleep(1.0)
                        mvp_log = log_path.read_text(encoding="utf-8", errors="replace")
                        if ("GUI session 'mvp' exited with" in mvp_log
                                or "GUI session 'mvp' stopped; CLI remains available" in mvp_log):
                            raise RuntimeError("The locally installed MVP GUI exited during startup")
                        monitor.key("c")
                        wait_for_log(log_path,
                            "GUI session 'mvp' stopped; CLI remains available", process, 30)
                        wait_for_log(log_path, "gui start mvp: OK", process, 30)
                        print("[PASS] CLI ISO install -> independent MVP session -> CLI shell")
                return True
            finally:
                if monitor is not None:
                    try:
                        monitor.execute("quit")
                    except (OSError, RuntimeError):
                        pass
                    monitor.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        if not log_path.exists() or log_path.stat().st_size == 0:
            raise RuntimeError("QEMU produced no serial output")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-dir", type=Path, default=ROOT / "build" / "esp",
                        help="UEFI installer ESP staged by kos-tool build --installer")
    parser.add_argument("--profile", choices=("cli", "plus"), default="cli",
                        help="Installer profile to exercise; plus also checks Default GUI autostart")
    parser.add_argument("--exercise-mvp-fallback", action="store_true",
                        help="on a disposable Plus disk, remove Default and verify MVP fallback")
    parser.add_argument("--exercise-mvp-install", action="store_true",
                        help="on a disposable CLI disk, install MVP from ISO and verify its session")
    parser.add_argument("--exercise-gui-manager", action="store_true",
                        help="on a disposable Plus disk, verify GUI commands and persistent selection")
    arguments = parser.parse_args()
    try:
        if not run(arguments.esp_dir.resolve(), arguments.profile,
                   arguments.exercise_mvp_fallback, arguments.exercise_mvp_install,
                   arguments.exercise_gui_manager):
            return 1
    except (OSError, RuntimeError, ValueError) as error:
        print(f"[FAIL] {error}")
        logs = sorted(BUILD.glob("boot-reboot-ci-*.log"),
                      key=lambda path: path.stat().st_mtime, reverse=True)
        print(f"QEMU boot test log: {logs[0] if logs else 'none'}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
