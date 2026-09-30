#!/usr/bin/env python3
"""The source groups the build and the self-host gates use.

Three groups matter and they are not the same group. The compiler's own
translation units are everything under `src` that is not the target library.
The target library is every unit under `src/runtime`, and a program built for
the target links all of it. The compiler, however, links only the part of the
target library it itself calls, and that part is smaller: the terminal, file,
memory-mapping, dynamic-loading, and arithmetic units exist for a program the
compiler builds, not for the compiler.

The compiler image is built from the smaller set because the target's heap is
six mebibytes in total, and an image carrying a terminal library it never calls
takes that heap away from the work the compiler has to do. The split is checked
rather than trusted: the self-host stage links the compiler image from this set
and an unresolved symbol names any unit that belongs in it and is not listed.
"""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "src" / "runtime"

# The units the compiler's own code calls: the allocator, the character
# classes, the formatted output, the file streams, the file calls, the numeric
# conversions, and the string routines. Everything else in the library answers an
# interface a program written for a hosted system uses.
#
# The file calls belong here because the compiler opens files to read them, and
# because the stream unit resolves a path through them: a program that opens a
# file by an absolute path would be refused a name the target does have, and
# the code that resolves the path is the file unit's. This is a set of units
# rather than a set of interfaces, so a unit another listed unit depends on is
# listed too. The self-host stage links the image from this set, and an
# unresolved symbol is what says the list has been left behind by a change.
COMPILER_LIBRARY = (
    "target_alloc.c",
    "target_ctype.c",
    "target_file.c",
    "target_format.c",
    "target_stdio.c",
    "target_stdlib.c",
    "target_string.c",
)


def production_sources() -> list[pathlib.Path]:
    """Every translation unit of the compiler itself."""
    return sorted(
        path for path in (ROOT / "src").rglob("*.c")
        if "runtime" not in path.parts and "selfhost" not in path.parts
    )


def compiler_library_sources() -> list[pathlib.Path]:
    """The part of the target library the compiler itself calls."""
    return [RUNTIME / name for name in COMPILER_LIBRARY]


def target_library_sources() -> list[pathlib.Path]:
    """Every unit of the target library, for a program built for the target."""
    return sorted(RUNTIME.glob("target_*.c"))


if __name__ == "__main__":
    missing = [name for name in COMPILER_LIBRARY
               if not (RUNTIME / name).exists()]
    if missing:
        raise SystemExit(f"missing target library units: {missing}")
    print(f"production: {len(production_sources())} units")
    print(f"compiler library: {len(compiler_library_sources())} units")
    print(f"target library: {len(target_library_sources())} units")
