#!/usr/bin/env python3
"""Exercise the CC64-built target bootstrap stage under QEMU."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        print("self-host: skipped (QEMU or target checkout unavailable)")
        return 0
    with tempfile.TemporaryDirectory(prefix="cc64-self-host-") as temp:
        work = pathlib.Path(temp)
        obj = work / "stage1.cc64o"
        image = work / "stage1.mz64"
        disk = work / "stage1.img"
        log = work / "stage1.log"
        subprocess.run([str(ROOT / "cc64"), "-c", str(ROOT / "src/selfhost/stage1.c"),
                        "-o", str(obj)], cwd=ROOT, check=True)
        subprocess.run([str(ROOT / "cc64"), "--link", "--format", "mz64",
                        str(obj), "-o", str(image)], cwd=ROOT, check=True)
        subprocess.run(["make", "lean"], cwd=TARGET, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(image), "STAGE1"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
                 "-display", "none"], stdin=subprocess.PIPE, stdout=stream,
                stderr=subprocess.STDOUT, text=True,
            )
            try:
                process.communicate("STAGE1\n", timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
        text = log.read_text(encoding="utf-8", errors="replace")
        if "Exit 51" not in text:
            raise SystemExit("stage1 did not return its deterministic checksum")
    print("self-host: CC64-built stage1 returned exit 51")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
