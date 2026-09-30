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
import re
import shutil
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
    parser.add_argument("--run", action="store_true",
                        help="boot the image on the target and report what it did")
    parser.add_argument("--run-for", type=float, default=30.0,
                        help="seconds to let the program run before stopping it")
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
    if arguments.run:
        return boot(arguments.image, arguments.run_for)
    return 0


def boot(image: str, seconds: float) -> int:
    """Run the image on the target and report what it did.

    The claim this exists for is that the program starts and draws, not that it
    merely loads, so the report looks for the first screen the editor draws and
    prints the exit status either way. Interactive input is not exercised: the
    target's console-input-status service reports the PS/2 keyboard only, and
    both emulators deliver a test's keystrokes over the serial line.
    """
    if shutil.which("qemu-system-x86_64") is None:
        print("target run: skipped (QEMU unavailable)")
        return 0
    work = OUTPUT
    work.mkdir(parents=True, exist_ok=True)
    disk = work / "edit-run.img"
    shutil.copy2(DOS64 / "build" / "dos64-lean.img", disk)
    subprocess.run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                    str(disk), str(OUTPUT / image), pathlib.Path(image).stem],
                   check=True, stdout=subprocess.DEVNULL)
    transcript = work / "edit-run.log"
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
             "-serial", "stdio", "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        process.stdin.write(pathlib.Path(image).stem + "\n")
        process.stdin.close()
        try:
            process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
    text = transcript.read_text(encoding="utf-8", errors="replace")
    marker = [line for line in text.splitlines() if "Loaded, pid" in line]
    exit_line = re.findall(r"^Exit (\d+)\s*$", text, re.MULTILINE)
    # The editor draws a styled screen, so a menu letter and its key hint are
    # separated by colour codes. Strip the sequences before looking for text:
    # a substring that never appears in the raw bytes still names a drawn
    # screen, and a check that only ever matched raw bytes would report "not
    # seen" for a screen that was.
    plain = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)
    plain = re.sub(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)", "", plain)
    drew = "(F)" in plain and "Untitled" in plain
    print(f"target run: {marker[-1] if marker else 'did not report a load'}"
          f"{', exit ' + exit_line[-1] if exit_line else ''}")
    print(f"target run: first screen {'drawn' if drew else 'not seen'}")
    if not marker:
        print("  the target did not start the image")
        return 1
    if not drew:
        print("  the image loaded but no screen was found in its output")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
