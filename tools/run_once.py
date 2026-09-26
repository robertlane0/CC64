#!/usr/bin/env python3
"""Run one CC64-produced image on the target and print its transcript.

A small helper for narrowing a target failure by hand: it builds a single
source, embeds it on a fresh volume, runs it once, and prints what the program
printed and the exit code the target reported.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"


def run_source(name: str, text: str, image_format: str = "raw",
               timeout: float = 30.0) -> tuple[str, str]:
    work = pathlib.Path(tempfile.mkdtemp(prefix="cc64-once-"))
    source = work / f"{name}.c"
    source.write_text(text, encoding="utf-8")
    obj = work / f"{name}.o"
    image = work / f"{name}.{'mz' if image_format == 'mz64' else 'com'}"
    subprocess.run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)],
                   check=True, cwd=ROOT)
    subprocess.run([str(ROOT / "cc64"), "--link", "--format", image_format,
                    str(obj), "-o", str(image)], check=True, cwd=ROOT)
    disk = work / "volume.img"
    shutil.copy2(TARGET / "build/dos64-lean.img", disk)
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    str(image), name], check=True, cwd=ROOT,
                   stdout=subprocess.DEVNULL)
    log = work / "transcript.log"
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
             "-serial", "stdio", "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        process.stdin.write(name + "\n")
        process.stdin.close()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            transcript = log.read_text(encoding="utf-8", errors="replace")
            if "Exit" in transcript and "A> " in transcript[transcript.index("Exit") + 4:]:
                break
            if process.poll() is not None:
                break
            time.sleep(0.05)
        if process.poll() is None:
            process.kill()
        process.wait()
    transcript = log.read_text(encoding="utf-8", errors="replace")
    start = transcript.find("Loaded")
    body = transcript[start:] if start >= 0 else transcript
    exit_code = ""
    for line in body.splitlines():
        if line.startswith("Exit "):
            exit_code = line
    return body.strip(), exit_code


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: run_once.py NAME SOURCE.c [raw|mz64]", file=sys.stderr)
        return 2
    text = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    body, exit_code = run_source(sys.argv[1], text,
                                 sys.argv[3] if len(sys.argv) > 3 else "raw")
    print(body)
    print("->", exit_code or "(no exit code)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
