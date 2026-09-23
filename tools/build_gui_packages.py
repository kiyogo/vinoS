#!/usr/bin/env python3
"""Build reproducible Vino GUI KPKG packages and detached SHA-256 metadata."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HEADER_FORMAT = "<IIIIQIIQIIQQQII"
MANIFEST_FORMAT = "<64s64s32sI16s256sIII512s"
FILE_FORMAT = "<256sQQI32s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MANIFEST_SIZE = struct.calcsize(MANIFEST_FORMAT)
FILE_SIZE = struct.calcsize(FILE_FORMAT)
KPKG_MAGIC = 0x47504B4B
KPKG_VERSION = 1
KPKG_SIG_NONE = 0
MAX_FILE_SIZE = 16 * 1024 * 1024
MAX_TOTAL_SIZE = 64 * 1024 * 1024


def read_descriptor(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or key in fields:
            raise ValueError(f"invalid or duplicate descriptor field in {path}: {line!r}")
        fields[key] = value
    required = {
        "id", "name", "version", "app_id", "entry", "architecture",
        "install_root", "integrity", "signature",
    }
    missing = required - fields.keys()
    if missing:
        raise ValueError(f"{path} is missing fields: {', '.join(sorted(missing))}")
    if fields["integrity"] != "sha256" or fields["signature"] != "none":
        raise ValueError(f"{path} must explicitly declare SHA-256 and unsigned signature status")
    if fields["architecture"] != "x86_64":
        raise ValueError(f"{path} architecture must match the x86_64 package builder")
    if fields["install_root"] != f"system/gui/{fields['id']}":
        raise ValueError(f"{path} install_root must be in the system/gui namespace")
    return fields


def fixed_string(value: str, size: int, field: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        raise ValueError(f"{field} exceeds its KPKG field size")
    return encoded.ljust(size, b"\0")


def version_code(version: str) -> int:
    parts = version.split(".")
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
        raise ValueError(f"version must use MAJOR.MINOR.PATCH: {version}")
    major, minor, patch = (int(part) for part in parts)
    if major > 9999 or minor > 99 or patch > 99:
        raise ValueError(f"version is outside the KPKG version-code range: {version}")
    return major * 10000 + minor * 100 + patch


def compile_entry(clang: str, linker: str, source: Path, output: Path, name: str) -> bytes:
    output.parent.mkdir(parents=True, exist_ok=True)
    object_paths = []
    sources = [
        source,
        ROOT / "user" / "lib" / "gui.c",
        ROOT / "user" / "lib" / "widgets.c",
        ROOT / "user" / "lib" / "memory.c",
    ]
    for index, input_source in enumerate(sources):
        object_path = output.with_name(f"{output.stem}.{index}.o")
        compile_command = [
            clang,
            "--target=x86_64-unknown-none-elf",
            "-std=c17",
            "-O2",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-pic",
            "-fno-pie",
            "-fno-builtin",
            "-mno-mmx",
            "-mno-sse",
            "-mno-80387",
            "-Iinclude",
            "-Iuser/include",
            f'-DGUI_PACKAGE_NAME="{name}"',
            "-c",
            str(input_source),
            "-o",
            str(object_path),
        ]
        subprocess.run(compile_command, cwd=ROOT, check=True)
        object_paths.append(str(object_path))
    subprocess.run([
        linker,
        "-static",
        "-e",
        "_start",
        "-o",
        str(output),
        *object_paths,
    ], cwd=ROOT, check=True)
    data = output.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"compiler output is not an ELF64 little-endian image: {output}")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    entry = struct.unpack_from("<Q", data, 24)[0]
    if elf_type != 2 or machine != 62 or entry == 0:
        raise ValueError(f"GUI entry must be an x86_64 ET_EXEC with an entry point: {output}")
    return data


def build_package(fields: dict[str, str], entry_data: bytes) -> bytes:
    entry_path = fields["entry"]
    manifest_text = (
        "format=VINO_GUI_PACKAGE_V1\n"
        f"id={fields['id']}\n"
        f"name={fields['name']}\n"
        f"version={fields['version']}\n"
        f"version_code={version_code(fields['version'])}\n"
        f"architecture={fields['architecture']}\n"
        f"entry_point={entry_path}\n"
        f"namespace=/{fields['install_root']}\n"
        "checksum_algorithm=SHA-256\n"
        "signature_type=none\n"
        "signature_status=unsigned\n"
        "ownership_file=share/OWNERSHIP.TXT\n"
    ).encode("ascii")
    ownership_text = (
        f"package_id={fields['app_id']}\n"
        f"namespace=/{fields['install_root']}\n"
        "owner=package\n"
        f"file={entry_path}\n"
        "file=share/GUI.MANIFEST\n"
        "file=share/OWNERSHIP.TXT\n"
        "file=share/SHA256SUMS.TXT\n"
    ).encode("ascii")

    files: list[tuple[str, bytes, int]] = [
        (entry_path, entry_data, 0o100755),
        ("share/GUI.MANIFEST", manifest_text, 0o100644),
        ("share/OWNERSHIP.TXT", ownership_text, 0o100644),
    ]
    checksums = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {path}\n" for path, data, _ in files
    ).encode("ascii")
    files.append(("share/SHA256SUMS.TXT", checksums, 0o100644))

    payload = bytearray()
    index = bytearray()
    for path, data, mode in files:
        if len(data) > MAX_FILE_SIZE:
            raise ValueError(f"package member exceeds 16 MiB: {path}")
        offset = len(payload)
        index.extend(struct.pack(
            FILE_FORMAT,
            fixed_string(path, 256, "file path"),
            len(data),
            offset,
            mode,
            hashlib.sha256(data).digest(),
        ))
        payload.extend(data)
    if len(payload) > MAX_TOTAL_SIZE:
        raise ValueError("package payload exceeds 64 MiB")

    manifest = struct.pack(
        MANIFEST_FORMAT,
        fixed_string(fields["app_id"], 64, "app_id"),
        fixed_string(fields["name"], 64, "name"),
        fixed_string(fields["version"], 32, "version"),
        version_code(fields["version"]),
        fixed_string(fields["architecture"], 16, "architecture"),
        fixed_string(entry_path, 256, "entry_point"),
        0,  # no privileged capabilities required
        1,  # minimum OS version 0.1
        0,  # dependency count
        bytes(8 * 64),
    )
    if len(manifest) != MANIFEST_SIZE:
        raise AssertionError("Python KPKG manifest layout disagrees with the C ABI")

    manifest_offset = HEADER_SIZE
    index_offset = manifest_offset + len(manifest)
    payload_offset = index_offset + len(index)
    signature_offset = payload_offset + len(payload)
    header = struct.pack(
        HEADER_FORMAT,
        KPKG_MAGIC,
        KPKG_VERSION,
        HEADER_SIZE,
        KPKG_SIG_NONE,
        manifest_offset,
        len(manifest),
        len(files),
        index_offset,
        len(index),
        0,
        payload_offset,
        len(payload),
        signature_offset,
        0,
        0,
    )
    package = header + manifest + index + payload
    if len(package) != signature_offset:
        raise AssertionError("KPKG offsets do not cover the complete generated package")
    validate_package(package, fields)
    return package


def validate_package(package: bytes, fields: dict[str, str]) -> None:
    values = struct.unpack_from(HEADER_FORMAT, package)
    (magic, version, header_size, signature_type, manifest_offset, manifest_size,
     file_count, index_offset, index_size, _, payload_offset, payload_size,
     signature_offset, signature_size, _) = values
    if (magic, version, header_size, signature_type) != (
        KPKG_MAGIC, KPKG_VERSION, HEADER_SIZE, KPKG_SIG_NONE
    ):
        raise ValueError("generated package header is invalid")
    if manifest_size != MANIFEST_SIZE or index_size != file_count * FILE_SIZE:
        raise ValueError("generated package manifest or ownership index has invalid size")
    if manifest_offset + manifest_size != index_offset:
        raise ValueError("generated package offsets are inconsistent")
    if index_offset + index_size != payload_offset:
        raise ValueError("generated package index does not lead to its payload")
    if payload_offset + payload_size != signature_offset or signature_size != 0:
        raise ValueError("generated package payload/signature metadata is inconsistent")

    payload = package[payload_offset:payload_offset + payload_size]
    seen: set[str] = set()
    for index in range(file_count):
        offset = index_offset + index * FILE_SIZE
        path_field, size, member_offset, _, expected_hash = struct.unpack_from(FILE_FORMAT, package, offset)
        path = path_field.split(b"\0", 1)[0].decode("utf-8")
        if path in seen or not path or member_offset + size > len(payload):
            raise ValueError("generated package has a duplicate or out-of-range ownership entry")
        seen.add(path)
        member = payload[member_offset:member_offset + size]
        if hashlib.sha256(member).digest() != expected_hash:
            raise ValueError(f"generated package SHA-256 mismatch: {path}")
    if fields["entry"] not in seen or "share/OWNERSHIP.TXT" not in seen:
        raise ValueError("generated package is missing its ELF entry or ownership list")


def build_one(clang: str, linker: str, output_dir: Path,
              package_id: str) -> tuple[Path, Path]:
    descriptor_path = ROOT / "assets" / "gui" / f"{package_id.upper()}.MANIFEST"
    fields = read_descriptor(descriptor_path)
    if fields["id"] != package_id:
        raise ValueError(f"descriptor ID mismatch: {descriptor_path}")

    entry_path = fields["entry"]
    if not entry_path.startswith("bin/") or ".." in Path(entry_path).parts:
        raise ValueError(f"entry point must be a safe bin/ path: {entry_path}")
    app_dir = output_dir / package_id
    elf_path = app_dir / Path(entry_path)
    entry_data = compile_entry(
        clang,
        linker,
        ROOT / "assets" / "gui" / "gui_package_app.c",
        elf_path,
        fields["name"],
    )
    package = build_package(fields, entry_data)
    output_dir.mkdir(parents=True, exist_ok=True)
    package_path = output_dir / f"{package_id.upper()}.KPKG"
    checksum_path = output_dir / f"{package_id.upper()}.KPKG.SHA256"
    package_path.write_bytes(package)
    checksum_path.write_text(hashlib.sha256(package).hexdigest() + "\n", encoding="ascii")
    return package_path, checksum_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--lld", required=True)
    parser.add_argument("--output", default="build/gui-packages")
    args = parser.parse_args()
    output_dir = (ROOT / args.output).resolve()
    for package_id in ("default", "mvp"):
        package_path, checksum_path = build_one(args.clang, args.lld,
                                                output_dir, package_id)
        print(f"Built {package_path.relative_to(ROOT)} ({package_path.stat().st_size} bytes)")
        print(f"SHA-256 metadata: {checksum_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"GUI package build failed: {error}")
