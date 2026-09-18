from pathlib import Path
from collections import defaultdict

# Các đuôi file được tính là code
CODE_EXTENSIONS = {
    ".c", ".h", ".cpp", ".hpp", ".cc",
    ".rs",
    ".py",
    ".js", ".ts", ".jsx", ".tsx",
    ".java",
    ".go",
    ".cs",
    ".php",
    ".rb",
    ".swift",
    ".kt", ".kts",
    ".asm", ".s",
    ".html", ".css",
    ".sh",
}

# Các thư mục bỏ qua
IGNORE_DIRS = {
    ".git",
    "node_modules",
    "target",
    "build",
    "dist",
    ".idea",
    ".vscode",
    "__pycache__",
}

root = Path.cwd()

total_lines = 0
total_files = 0

stats = defaultdict(lambda: {"files": 0, "lines": 0})

for file in root.rglob("*"):
    if not file.is_file():
        continue

    if any(part in IGNORE_DIRS for part in file.parts):
        continue

    ext = file.suffix.lower()

    if ext not in CODE_EXTENSIONS:
        continue

    try:
        with file.open("r", encoding="utf-8", errors="ignore") as f:
            lines = sum(1 for _ in f)

        total_lines += lines
        total_files += 1

        stats[ext]["files"] += 1
        stats[ext]["lines"] += lines

    except Exception as e:
        print(f"Lỗi: {file}: {e}")

print("=" * 50)
print(f"Thư mục: {root}")
print(f"Tổng file code: {total_files:,}")
print(f"Tổng dòng code: {total_lines:,}")
print("=" * 50)

for ext, data in sorted(
    stats.items(),
    key=lambda x: x[1]["lines"],
    reverse=True
):
    print(
        f"{ext:<8} "
        f"{data['files']:>6} file | "
        f"{data['lines']:>12,} dòng"
    )

print("=" * 50)