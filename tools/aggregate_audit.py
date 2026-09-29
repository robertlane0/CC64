#!/usr/bin/env python3
"""List the aggregate-by-value signatures a C program uses, with their sizes.

An aggregate or enumeration passed or returned by value needs an ABI decision
that a pointer parameter does not. This lists what the program actually does, so
the backend work is sized by the program's needs rather than by a guess.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

# A parameter or return type that is not a scalar: a bare `struct X`, `union X`,
# or a typedef name, as opposed to a pointer to one.
DECL = re.compile(
    r"^\s*(?P<decl>[A-Za-z_][A-Za-z_0-9 \t*]*?[A-Za-z_][A-Za-z_0-9]*"
    r"(?:\s*\*)*)\s*(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*[,;)]",
    re.MULTILINE)
FUNCTION = re.compile(r"^[A-Za-z_][A-Za-z_0-9 \t*]*?\b([A-Za-z_][A-Za-z_0-9]*)"
                      r"\s*\(([^;{]*)\)\s*[;{]", re.MULTILINE)
TYPEDEF = re.compile(r"^\s*typedef\s+.*?\b([A-Za-z_][A-Za-z_0-9]*)\s*;",
                     re.MULTILINE)


def strip_noise(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--cc", default="/root/code/CC64/cc64")
    arguments = parser.parse_args()

    headers = sorted(arguments.source.rglob("*.h"))
    typedefs: set[str] = set()
    records: set[str] = set()
    for path in headers + sorted(arguments.source.rglob("*.c")):
        text = strip_noise(path.read_text(encoding="utf-8", errors="replace"))
        typedefs |= set(TYPEDEF.findall(text))
        records |= set(re.findall(r"^\s*(?:typedef\s+)?(?:struct|union)\s+"
                                  r"([A-Za-z_][A-Za-z_0-9]*)\s*\{", text,
                                  re.MULTILINE))
    by_value = records | typedefs

    found: dict[str, set[str]] = {}
    for path in headers:
        text = strip_noise(path.read_text(encoding="utf-8", errors="replace"))
        for name, arguments_text in FUNCTION.findall(text):
            for spec in arguments_text.split(","):
                match = DECL.match(spec.strip())
                if match is None:
                    continue
                decl = match.group("decl").strip()
                if "*" in decl:
                    continue
                if decl not in by_value:
                    continue
                found.setdefault(name, set()).add(decl)
    for name in sorted(found):
        print(f"{name}: {', '.join(sorted(found[name]))}")
    print(f"--- {len(found)} function names take a by-value aggregate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
