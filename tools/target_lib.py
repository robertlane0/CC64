#!/usr/bin/env python3
"""Compile the target C library with CC64 and report the first failure.

The library is target-only source: it is never part of the host build, and it
is compiled here with the target header profile so that a defect in either the
library or the compiler is reported before the self-host link is attempted.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "src" / "runtime"
INCLUDE_PATHS = [ROOT / "include" / "target", ROOT / "include" / "cc64", ROOT / "src"]


def library_sources() -> list[pathlib.Path]:
    return sorted(RUNTIME.glob("target_*.c"))


def main() -> int:
    sources = library_sources()
    if not sources:
        raise SystemExit("target library: no sources found")
    with tempfile.TemporaryDirectory(prefix="cc64-target-lib-") as temp:
        work = pathlib.Path(temp)
        for index, source in enumerate(sources):
            output = work / f"lib-{index}.cc64o"
            command = [str(ROOT / "cc64"), "-c"]
            for path in INCLUDE_PATHS:
                command += ["-I", str(path)]
            command += [str(source), "-o", str(output)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                    text=True, check=False)
            if result.returncode != 0:
                print(f"target library: {source.name} rejected",
                      file=sys.stderr)
                print(result.stdout + result.stderr, file=sys.stderr)
                return 1
    print(f"target-lib: compiled {len(sources)} library translation units")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
