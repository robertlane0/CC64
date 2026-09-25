#!/usr/bin/env python3
"""Compile every production compiler translation unit for the target profile."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT / "src"
INCLUDE_PATHS = [ROOT / "include" / "target", ROOT / "include" / "cc64", ROOT / "src"]


def production_sources() -> list[pathlib.Path]:
    return sorted(
        path for path in SOURCE_ROOT.rglob("*.c")
        if "runtime" not in path.parts and "selfhost" not in path.parts
    )


def main() -> int:
    sources = production_sources()
    if not sources:
        raise SystemExit("self-host probe found no production sources")
    with tempfile.TemporaryDirectory(prefix="cc64-selfhost-probe-") as temp:
        work = pathlib.Path(temp)
        for index, source in enumerate(sources):
            output = work / f"unit-{index}.cc64o"
            command = [str(ROOT / "cc64"), "-c"]
            for path in INCLUDE_PATHS:
                command.extend(["-I", str(path)])
            command.extend([str(source), "-o", str(output)])
            result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                    text=True, check=False)
            if result.returncode != 0:
                sys_output = result.stdout + result.stderr
                raise SystemExit(f"target profile rejected {source.relative_to(ROOT)}:\n{sys_output}")
    print(f"selfhost-probe: compiled {len(sources)} production translation units")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
