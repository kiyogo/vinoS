import subprocess
import time
import sys
import threading

qemu_exe = "C:/Program Files/qemu/qemu-system-x86_64.exe"
firmware = "C:/Program Files/qemu/share/edk2-x86_64-code.fd"
vars_file = "build/edk2-vars.fd"

cmd = [
    qemu_exe,
    "-machine", "q35",
    "-cpu", "max",
    "-smp", "4",
    "-m", "512",
    "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
    "-drive", f"if=pflash,format=raw,file={vars_file}",
    "-drive", "format=raw,file=fat:rw:build/esp",
    "-serial", "stdio",
    "-monitor", "none",
    "-no-reboot",
    "-netdev", "user,id=kosnet,ipv4=on,ipv6=off",
    "-device", "e1000,netdev=kosnet,mac=52:54:00:12:34:56",
    "-display", "none"
]

print("Launching QEMU (SMP=4, CPU=max)...")
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

output_lines = []

def reader():
    try:
        for line in iter(proc.stdout.readline, ''):
            output_lines.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()
    except Exception as e:
        print("Reader exception:", e)

t = threading.Thread(target=reader, daemon=True)
t.start()

# Wait for boot and all bootstrap tests to run sequentially
time.sleep(20)

proc.terminate()
try:
    proc.wait(timeout=3)
except Exception:
    proc.kill()

full_output = "".join(output_lines)
print("\n--- TEST SUMMARY ---")
pkg_pass = any("ALL APPLICATION PACKAGE & INSTALLER TESTS PASSED" in line for line in output_lines)
sec_pass = any("ALL KERNEL SECURITY TESTS PASSED" in line for line in output_lines)
sys_pass = any("ALL SYSCALL ABI & MEMORY SAFETY TESTS PASSED" in line for line in output_lines)

print(f"Kernel Security (sectest) : {'PASS' if sec_pass else 'FAIL'}")
print(f"Syscall ABI (syscalltest) : {'PASS' if sys_pass else 'FAIL'}")
print(f"Application Package (pkg) : {'PASS' if pkg_pass else 'FAIL'}")

if pkg_pass and sec_pass and sys_pass:
    print("\n>>> ALL TARGET TESTS PASSED! <<<")
    sys.exit(0)
else:
    print("\n>>> SOME TESTS FAILED! <<<")
    sys.exit(1)
