#!/usr/bin/env python3
"""Run compiler-produced images in the pinned DOS64 target."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"


def run(command: list[str], cwd: pathlib.Path, timeout: int = 180) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def execute(qemu: str, disk: pathlib.Path, name: str, command: str) -> str:
    transcript = disk.with_suffix(".log")
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            process.communicate(command + "\n", timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
    return transcript.read_text(encoding="utf-8", errors="replace")


def compile_and_link(work: pathlib.Path, source_text: str, name: str,
                     image_format: str = "raw") -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    image = work / (f"{name}.mz" if image_format == "mz64" else f"{name}.com")
    source.write_text(source_text, encoding="utf-8")
    run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)], ROOT)
    command = [str(ROOT / "cc64"), "--link"]
    if image_format == "mz64":
        command += ["--format", "mz64"]
    command += [str(obj), "-o", str(image)]
    run(command, ROOT)
    return image


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        print("target: skipped (QEMU or target checkout unavailable)")
        return 0
    run(["make", "lean"], TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-target-") as temp:
        work = pathlib.Path(temp)
        cases = [
            ("C64R", "int main(void) { return 7; }", "raw", 7),
            ("C64S", "int f(int x){int y=0; switch(x){case 7: y=9; break; default: y=3;} return y;} int main(void){return f(7);}", "raw", 9),
            ("C64A", "int main(void){int a[2][2]={{1,2},{3,4}}; return a[1][1];}", "raw", 4),
            ("C64P", "int x=7; int *p=&x; int main(void){return *p;}", "mz64", 7),
            ("C64I", "int main(void){int x=4; int y=x++; return y*10+x;}", "raw", 45),
        ]
        for name, source, image_format, expected in cases:
            disk = work / f"{name}.img"
            image = compile_and_link(work, source, name, image_format)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                 str(image), name], ROOT)
            text = execute(qemu, disk, name, name)
            if f"Exit {expected}" not in text:
                raise SystemExit(f"{name}: target did not return {expected}")
        print(f"target: QEMU passed {len(cases)} raw/MZ64 image cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
