#!/usr/bin/env python3
"""List the library functions a C program calls, against a header's names.

The list is derived from the call sites in the sources rather than from a
header, so a function the program uses and the target does not declare shows
up instead of surfacing later as a link failure.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

CALL = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(")
# Words that look like a call but are keywords, macros the program defines
# itself, or the function this script's own header uses.
KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "defined", "case",
    "do", "else", "goto", "break", "continue", "static_assert", "_Alignof",
    "_Static_assert", "alignof", "offsetof", "va_start", "va_end", "va_arg",
    "va_copy", "__attribute__", "__extension__", "__builtin_offsetof",
    "_Generic", "__asm", "asm",
}


def strip_noise(text: str) -> str:
    """Remove comments and literal text, so prose is not read as a call."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(\\.|[^'\\\n])*'", "''", text)
    return text


def declared(header: str) -> set[str]:
    found = set()
    for match in re.finditer(r"^\s*(?:[A-Za-z_][A-Za-z_0-9]*\s+|\*+\s*)*"
                             r"([A-Za-z_][A-Za-z_0-9]*)\s*\(", header, re.MULTILINE):
        found.add(match.group(1))
    for match in re.finditer(r"^#define\s+([A-Za-z_][A-Za-z_0-9]*)",
                             header, re.MULTILINE):
        found.add(match.group(1))
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("header", type=pathlib.Path)
    parser.add_argument("--defined-in", default=None,
                        help="file listing the project's own definitions")
    arguments = parser.parse_args()

    have = KEYWORDS.copy()
    for path in sorted(arguments.source.rglob("*.h")) + \
            sorted(arguments.source.rglob("*.c")):
        have |= declared(strip_noise(path.read_text(encoding="utf-8",
                                                    errors="replace")))
    known = declared(strip_noise(arguments.header.read_text(encoding="utf-8")))

    used: set[str] = set()
    for path in sorted(arguments.source.rglob("*.c")):
        text = strip_noise(path.read_text(encoding="utf-8", errors="replace"))
        used |= set(CALL.findall(text))
    missing = sorted(used - known - have)
    if missing:
        print("called but not declared by the target header:")
        for name in missing:
            print(f"  {name}")
    else:
        print("every called function is declared by the target header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
