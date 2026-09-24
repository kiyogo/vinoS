"""Exercise the graphical installer, Limine install, reboot, and disk boot in QEMU."""

import argparse
import json
import os
from pathlib import Path
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
            key_name = "spc" if character == " " else character
            if not (key_name == "spc" or key_name.isalnum()):
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


def run(esp_dir):
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

                print("[QEMU] Launching installer UI and selecting Vino CLI")
                monitor.type_text("installer ui")
                monitor.key("ret")
                time.sleep(0.7)
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
                time.sleep(1.5)

                print("[QEMU] Detaching installer media and rebooting into the target disk")
                monitor.execute("device_del", {"id": "installer-media-device"})
                time.sleep(1.0)
                monitor.key("r")
                wait_for_log(log_path, "KOS booting...", process, 90, occurrences=2)
                wait_for_log(log_path,
                    "Vino CLI ready; open the disk installer with `installer ui`.",
                    process, 90, occurrences=2)
                print("[PASS] Limine rebooted from the installed ESP into the Vino CLI shell")
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
    arguments = parser.parse_args()
    try:
        if not run(arguments.esp_dir.resolve()):
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
