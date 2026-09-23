"""Boot KOS against synthetic MBR and GPT disks on AHCI, VirtIO, and NVMe."""

import os
import shutil
import struct
import subprocess
import sys
import zlib

QEMU = r"C:/Program Files/qemu/qemu-system-x86_64.exe"
OVMF_CODE = r"C:/Program Files/qemu/share/edk2-x86_64-code.fd"
OVMF_VARS_TEMPLATE = r"C:/Program Files/qemu/share/edk2-i386-vars.fd"
SECTOR_SIZE = 512
SECTOR_COUNT = 32768


def write_sector(image, lba, data):
    image[lba * SECTOR_SIZE:(lba + 1) * SECTOR_SIZE] = data


def write_mbr(path):
    image = bytearray(SECTOR_COUNT * SECTOR_SIZE)
    mbr = bytearray(SECTOR_SIZE)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0\0\0", 0x0C, b"\0\0\0", 2048, 8192)
    struct.pack_into("<H", mbr, 510, 0xAA55)
    write_sector(image, 0, mbr)
    with open(path, "wb") as disk:
        disk.write(image)


def write_gpt(path):
    image = bytearray(SECTOR_COUNT * SECTOR_SIZE)
    protective_mbr = bytearray(SECTOR_SIZE)
    struct.pack_into("<B3sB3sII", protective_mbr, 446, 0, b"\0\0\0", 0xEE, b"\0\0\0", 1, SECTOR_COUNT - 1)
    struct.pack_into("<H", protective_mbr, 510, 0xAA55)
    write_sector(image, 0, protective_mbr)

    entries = bytearray(128 * 128)
    basic_data_guid = bytes((0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                            0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7))
    unique_guid = bytes(range(1, 17))
    struct.pack_into("<16s16sQQQ", entries, 0, basic_data_guid, unique_guid, 2048, 10239, 0)
    image[2 * SECTOR_SIZE:2 * SECTOR_SIZE + len(entries)] = entries

    header = bytearray(SECTOR_SIZE)
    struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART", 0x00010000, 92, 0, 0,
                     1, SECTOR_COUNT - 1, 34, SECTOR_COUNT - 34, bytes(range(16)),
                     2, 128, 128, zlib.crc32(entries) & 0xFFFFFFFF)
    struct.pack_into("<I", header, 16, zlib.crc32(header[:92]) & 0xFFFFFFFF)
    write_sector(image, 1, header)
    with open(path, "wb") as disk:
        disk.write(image)


def require(path, description):
    if not os.path.exists(path):
        raise RuntimeError(f"Missing {description}: {path}")


def run_case(kind, image_path):
    vars_path = "build/storage-ci-vars.fd"
    log_path = f"build/storage-ci-{kind}.log"
    shutil.copyfile(OVMF_VARS_TEMPLATE, vars_path)
    command = [
        QEMU, "-machine", "q35", "-cpu", "max", "-m", "512",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={vars_path}",
        "-drive", "format=raw,file=fat:rw:build/esp",
        "-drive", f"if=none,format=raw,snapshot=on,file={image_path},id=ahci_data",
        "-device", "ich9-ahci,id=ahci_ci",
        "-device", "ide-hd,drive=ahci_data,bus=ahci_ci.0",
        "-drive", f"if=none,format=raw,snapshot=on,file={image_path},id=virtio_data",
        "-device", "virtio-blk-pci-transitional,drive=virtio_data,queue-size=128",
        "-drive", f"if=none,format=raw,snapshot=on,file={image_path},id=nvme_data",
        "-device", "nvme,drive=nvme_data,serial=KOSSTORAGE",
        "-serial", "stdio", "-monitor", "none", "-no-reboot", "-display", "none",
    ]
    print(f"Starting {kind.upper()} storage integration test...")
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    try:
        full_log, _ = proc.communicate(timeout=50)
    except subprocess.TimeoutExpired:
        proc.kill()
        full_log, _ = proc.communicate()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    with open(log_path, "w", encoding="utf-8") as log_file:
        log_file.write(full_log)

    passed = ">>> [PASS] STORAGE BACKEND INTEGRATION TEST PASSED <<<" in full_log
    required = (
        "[storage][ahci] Detected SATA drive",
        "[storage][virtio] Registered",
        "[storage][nvme] Registered nvme0n1",
        "Device: nvme0n1",
        "Type: NVME",
        f"{kind.upper()} table",
    )
    missing = [entry for entry in required if entry not in full_log]
    if not passed or missing:
        print(f"[FAIL] {kind.upper()} storage integration test")
        if missing:
            print("Missing log markers:", ", ".join(missing))
        return False
    print(f"[PASS] {kind.upper()} storage integration test")
    return True


def main():
    try:
        require(QEMU, "QEMU")
        require(OVMF_CODE, "OVMF code firmware")
        require(OVMF_VARS_TEMPLATE, "OVMF variables firmware")
        require("build/esp/boot/kos.elf", "built KOS ESP")
    except RuntimeError as error:
        print(error)
        return 1

    os.makedirs("build", exist_ok=True)
    mbr_image = "build/storage-mbr.img"
    gpt_image = "build/storage-gpt.img"
    write_mbr(mbr_image)
    write_gpt(gpt_image)

    ok = run_case("mbr", mbr_image)
    ok = run_case("gpt", gpt_image) and ok
    print("[CI RESULT] Storage backend integration passed" if ok else "[CI RESULT] Storage backend integration failed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
