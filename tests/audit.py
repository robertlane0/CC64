#!/usr/bin/env python3
"""Check CC64 repository provenance and generated-file hygiene."""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FORBIDDEN_PARTS = {
    ".git", "build", "MS-DOS64", "nasm", "__pycache__", ".cache",
}
FORBIDDEN_SUFFIXES = {
    ".o", ".obj", ".a", ".so", ".dll", ".exe", ".com", ".mz64", ".pyc",
}
EXTERNAL_COMPILER_NAMES = re.compile(
    r"\b(?:gcc|clang|chibicc|8cc|lacc|tcc|pycc)\b", re.IGNORECASE
)
EXTERNAL_URL = re.compile(r"github\.com/.+/(?:chibicc|8cc|lacc|pcc|cc64-compiler)", re.I)


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [item.decode() for item in result.stdout.split(b"\0") if item]


def main() -> int:
    errors: list[str] = []
    if os.environ.get("CC64_REQUIRE_CLEAN") == "1":
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=ROOT, check=False, capture_output=True, text=True,
        )
        if status.returncode != 0:
            errors.append("cannot inspect source-tree status")
        elif status.stdout.strip():
            errors.append("release audit requires a clean tracked and untracked tree")
    files = tracked_files()
    if not files:
        errors.append("no tracked files")

    for name in files:
        path = ROOT / name
        if any(part in FORBIDDEN_PARTS for part in pathlib.PurePath(name).parts):
            errors.append(f"forbidden tracked path: {name}")
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            errors.append(f"generated or binary file tracked: {name}")
        if path.is_symlink():
            errors.append(f"symlink tracked: {name}")
        if path.suffix.lower() not in {".md", ".c", ".h", ".py", ".s", ""} and name != "Makefile":
            errors.append(f"unexpected tracked source type: {name}")

    ledger = (ROOT / "docs/provenance-ledger.md").read_text(encoding="utf-8")
    for required in ("C17", "Intel SDM", "System V AMD64", "MS-DOS64"):
        if required not in ledger:
            errors.append(f"provenance ledger omits {required}")

    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    if "! -path 'src/runtime/*'" not in makefile:
        errors.append("host bootstrap source list includes target runtime sources")
    status = (ROOT / "docs/status.md").read_text(encoding="utf-8")
    for required in ("M7 self-hosting | partial", "M8 release quality | partial",
                     "Bochs", "not a releasable"):
        if required not in status:
            errors.append(f"status omits release limitation: {required}")

    for name in files:
        if not name.endswith((".c", ".h", ".py", ".md")):
            continue
        text = (ROOT / name).read_text(encoding="utf-8")
        if EXTERNAL_URL.search(text):
            errors.append(f"external compiler repository reference: {name}")
        if name.startswith("docs/") or name in {"AGENTS.md", "tests/audit.py"}:
            continue
        if EXTERNAL_COMPILER_NAMES.search(text):
            errors.append(f"external compiler name requires review: {name}")

    if errors:
        for error in errors:
            print(f"audit: {error}", file=sys.stderr)
        return 1
    print(f"audit: {len(files)} tracked files passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
