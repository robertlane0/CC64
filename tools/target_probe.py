#!/usr/bin/env python3
"""Compile one C file with CC64, put it on the target's volume, and run it.

This is the general form of what each `tests/target_*.py` does for its own
program: compile against the target headers, link with the target library in the
load-biased form, embed, boot, and print what the program said. It exists so
that narrowing a target failure down to a construct is one file and one command
rather than a new script each time.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = pathlib.Path("/root/code/MS-DOS64")
CC64 = ROOT / "cc64"


def run(command: list[str]) -> None:
    subprocess.run(command, cwd=ROOT, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.STDOUT, timeout=600)


def build(source: pathlib.Path, work: pathlib.Path, name: str) -> pathlib.Path:
    obj = work / "probe.cc64o"
    run([str(CC64), "-I", str(ROOT / "include/target"), "-I",
         str(ROOT / "include/cc64"), "-c", str(source), "-o", str(obj)])
    # The library holds a table of pointers, so an image that includes it is
    # linked in the load-biased form.
    library = []
    for index, unit in enumerate(sorted((ROOT / "src/runtime").glob("target_*.c"))):
        output = work / f"L{index}.cc64o"
        run([str(CC64), "-I", str(ROOT / "include/target"), "-I",
             str(ROOT / "include/cc64"), "-I", str(ROOT / "src"),
             "-c", str(unit), "-o", str(output)])
        library.append(str(output))
    image = work / name
    run([str(CC64), "--link", "--format", "mz64", str(obj), *library,
         "-o", str(image)])
    return image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", help="C file to compile")
    parser.add_argument("--name", default="PROBE", help="image name on the volume")
    parser.add_argument("--for", dest="seconds", type=float, default=90.0,
                        help="seconds to let the program run")
    parser.add_argument("--keep", default=None, help="keep the build here")
    arguments = parser.parse_args()

    source = pathlib.Path(arguments.source).resolve()
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        print("probe: skipped (QEMU unavailable)")
        return 0

    def go(work: pathlib.Path) -> int:
        image = build(source, work, f"{arguments.name}.COM")
        disk = work / "probe.img"
        shutil.copy2(TARGET / "build" / "dos64-lean.img", disk)
        subprocess.run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(image), arguments.name],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        transcript = work / "probe.log"
        with transcript.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
                 "-display", "none"],
                stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                text=True)
            # The shell prompts as it boots, so the command waits for it.
            time.sleep(6.0)
            process.stdin.write(f"{arguments.name}\n")
            process.stdin.flush()
            deadline = time.monotonic() + arguments.seconds
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                time.sleep(0.3)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
        text = transcript.read_text(encoding="utf-8", errors="replace")
        if "Loaded, pid" not in text:
            print("probe: the image did not start")
            print(text[-2000:])
            return 1
        print(text)
        return 0

    if arguments.keep:
        work = pathlib.Path(arguments.keep)
        work.mkdir(parents=True, exist_ok=True)
        return go(work)
    with tempfile.TemporaryDirectory(prefix="cc64-probe-") as temp:
        return go(pathlib.Path(temp))


if __name__ == "__main__":
    raise SystemExit(main())
