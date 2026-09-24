"""Capture the installed Default GUI at fixed QEMU resolutions and compare references."""

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BOOT_TEST = ROOT / "tools" / "boot_reboot_ci.py"


def compare_reference(captured_path, reference_path):
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError as error:
        raise RuntimeError("Pillow is required when --reference-dir is specified") from error

    with Image.open(captured_path) as captured_image, Image.open(reference_path) as reference_image:
        captured = captured_image.convert("RGB")
        reference = reference_image.convert("RGB")
        if captured.size != reference.size:
            raise RuntimeError(
                f"Reference size {reference.size} does not match capture {captured.size}: "
                f"{reference_path}")
        difference = ImageChops.difference(captured, reference)
        mean_absolute_error = sum(ImageStat.Stat(difference).mean) / 3
        close_pixels = sum(
            1 for red, green, blue in difference.getdata()
            if max(red, green, blue) <= 24
        )
        close_fraction = close_pixels / (captured.width * captured.height)
        heatmap = difference.point(lambda value: min(value * 4, 255))
        heatmap.save(captured_path.with_name(captured_path.stem + "-diff.png"))
        Image.blend(reference, captured, 0.5).save(
            captured_path.with_name(captured_path.stem + "-overlay.png"))
        print(f"[COMPARE] {captured.width}x{captured.height}: "
              f"MAE={mean_absolute_error:.2f}/255, "
              f"pixels-within-24={close_fraction:.1%}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-dir", type=Path, default=ROOT / "build" / "esp",
                        help="installer ESP staged by kos-tool build --installer")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build" / "gui-visual",
                        help="directory for QEMU desktop screenshots")
    parser.add_argument("--reference-dir", type=Path,
                        help="optional directory containing <WIDTHxHEIGHT>.png Demoweb captures")
    parser.add_argument("--resolutions", nargs="+", default=("1024x768", "800x600"),
                        help="QEMU desktop sizes to capture (default: 1024x768 800x600)")
    arguments = parser.parse_args()

    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    captures = []
    for resolution in arguments.resolutions:
        capture = arguments.output_dir / f"vino-default-{resolution}.ppm"
        command = [
            sys.executable, str(BOOT_TEST),
            "--esp-dir", str(arguments.esp_dir.resolve()),
            "--profile", "plus",
            "--resolution", resolution,
            "--screenshot", str(capture.resolve()),
            "--ps2-only", "--exercise-pointer",
        ]
        print(f"[VISUAL] Capturing Vino Default at {resolution}", flush=True)
        result = subprocess.run(command, cwd=ROOT)
        if result.returncode != 0:
            return result.returncode
        captures.append((resolution, capture))

    if arguments.reference_dir is None:
        print("[BLOCKED] Captures, input, reboot, and idle-redraw checks passed; "
              "provide viewport-only Demoweb PNGs in --reference-dir to compare pixels.")
        return 2

    missing = [arguments.reference_dir / f"{resolution}.png"
               for resolution, _ in captures
               if not (arguments.reference_dir / f"{resolution}.png").is_file()]
    if missing:
        for reference in missing:
            print(f"[BLOCKED] Missing Demoweb reference screenshot: {reference}")
        return 2

    for resolution, capture in captures:
        compare_reference(capture, arguments.reference_dir / f"{resolution}.png")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
