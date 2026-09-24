"""Run KOS smoke/full QEMU gates with a fail-closed checklist."""

from pathlib import Path
import argparse
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
QEMU = Path(r"C:/Program Files/qemu/qemu-system-x86_64.exe")
OVMF_CODE = Path(r"C:/Program Files/qemu/share/edk2-x86_64-code.fd")
OVMF_VARS_TEMPLATE = Path(r"C:/Program Files/qemu/share/edk2-i386-vars.fd")
VARS = BUILD / "edk2-vars.fd"
KOS_TOOL = BUILD / "kos-tool.exe"


def ensure_host_tool():
    """Build the ignored host helper when it is missing or older than kos.c."""
    source = ROOT / "tools" / "kos.c"
    if KOS_TOOL.exists() and KOS_TOOL.stat().st_mtime >= source.stat().st_mtime:
        return True
    clang = shutil.which("clang") or r"C:/Program Files/LLVM/bin/clang.exe"
    if not Path(clang).exists() and shutil.which("clang") is None:
        print(f"[FAIL] clang not found: {clang}")
        return False
    BUILD.mkdir(exist_ok=True)
    result = subprocess.run([clang, "-O2", str(source), "-o", str(KOS_TOOL)], cwd=ROOT)
    return result.returncode == 0


def prepare_environment():
    BUILD.mkdir(exist_ok=True)
    if not VARS.exists() and OVMF_VARS_TEMPLATE.exists():
        shutil.copyfile(OVMF_VARS_TEMPLATE, VARS)
    required = (QEMU, OVMF_CODE, VARS)
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        print("[FAIL] Missing CI prerequisites:")
        for path in missing:
            print(f"  {path}")
        return False
    return True


def build_image(smoke):
    command = [str(KOS_TOOL), "build"]
    if smoke:
        command.append("--ci-smoke")
    label = "smoke" if smoke else "full"
    print(f"[CI] Building {label} image...")
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        print(f"[FAIL] {label} image build returned {result.returncode}")
        return False
    return True


def qemu_command(smoke):
    cpu_count = "1" if smoke else "4"
    return [
        str(QEMU), "-machine", "q35", "-cpu", "max", "-smp", cpu_count, "-m", "512",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={VARS}",
        "-drive", "format=raw,file=fat:rw:build/esp",
        "-serial", "stdio", "-monitor", "none", "-no-reboot",
        "-netdev", "user,id=kosnet,ipv4=on,ipv6=off,hostfwd=tcp::18080-:80",
        "-device", "e1000,netdev=kosnet,mac=52:54:00:12:34:56",
        "-display", "none",
    ]


def run_qemu(smoke):
    label = "smoke" if smoke else "full"
    log_path = BUILD / f"ci-{label}.log"
    timeout = 25 if smoke else 70
    print(f"[CI] Starting {label} QEMU gate...")
    start = time.time()
    proc = subprocess.Popen(
        qemu_command(smoke), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", cwd=ROOT,
    )
    try:
        output, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = error.output or ""
        proc.kill()
        remaining, _ = proc.communicate()
        output += remaining
    if isinstance(output, bytes):
        output = output.decode("utf-8", errors="replace")
    log_path.write_text(output, encoding="utf-8")
    print(output, end="")
    print(f"[CI] {label} QEMU stopped after {time.time() - start:.1f}s")
    return output


def checklist(label, output):
    common = [
        ("Boot", "KOS booting..."),
        ("VFS multi-volume", "VFS multi-volume path self-test passed."),
        ("DHCP", "DHCP self-test passed"),
        ("TCP", "tcptest: TCP connection closed cleanly."),
        ("Storage block core", ">>> [PASS] BLOCK DEVICE SUBSYSTEM TEST PASSED <<<"),
        ("FAT32", ">>> [PASS] FAT32 FILESYSTEM ENGINE TEST PASSED <<<"),
        ("Installer storage manager", ">>> [PASS] INSTALLER STORAGE MANAGER TEST PASSED <<<"),
        ("User-space file syscalls", ">>> [PASS] USER-SPACE FILE SYSCALL TEST PASSED <<<"),
    ]
    if label == "smoke":
        checks = common + [
            ("Base OS installer RAM test", "[installer-engine] RAM-disk base OS install and commit test passed."),
            ("Smoke completion", "[CI SMOKE] PASS:"),
        ]
    else:
        checks = common + [
            ("Hardware syscall Ring 3", "Hardware syscall Ring 3 execution"),
            ("Ring 3 process suite", "ALL K14 USERSPACE & PROCESS TESTS PASSED"),
            ("Regression suite", "ALL REGRESSION TESTS PASSED"),
            ("ELF fuzzing", "ELF parser survived"),
            ("Syscall fuzzing", "Syscall dispatcher survived"),
            ("Partition fuzzing", "Partition parsers survived"),
            ("FAT32 fuzzing", "FAT32 engine survived"),
            ("Network fuzzing", "Network stack processed"),
            ("ASN.1/X.509 fuzzing", "ASN.1 / X.509 parsers survived"),
            ("TLS fuzzing", "TLS record layer survived"),
            ("Fault injection", "ALL FAULT INJECTION TESTS PASSED"),
            ("Multi-core stress", "concurrent threads completed"),
            ("Filesystem stress", "Filesystem completed"),
            ("Socket stress", "Socket subsystem completed"),
            ("Memory churn stress", "Memory churn completed"),
            ("Stress resource cleanup", "Zero resource leaks in 'stress_suite'"),
            ("Master QA result", "ALL K27 QA, FUZZ, FAULT & STRESS SUITES PASSED"),
        ]

    print("=" * 60)
    print(f"  KOS {label.capitalize()} CI Checklist")
    print("=" * 60)
    all_ok = True
    checklist_lines = []
    for name, marker in checks:
        ok = marker in output
        all_ok = ok and all_ok
        status = "PASS" if ok else "FAIL"
        line = f"[{status}] {name}"
        checklist_lines.append(line)
        print(f"  {line}")
    (BUILD / f"ci-{label}-checklist.txt").write_text(
        "\n".join(checklist_lines) + "\n", encoding="utf-8"
    )
    return all_ok


def run_mode(label):
    smoke = label == "smoke"
    if not ensure_host_tool() or not prepare_environment():
        return False
    if not build_image(smoke):
        return False
    output = run_qemu(smoke)
    passed = checklist(label, output)
    print(
        f"\n[CI RESULT] {label.upper()} TESTS PASSED SUCCESSFULLY! (Code 0)"
        if passed else
        f"\n[CI RESULT] {label.upper()} TEST FAILED! (Code 1)"
    )
    return passed


def run_storage():
    script = ROOT / "tools" / "storage_ci.py"
    print("[CI] Starting storage integration gate...")
    result = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("smoke", "full", "all"), default="full")
    args = parser.parse_args()

    if args.mode == "all":
        smoke_ok = run_mode("smoke")
        full_ok = run_mode("full")
        storage_ok = full_ok and run_storage()
        ok = smoke_ok and full_ok and storage_ok
        print("\n[CI RESULT] ALL KOS QA GATES PASSED" if ok else "\n[CI RESULT] KOS QA GATES FAILED")
        return 0 if ok else 1
    return 0 if run_mode(args.mode) else 1


if __name__ == "__main__":
    sys.exit(main())
