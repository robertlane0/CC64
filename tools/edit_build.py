#!/usr/bin/env python3
"""Build the c-edit program with CC64, link it, and place it on a target volume.

The program's own sources are not modified. The build reports, for every unit,
the first diagnostic CC64 produces, so the work is a list of what the compiler
or the target library still lacks rather than a search through a build log.
When every unit compiles, the objects are linked with the target library and
the image is written where the target's own tools can run it.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

CC64 = pathlib.Path(__file__).resolve().parents[1] / "cc64"
ROOT = CC64.parent
TARGET_INCLUDE = ROOT / "include/target"
CC64_INCLUDE = ROOT / "include/cc64"
RUNTIME = ROOT / "src/runtime"
SOURCE = pathlib.Path("/root/code/c-edit")
DOS64 = SOURCE.parent / "MS-DOS64"
INCLUDES = ["-I", str(SOURCE / "c/include"),
            "-I", str(SOURCE / "c/src"),
            "-I", str(TARGET_INCLUDE), "-I", str(CC64_INCLUDE),
            "-I", str(ROOT / "src")]
OUTPUT = pathlib.Path("/tmp/opencode/editbuild")


def units() -> list[pathlib.Path]:
    result = sorted((SOURCE / "c/src").glob("*.c"))
    result += sorted((SOURCE / "c/src/bin").glob("*.c"))
    return result


def run(command: list[str], timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          timeout=timeout)


def compile_units(chosen: list[pathlib.Path]) -> tuple[list[pathlib.Path], list[tuple[str, str]]]:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    clean: list[pathlib.Path] = []
    blocked: list[tuple[str, str]] = []
    for source in chosen:
        obj = OUTPUT / f"{source.stem}.cc64o"
        result = run([str(CC64), *INCLUDES, "-c", str(source), "-o", str(obj)], 120)
        if result.returncode == 0:
            clean.append(obj)
            continue
        detail = (result.stdout + result.stderr).strip().splitlines()
        blocked.append((source.stem, detail[0] if detail else "(no diagnostic)"))
    return clean, blocked


def build_library() -> list[pathlib.Path]:
    library = []
    for index, source in enumerate(sorted(RUNTIME.glob("target_*.c"))):
        obj = OUTPUT / f"target-lib-{index}.cc64o"
        result = run([str(CC64), *INCLUDES, "-c", str(source), "-o", str(obj)])
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip().splitlines()
            raise SystemExit(f"target library: {source.name} rejected: "
                             f"{detail[0] if detail else ''}")
        library.append(obj)
    return library


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=None,
                        help="compile just this unit, by base name")
    parser.add_argument("--image", default="EDIT.COM",
                        help="image name the loader is given")
    # The target library holds a table of pointers, so an image that includes
    # it is linked in the load-biased form; the raw form is kept for a program
    # that needs no data pointers at all.
    parser.add_argument("--format", default="mz64", choices=["raw", "mz64"],
                        help="executable form to link")
    parser.add_argument("--volume", default=None,
                        help="target volume image to place the program on")
    arguments = parser.parse_args()
    chosen = units()
    if arguments.only:
        chosen = [path for path in chosen if path.stem == arguments.only]
        if not chosen:
            raise SystemExit(f"no such unit: {arguments.only}")

    clean, blocked = compile_units(chosen)
    print(f"compiled {len(clean)} of {len(chosen)} units")
    if clean:
        print("  ok: " + " ".join(path.stem for path in clean))
    for name, first in blocked:
        print(f"  {name}: {first}")
    if blocked:
        return 1
    if arguments.only:
        return 0

    library = build_library()
    image = OUTPUT / arguments.image
    command = [str(CC64), "--link"]
    if arguments.format == "mz64":
        command += ["--format", "mz64"]
    command += [str(path) for path in clean] + [str(path) for path in library]
    command += ["-o", str(image)]
    result = run(command)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip().splitlines()
        print("  link: " + (detail[0] if detail else "(no diagnostic)"))
        return 1
    size = image.stat().st_size
    print(f"linked {arguments.image} ({size} bytes) from {len(clean)} units "
          f"and {len(library)} library units")

    if arguments.volume:
        disk = pathlib.Path(arguments.volume)
        result = run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                      str(disk), str(image), pathlib.Path(arguments.image).stem])
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip().splitlines()
            print("  volume: " + (detail[-1] if detail else "(no diagnostic)"))
            return 1
        print(f"placed on {disk}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
