#!/usr/bin/env python3
"""Compile the c-edit program with CC64 and report the first blocker per unit.

The program's own sources are not modified. This reports, for every unit, the
first diagnostic CC64 produces, so the work is a list of what the compiler or
the target library still lacks rather than a search through a build log.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

CC64 = pathlib.Path(__file__).resolve().parents[1] / "cc64"
TARGET_INCLUDE = pathlib.Path("/root/code/CC64/include/target")
CC64_INCLUDE = pathlib.Path("/root/code/CC64/include/cc64")
SOURCE = pathlib.Path("/root/code/c-edit")
INCLUDES = ["-I", str(SOURCE / "c/include"),
            "-I", str(SOURCE / "c/src"),
            "-I", str(TARGET_INCLUDE), "-I", str(CC64_INCLUDE)]


def units() -> list[pathlib.Path]:
    result = sorted((SOURCE / "c/src").glob("*.c"))
    result += sorted((SOURCE / "c/src/bin").glob("*.c"))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=None,
                        help="compile just this unit, by base name")
    parser.add_argument("--keep", action="store_true",
                        help="keep the objects in a fixed directory")
    arguments = parser.parse_args()
    chosen = units()
    if arguments.only:
        chosen = [path for path in chosen if path.stem == arguments.only]
        if not chosen:
            raise SystemExit(f"no such unit: {arguments.only}")
    output = pathlib.Path("/tmp/opencode/editbuild")
    output.mkdir(parents=True, exist_ok=True)
    clean = []
    blocked = []
    for source in chosen:
        obj = output / f"{source.stem}.cc64o"
        result = subprocess.run(
            [str(CC64), *INCLUDES, "-c", str(source), "-o", str(obj)],
            capture_output=True, text=True, check=False, timeout=120)
        if result.returncode == 0:
            clean.append(source.stem)
            continue
        detail = (result.stdout + result.stderr).strip().splitlines()
        first = detail[0] if detail else "(no diagnostic)"
        blocked.append((source.stem, first))
    print(f"compiled {len(clean)} of {len(chosen)} units")
    if clean:
        print("  ok: " + " ".join(clean))
    for name, first in blocked:
        print(f"  {name}: {first}")
    return 0 if not blocked else 1


if __name__ == "__main__":
    raise SystemExit(main())
