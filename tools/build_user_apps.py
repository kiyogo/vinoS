#!/usr/bin/env python3
"""Build the freestanding user-space installer frontend ELF."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def build_installer(clang: str, linker: str, output_dir: Path) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / "installer.elf"
    sources = [
        ROOT / "user" / "apps" / "installer" / "main.c",
        ROOT / "user" / "lib" / "installer.c",
        ROOT / "user" / "lib" / "gui.c",
        ROOT / "user" / "lib" / "widgets.c",
        ROOT / "user" / "lib" / "memory.c",
    ]
    objects = []
    for index, source in enumerate(sources):
        object_path = output_dir / f"installer.{index}.o"
        subprocess.run([
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
            "-c",
            str(source),
            "-o",
            str(object_path),
        ], cwd=ROOT, check=True)
        objects.append(str(object_path))
    subprocess.run([
        linker, "-static", "-e", "_start", "-o", str(output), *objects,
    ], cwd=ROOT, check=True)
    image = output.read_bytes()
    if len(image) < 64 or image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise ValueError("installer frontend did not produce an ELF64 little-endian image")
    elf_type, machine = struct.unpack_from("<HH", image, 16)
    entry = struct.unpack_from("<Q", image, 24)[0]
    if elf_type != 2 or machine != 62 or entry < 0x00200000:
        raise ValueError("installer frontend must be an x86_64 ET_EXEC with a user entry point")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--lld", required=True)
    parser.add_argument("--output", default="build/user-apps")
    args = parser.parse_args()
    output_dir = (ROOT / args.output).resolve()
    output = build_installer(args.clang, args.lld, output_dir)
    print(f"Built {output.relative_to(ROOT)} ({output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"User app build failed: {error}")
