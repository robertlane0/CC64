#!/usr/bin/env python3
"""Run compiler-produced images in the pinned DOS64 target."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
TARGET_REVISION = "13c3cedb05ad75592c17bf2006ba8617c8761a38"
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def run(command: list[str], cwd: pathlib.Path, timeout: int = 180) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def check_target() -> None:
    if not TARGET.is_dir():
        return
    override = os.environ.get("CC64_TARGET_REVISION")
    if override is not None and os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
        raise SystemExit("strict release mode does not allow a target revision override")
    expected = override or TARGET_REVISION
    result = subprocess.run(["git", "-C", str(TARGET), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit("target checkout has no readable revision")
    actual = result.stdout.strip()
    if actual != expected:
        raise SystemExit(f"target revision {actual} does not match pinned {expected}")
    dirty = subprocess.run(["git", "-C", str(TARGET), "diff", "--quiet", "HEAD", "--"],
                           check=False)
    if dirty.returncode != 0:
        raise SystemExit("target checkout has tracked source changes")


def check_volume(image: pathlib.Path, label: str) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed ({label}): {detail}")


def execute(qemu: str, disk: pathlib.Path, name: str, command: str,
            expected: int) -> str:
    transcript = disk.with_suffix(".log")
    timed_out = False
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            process.communicate(command + "\n", timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            timed_out = True
    text = transcript.read_text(encoding="utf-8", errors="replace")
    marker = f"Exit {expected}"
    if timed_out:
        if marker not in text or "A> " not in text[text.index(marker) + len(marker):]:
            raise SystemExit(f"{name}: QEMU timed out before a complete target result")
    elif process.returncode != 0:
        raise SystemExit(f"{name}: QEMU exited with status {process.returncode}")
    return text


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
        message = "target: skipped (QEMU or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    check_target()
    run(["make", "clean", "lean"], TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-target-") as temp:
        work = pathlib.Path(temp)
        cases = [
            ("C64R", "int main(void) { return 7; }", "raw", 7),
            ("C64S", "int f(int x){int y=0; switch(x){case 7: y=9; break; default: y=3;} return y;} int main(void){return f(7);}", "raw", 9),
            ("C64A", "int main(void){int a[2][2]={{1,2},{3,4}}; return a[1][1];}", "raw", 4),
            ("C64P", "int x=7; int *p=&x; int main(void){return *p;}", "mz64", 7),
            ("C64I", "int main(void){int x=4; int y=x++; return y*10+x;}", "raw", 45),
            ("C64X", "void cc64_exit(int); int main(void){cc64_exit(9); return 3;}", "raw", 9),
            ("C64O", "int cc64_open(const char *); int cc64_close(int); int main(void){int h=cc64_open(\"HELLO.TXT\"); if(h>=0) cc64_close(h); return h>=0?7:1;}", "raw", 7),
        ]
        for name, source, image_format, expected in cases:
            disk = work / f"{name}.img"
            image = compile_and_link(work, source, name, image_format)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk, f"{name} fresh")
            run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                 str(image), name], ROOT)
            check_volume(disk, f"{name} embedded")
            text = execute(qemu, disk, name, name, expected)
            check_volume(disk, f"{name} after QEMU")
            if f"Exit {expected}" not in text:
                raise SystemExit(f"{name}: target did not return {expected}")
        print(f"target: QEMU passed {len(cases)} raw/MZ64 image cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
