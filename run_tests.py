import os
import subprocess
import time
import sys

qemu_exe = "C:/Program Files/qemu/qemu-system-x86_64.exe"
firmware = "C:/Program Files/qemu/share/edk2-x86_64-code.fd"
vars_file = "build/edk2-vars.fd"
log_file = "build/qemu_serial.log"

try:
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
except Exception:
    pass
time.sleep(0.5)

if os.path.exists(log_file):
    try:
        os.remove(log_file)
    except Exception:
        pass

cmd = [
    qemu_exe,
    "-machine", "q35",
    "-cpu", "max",
    "-smp", "4",
    "-m", "512",
    "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
    "-drive", f"if=pflash,format=raw,file={vars_file}",
    "-drive", "format=raw,file=fat:rw:build/esp",
    "-serial", f"file:{log_file}",
    "-monitor", "none",
    "-no-reboot",
    "-netdev", "user,id=kosnet,ipv4=on,ipv6=off",
    "-device", "e1000,netdev=kosnet,mac=52:54:00:12:34:56",
    "-display", "none"
]

print("Launching QEMU (SMP=4, CPU=max)...")
proc = subprocess.Popen(cmd)

pkg_pass = False
sec_pass = False
sys_pass = False
output_lines = []

start_time = time.time()
while time.time() - start_time < 25:
    time.sleep(0.5)
    if os.path.exists(log_file):
        try:
            with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
                output_lines = content.splitlines()
                pkg_pass = "ALL APPLICATION PACKAGE & INSTALLER TESTS PASSED" in content
                sec_pass = "ALL KERNEL SECURITY TESTS PASSED" in content
                sys_pass = "ALL SYSCALL ABI & MEMORY SAFETY TESTS PASSED" in content
                if pkg_pass and sec_pass and sys_pass:
                    time.sleep(1)
                    break
        except Exception:
            pass

proc.terminate()
try:
    proc.wait(timeout=3)
except Exception:
    proc.kill()

# Print full captured log
if os.path.exists(log_file):
    with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
        print(f.read())

print("\n--- TEST SUMMARY ---")
print(f"Kernel Security (sectest) : {'PASS' if sec_pass else 'FAIL'}")
print(f"Syscall ABI (syscalltest) : {'PASS' if sys_pass else 'FAIL'}")
print(f"Application Package (pkg) : {'PASS' if pkg_pass else 'FAIL'}")

if pkg_pass and sec_pass and sys_pass:
    print("\n>>> ALL TARGET TESTS PASSED! <<<")
    sys.exit(0)
else:
    print("\n>>> SOME TESTS FAILED! <<<")
    sys.exit(1)
