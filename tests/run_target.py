#!/usr/bin/env python3
"""Run one compiler-produced COM image in the pinned DOS64 target."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT / "MS-DOS64"


def run(command: list[str], cwd: pathlib.Path, timeout: int = 120) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        print("target: skipped (QEMU or target checkout unavailable)")
        return 0
    with tempfile.TemporaryDirectory(prefix="cc64-target-") as temp:
        work = pathlib.Path(temp)
        source = work / "return7.c"
        obj = work / "return7.cc64o"
        image = work / "return7.com"
        disk = work / "dos64-lean.img"
        source.write_text(
            "int base = 3;\n"
            "int add(int a, int b) { return a + b; }\n"
            "int main(void) { return add(base, 4); }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)], ROOT)
        run([str(ROOT / "cc64"), "--link", str(obj), "-o", str(image)], ROOT)
        run(["make", "lean"], TARGET, timeout=180)
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk), str(image), "CC64"], ROOT)
        transcript = work / "qemu.log"
        with transcript.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio", "-display", "none"],
                stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                process.communicate("CC64\n", timeout=30)
                returncode = process.returncode
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                returncode = 124
        text = transcript.read_text(encoding="utf-8", errors="replace")
        if returncode not in (0, 124) or "Exit 7" not in text:
            raise SystemExit("compiler-produced image did not reach target exit 7")
    print("target: QEMU CC64.COM returned exit 7")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
