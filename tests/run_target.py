#!/usr/bin/env python3
"""Run compiler-produced images in the pinned DOS64 target."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import corpus  # noqa: E402  (path is set above)
import target_revision  # noqa: E402  (path is set above)

TARGET_REVISION = target_revision.TARGET_REVISION
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def run(command: list[str], cwd: pathlib.Path, timeout: int = 180) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def check_target() -> None:
    target_revision.check(TARGET)


def check_volume(image: pathlib.Path, label: str) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed ({label}): {detail}")


def stop_process(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def execute(qemu: str, disk: pathlib.Path, name: str, command: str,
            expected: int, timeout: float = 15.0) -> str:
    transcript = disk.with_suffix(".log")
    process = None
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True,
        )
        process.stdin.write(command + "\n")
        process.stdin.close()
        marker = f"Exit {expected}"
        deadline = time.monotonic() + timeout
        complete = False
        while time.monotonic() < deadline:
            text = transcript.read_text(encoding="utf-8", errors="replace")
            if marker in text and "A> " in text[text.index(marker) + len(marker):]:
                complete = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.05)
        stop_process(process)
    text = transcript.read_text(encoding="utf-8", errors="replace")
    if not complete:
        if process.returncode not in (0, -15):
            raise SystemExit(f"{name}: QEMU exited with status {process.returncode}")
        raise SystemExit(f"{name}: QEMU timed out before a complete target result")
    return text


def target_library_objects(work: pathlib.Path) -> list[str]:
    """Compile the target C library with CC64 for one linked case."""
    sources = sorted((ROOT / "src" / "runtime").glob("target_*.c"))
    objects = []
    for index, source in enumerate(sources):
        output = work / f"target-lib-{index}.cc64o"
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-I", str(ROOT / "src"),
             "-c", str(source), "-o", str(output)], ROOT)
        objects.append(str(output))
    return objects


def compile_and_link(work: pathlib.Path, source_text: str, name: str,
                     image_format: str = "raw") -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    image = work / (f"{name}.mz" if image_format == "mz64" else f"{name}.com")
    source.write_text(source_text, encoding="utf-8")
    run([str(ROOT / "cc64"), "-I", str(ROOT / "include/target"),
         "-I", str(ROOT / "include/cc64"), "-c", str(source), "-o", str(obj)], ROOT)
    command = [str(ROOT / "cc64"), "--link"]
    if image_format == "mz64":
        command += ["--format", "mz64"]
    command += [str(obj), "-o", str(image)]
    run(command, ROOT)
    return image


def main() -> int:
    if not TARGET.is_dir():
        print("target: skipped (target checkout unavailable)")
        return 0
    if not target_revision.require_emulator(
            "qemu-system-x86_64",
            "target: skipped (QEMU or target checkout unavailable)"):
        return 0
    qemu = shutil.which("qemu-system-x86_64")
    check_target()
    run(["make", "clean", "lean"], TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-target-") as temp:
        work = pathlib.Path(temp)
        cases = corpus.CASES
        library_objects = target_library_objects(work)
        for entry in cases:
            name, source, image_format, expected, expected_text = entry[:5]
            command = entry[5] if len(entry) > 5 else name
            disk = work / f"{name}.img"
            image = compile_and_link(work, source, name, image_format)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk, f"{name} fresh")
            run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                 str(image), name], ROOT)
            check_volume(disk, f"{name} embedded")
            text = execute(qemu, disk, name, command, expected)
            check_volume(disk, f"{name} after QEMU")
            if f"Exit {expected}" not in text:
                raise SystemExit(f"{name}: target did not return {expected}")
            if expected_text is not None and expected_text not in text:
                raise SystemExit(f"{name}: target output lacked {expected_text!r}")
        library_source = work / "C64L.c"
        library_object = work / "C64L.cc64o"
        library_source.write_text(corpus.LIBRARY_PROGRAM, encoding="utf-8")
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(library_source),
             "-o", str(library_object)], ROOT)
        library_image = work / "C64L.mz"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64",
             str(library_object), *library_objects, "-o", str(library_image)], ROOT)
        library_disk = work / f"{corpus.LIBRARY_CASE}.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", library_disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(library_disk),
             str(library_image), corpus.LIBRARY_CASE], ROOT)
        # The library case moves a file larger than the target's sixteen-bit
        # service count, which is slow in the emulator.
        text = execute(qemu, library_disk, corpus.LIBRARY_CASE,
                       corpus.LIBRARY_CASE, corpus.LIBRARY_EXIT,
                       timeout=corpus.LIBRARY_TIMEOUT)
        if f"Exit {corpus.LIBRARY_EXIT}" not in text:
            raise SystemExit(
                f"{corpus.LIBRARY_CASE}: target library case did not return "
                f"{corpus.LIBRARY_EXIT}")
        if corpus.LIBRARY_OUTPUT not in text:
            raise SystemExit(f"{corpus.LIBRARY_CASE}: target library output "
                             f"unexpected: {text[-200:]}")

        vform_source = work / "C64V2.c"
        vform_object = work / "C64V2.cc64o"
        vform_source.write_text(corpus.LIBRARY_VFORM_PROGRAM, encoding="utf-8")
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(vform_source),
             "-o", str(vform_object)], ROOT)
        vform_image = work / "C64V2.mz"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64",
             str(vform_object), *library_objects, "-o", str(vform_image)], ROOT)
        vform_disk = work / "C64V2.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", vform_disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(vform_disk),
             str(vform_image), "C64V2"], ROOT)
        text = execute(qemu, vform_disk, "C64V2", "C64V2", 9, timeout=60.0)
        if "Exit 9" not in text:
            raise SystemExit("C64V2: target library v-form case did not return 9")
        if "v-42" not in text:
            raise SystemExit(f"C64V2: target library v-form output unexpected: "
                             f"{text[-200:]}")
        # Every name the target headers declare has to answer at link time, or
        # a program written for a hosted system compiles and then fails on a
        # symbol rather than on the header that promised it. The program is
        # generated from the headers so a new declaration without a definition
        # fails here without anything having to remember to extend a list.
        declared = work / "C64D.c"
        declared_object = work / "C64D.cc64o"
        subprocess.run(
            [sys.executable, str(ROOT / "tests/declared_symbols.py"),
             "-o", str(declared)], check=True, cwd=ROOT,
            stdout=subprocess.DEVNULL)
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(declared),
             "-o", str(declared_object)], ROOT)
        declared_image = work / "C64D.mz"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64",
             str(declared_object), *library_objects, "-o", str(declared_image)], ROOT)
        declared_disk = work / "C64D.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", declared_disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(declared_disk),
             str(declared_image), "C64D"], ROOT)
        text = execute(qemu, declared_disk, "C64D", "C64D", 0, timeout=60.0)
        if "Exit 0" not in text:
            raise SystemExit("C64D: a target header declares a name the "
                             f"library does not define: {text[-200:]}")
        # The target's own arithmetic, checked on the target: the routines are
        # written from the numeric definitions rather than from a library, and
        # the only way to know a target's floating-point code is right is to run
        # it on the target.
        math_source = work / "C64S.c"
        math_object = work / "C64S.cc64o"
        shutil.copy2(ROOT / "tests" / "target_math.c", math_source)
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(math_source),
             "-o", str(math_object)], ROOT)
        math_image = work / "C64S.mz"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64",
             str(math_object), *library_objects, "-o", str(math_image)], ROOT)
        math_disk = work / "C64S.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", math_disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(math_disk),
             str(math_image), "C64S"], ROOT)
        text = execute(qemu, math_disk, "C64S", "C64S", 0, timeout=180.0)
        if "Exit 0" not in text:
            raise SystemExit("C64S: the target's arithmetic suite did not "
                             f"return 0: {text[-200:]}")
        print(f"target: QEMU passed {len(cases)} raw/MZ64 image cases, "
              "four linked target-library cases, and every declared name")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
