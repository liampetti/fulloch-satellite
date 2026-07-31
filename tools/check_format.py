#!/usr/bin/env python3
"""Reject whitespace-only formatting drift in tracked source and documentation."""

import subprocess
import sys
from pathlib import Path


SUFFIXES = {".c", ".h", ".md", ".py", ".json", ".yml", ".yaml", ".cmake"}
NAMES = {"CMakeLists.txt"}


def main() -> int:
    files = subprocess.check_output(["git", "ls-files"], text=True).splitlines()
    failures = []
    for name in files:
        path = Path(name)
        if path.suffix not in SUFFIXES and path.name not in NAMES:
            continue
        data = path.read_bytes()
        if data and not data.endswith(b"\n"):
            failures.append(f"{name}: missing final newline")
        for line_number, line in enumerate(data.splitlines(), start=1):
            if line.rstrip(b" \t") != line:
                failures.append(f"{name}:{line_number}: trailing whitespace")
    if failures:
        print("Formatting check failed:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
