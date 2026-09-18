import subprocess
import time
import os

qemu_exe = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
log_file = r"build\qemu_serial.log"

if os.path.exists(log_file):
    os.remove(log_file)

cmd = [
    qemu_exe,
    "-machine", "q35",
    "-cpu", "max",
    "-m", "256",
    "-drive", r"if=pflash,format=raw,readonly=on,file=C:\Program Files\qemu\share\edk2-x86_64-code.fd",
    "-drive", r"if=pflash,format=raw,file=build\edk2-vars.fd",
    "-drive", r"format=raw,file=fat:rw:build\esp",
    "-serial", f"file:{log_file}",
    "-monitor", "none",
    "-no-reboot",
    "-netdev", "user,id=kosnet,ipv4=on,ipv6=off,hostfwd=tcp::8080-:80",
    "-device", "e1000,netdev=kosnet,mac=52:54:00:12:34:56",
    "-display", "none"
]

print("Starting QEMU...")
proc = subprocess.Popen(cmd)
time.sleep(15)
proc.terminate()
try:
    proc.wait(timeout=3)
except subprocess.TimeoutExpired:
    proc.kill()

print("QEMU finished. Reading log:")
if os.path.exists(log_file):
    with open(log_file, "r", encoding="utf-8", errors="replace") as f:
        print(f.read())
else:
    print("Log file not found!")
