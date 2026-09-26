#!/usr/bin/env python3
"""Run the target-built compiler on the target and compare its output.

The host bootstrap compiler builds a complete CC64 image; that image is then run
on the pinned target, where it compiles a project-authored source with its own
front end and links the result with its own linker. The object and the image it
produces are read back off the volume and compared byte for byte with what the
host-built compiler produced from the same source, and the linked program is
then executed. A self-hosted compiler that agreed with the bootstrap compiler
here has reached a fixed point on every construct the probe uses.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
import target_revision  # noqa: E402  (path is set above)

PROBE = ROOT / "src/selfhost/fixed_point.c"
PROBE_NAME = "FIXED.C"
OBJECT_NAME = "FIXED.O"
IMAGE_NAME = "OUT.COM"
COMPILER_NAME = "CC64S.COM"
PROBE_EXIT = 66
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def build_stage(work: pathlib.Path) -> tuple[pathlib.Path, list[pathlib.Path]]:
    """Compile every production unit and the target library, then link stage1."""
    include = ["-I", str(ROOT / "include/target"), "-I", str(ROOT / "include/cc64"),
               "-I", str(ROOT / "src")]
    sources = [path for path in sorted((ROOT / "src").rglob("*.c"))
               if "runtime" not in path.parts and "selfhost" not in path.parts]
    sources += sorted((ROOT / "src/runtime").glob("target_*.c"))
    if not sources:
        raise SystemExit("self-host run found no sources to build")
    objects = []
    for source in sources:
        output = work / f"{source.stem}.cc64o"
        result = subprocess.run([str(ROOT / "cc64"), *include, "-c", str(source),
                                 "-o", str(output)], cwd=ROOT, capture_output=True,
                                text=True)
        if result.returncode != 0:
            raise SystemExit(f"self-host run cannot compile {source.name}:\n"
                             f"{result.stdout}{result.stderr}")
        objects.append(output)
    image = work / "stage1.mz64"
    result = subprocess.run([str(ROOT / "cc64"), "--link", "--format", "mz64",
                             *[str(o) for o in objects], "-o", str(image)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"self-host run cannot link stage1:\n"
                         f"{result.stdout}{result.stderr}")
    return image, objects


def check_volume(image: pathlib.Path) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed: {detail}")


def boot(qemu: str, disk: pathlib.Path, command: str, marker: str,
         timeout: float) -> str:
    """Run one command on the target and wait for its result in the transcript.

    The target shell stays resident after a program returns, so the run is
    complete when the transcript shows the result followed by another prompt;
    the machine is then stopped rather than waited on.
    """
    transcript = disk.with_suffix(".log")
    process = None
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        process.stdin.write(command + "\n")
        process.stdin.close()
        deadline = time.monotonic() + timeout
        complete = False
        while time.monotonic() < deadline:
            text = transcript.read_text(encoding="utf-8", errors="replace")
            if marker in text and "A> " in text[text.index(marker) + len(marker):]:
                complete = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.1)
        if process.poll() is None:
            process.kill()
        process.wait()
    text = transcript.read_text(encoding="utf-8", errors="replace")
    if not complete:
        raise SystemExit(f"target run of {command!r} did not reach {marker}:\n"
                         f"{text[-400:]}")
    return text


def embed(disk: pathlib.Path, payload: pathlib.Path, name: str) -> None:
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    str(payload), name], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)


def extract(disk: pathlib.Path, name: str, output: pathlib.Path) -> bytes:
    result = subprocess.run(["python3", str(ROOT / "tests/extract_fat12.py"),
                             str(disk), name, "-o", str(output)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"cannot read {name} back off the volume:\n"
                         f"{result.stdout}{result.stderr}")
    return output.read_bytes()


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        message = "self-host run: skipped (QEMU or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    target_revision.check(TARGET)
    if not PROBE.is_file():
        raise SystemExit(f"self-host run is missing its probe source {PROBE}")
    with tempfile.TemporaryDirectory(prefix="cc64-self-host-run-") as temp:
        work = pathlib.Path(temp)
        stage1, _ = build_stage(work)
        # What the host-built compiler produces from the same source.
        expected_object = work / "expected.cc64o"
        expected_image = work / "expected.com"
        subprocess.run([str(ROOT / "cc64"), "-c", str(PROBE), "-o",
                        str(expected_object)], cwd=ROOT, check=True)
        subprocess.run([str(ROOT / "cc64"), "--link", str(expected_object), "-o",
                        str(expected_image)], cwd=ROOT, check=True)
        disk = work / "selfhost.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        check_volume(disk)
        embed(disk, stage1, COMPILER_NAME)
        embed(disk, PROBE, PROBE_NAME)
        check_volume(disk)
        # The self-hosted compiler compiles the probe with its own front end.
        boot(qemu, disk, f"CC64S -c {PROBE_NAME} -o {OBJECT_NAME}", "Exit 0", 240.0)
        check_volume(disk)
        produced_object = work / "produced.cc64o"
        if extract(disk, OBJECT_NAME, produced_object) != expected_object.read_bytes():
            raise SystemExit("self-hosted object differs from the bootstrap object")
        # Its own linker links that object into a runnable image.
        boot(qemu, disk, f"CC64S --link {OBJECT_NAME} -o {IMAGE_NAME}", "Exit 0", 240.0)
        check_volume(disk)
        produced_image = work / "produced.com"
        if extract(disk, IMAGE_NAME, produced_image) != expected_image.read_bytes():
            raise SystemExit("self-hosted image differs from the bootstrap image")
        # A second run must reproduce both files exactly.
        boot(qemu, disk, f"CC64S -c {PROBE_NAME} -o {OBJECT_NAME}", "Exit 0", 240.0)
        repeat = work / "repeat.cc64o"
        if extract(disk, OBJECT_NAME, repeat) != expected_object.read_bytes():
            raise SystemExit("self-hosted object is not reproducible across runs")
        check_volume(disk)
        # The image the self-hosted linker produced must run on the target.
        boot(qemu, disk, "OUT", f"Exit {PROBE_EXIT}", 60.0)
        check_volume(disk)
    print(f"self-host run: target compiler produced the same object, image, and "
          f"exit {PROBE_EXIT} as the bootstrap compiler")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
