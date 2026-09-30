#!/usr/bin/env python3
"""Compile one C file with CC64 and run it in the pinned DOS64 target.

The conformance corpus is the gate, but it is long and a change to the calling
convention has to be looked at one program at a time. This is the short path:
one source file, one image, one run.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
RUNTIME = ROOT / "src" / "runtime"
INCLUDE_PATHS = [ROOT / "include" / "target", ROOT / "include" / "cc64", ROOT / "src"]


def run(command: list[str], cwd: pathlib.Path, timeout: int = 600) -> None:
    result = subprocess.run(command, cwd=cwd, check=False, timeout=timeout,
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"failed: {' '.join(command)}")


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("usage: target_one.py SOURCE.c [NAME]")
    source = pathlib.Path(sys.argv[1]).resolve()
    name = pathlib.Path(sys.argv[2]).name.split(".")[0].upper() if len(sys.argv) > 2 \
        else source.stem.upper()
    image_format = sys.argv[3] if len(sys.argv) > 3 else "mz64"
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        raise SystemExit("qemu-system-x86_64 is not installed")
    with tempfile.TemporaryDirectory(prefix="cc64-one-") as temp:
        work = pathlib.Path(temp)
        if not (TARGET / "build" / "dos64-lean.img").is_file():
            run(["make", "clean", "lean"], TARGET)
        object_path = work / f"{name}.cc64o"
        compile_command = [str(ROOT / "cc64")]
        for path in INCLUDE_PATHS:
            compile_command += ["-I", str(path)]
        run(compile_command + ["-c", str(source), "-o", str(object_path)], ROOT)
        image = work / f"{name}.{image_format}"
        # The library is only needed when the program names something outside
        # itself; without it a raw image can be linked, and a program that does
        # use it is the case worth having the library for.
        try:
            run([str(ROOT / "cc64"), "--link", "--format", image_format,
                 str(object_path), "-o", str(image)], ROOT)
        except SystemExit:
            library = []
            for index, unit in enumerate(sorted(RUNTIME.glob("target_*.c"))):
                built = work / f"lib-{index}.cc64o"
                run(compile_command + ["-c", str(unit), "-o", str(built)], ROOT)
                library.append(str(built))
            run([str(ROOT / "cc64"), "--link", "--format", image_format,
                 str(object_path), *library, "-o", str(image)], ROOT)
        disk = work / f"{name}.img"
        shutil.copy2(TARGET / "build" / "dos64-lean.img", disk)
        run(["python3", str(ROOT / "tests" / "embed_fat12.py"), str(disk),
             str(image), name], ROOT)
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace")
        process.stdin.write(name + "\n")
        process.stdin.close()
        try:
            text = process.communicate(timeout=60)[0]
        except subprocess.TimeoutExpired:
            process.kill()
            text = process.communicate()[0]
        print(text[-4000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
