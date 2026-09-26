#!/usr/bin/env python3
"""Run one CC64-produced image on the target under Bochs and print the result.

The counterpart of tools/run_once.py for the second emulator: when a target
result disagrees with the generated code, running the same image under the other
emulator separates a code-generation problem from an emulation problem.
"""

from __future__ import annotations

import os
import pathlib
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: run_bochs_once.py NAME SOURCE.c", file=sys.stderr)
        return 2
    name, source_path = sys.argv[1], pathlib.Path(sys.argv[2])
    bochs = shutil.which("bochs")
    if bochs is None:
        print("bochs is unavailable")
        return 1
    work = pathlib.Path(tempfile.mkdtemp(prefix="cc64-bochs-once-"))
    source = work / f"{name}.c"
    source.write_text(source_path.read_text(encoding="utf-8"), encoding="utf-8")
    obj = work / f"{name}.o"
    image = work / f"{name}.com"
    subprocess.run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)],
                   check=True, cwd=ROOT)
    subprocess.run([str(ROOT / "cc64"), "--link", str(obj), "-o", str(image)],
                   check=True, cwd=ROOT)
    disk = work / "volume.img"
    shutil.copy2(TARGET / "build/dos64-lean.img", disk)
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    str(image), name], check=True, cwd=ROOT,
                   stdout=subprocess.DEVNULL)
    template = (TARGET / "bochsrc.txt.in").read_text(encoding="utf-8")
    master, slave = pty.openpty()
    serial_path = os.ttyname(slave)
    os.close(slave)
    display_master, display_slave = pty.openpty()
    config = work / "bochsrc.txt"
    config.write_text(template.replace("@IMAGE@", str(disk))
                      .replace("log: bochs.log", f"log: {work / 'bochs.log'}")
                      .replace("display_library: nogui", "display_library: term")
                      .replace("com1: enabled=1, mode=file, dev=serial.log",
                               f"com1: enabled=1, mode=term, dev={serial_path}"),
                      encoding="utf-8")
    console = (work / "console.log").open("wb")
    process = subprocess.Popen([bochs, "-q", "-f", str(config)],
                               stdin=subprocess.DEVNULL, stdout=console,
                               stderr=subprocess.STDOUT)
    output = bytearray()
    sent = False
    deadline = time.monotonic() + 60.0
    try:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], 0.1)
            if readable:
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    pass
            if not sent and b"A> " in output:
                os.write(master, name.encode("ascii") + b"\n")
                sent = True
            if b"Exit " in output:
                marker = output.index(b"Exit ") + 5
                if b"A> " in output[marker:]:
                    break
            if process.poll() is not None:
                break
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        console.close()
        os.close(master)
        os.close(display_master)
        os.close(display_slave)
    text = bytes(output).decode("utf-8", errors="replace")
    start = text.find("Loaded")
    print(text[start:].strip() if start >= 0 else text.strip()[-400:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
