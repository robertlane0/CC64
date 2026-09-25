#!/usr/bin/env python3
"""Exercise the CC64-built target bootstrap stage under QEMU."""

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


def check_target() -> None:
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


def check_volume(image: pathlib.Path) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed: {detail}")


def execute(qemu: str, disk: pathlib.Path, name: str) -> str:
    transcript = disk.with_suffix(".log")
    timed_out = False
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"], stdin=subprocess.PIPE, stdout=stream,
            stderr=subprocess.STDOUT, text=True,
        )
        try:
            process.communicate(name + "\n", timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            timed_out = True
    text = transcript.read_text(encoding="utf-8", errors="replace")
    marker = "Exit 51"
    if timed_out:
        if marker not in text or "A> " not in text[text.index(marker) + len(marker):]:
            raise SystemExit("stage1 QEMU timed out before a complete target result")
    elif process.returncode != 0:
        raise SystemExit(f"stage1 QEMU exited with status {process.returncode}")
    return text


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        message = "self-host: skipped (QEMU or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    check_target()
    with tempfile.TemporaryDirectory(prefix="cc64-self-host-") as temp:
        work = pathlib.Path(temp)
        subprocess.run(["make", "clean", "lean"], cwd=TARGET, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        image_bytes = []
        for index in range(2):
            obj = work / f"stage1-{index}.cc64o"
            image = work / f"stage1-{index}.mz64"
            disk = work / f"stage1-{index}.img"
            log = work / f"stage1-{index}.log"
            subprocess.run([str(ROOT / "cc64"), "-c",
                            str(ROOT / "src/selfhost/stage1.c"), "-o", str(obj)],
                           cwd=ROOT, check=True)
            subprocess.run([str(ROOT / "cc64"), "--link", "--format", "mz64",
                            str(obj), "-o", str(image)], cwd=ROOT, check=True)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk)
            subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                            str(disk), str(image), "STAGE1"], cwd=ROOT,
                           check=True, stdout=subprocess.DEVNULL)
            check_volume(disk)
            text = execute(qemu, disk, "STAGE1")
            check_volume(disk)
            if "Exit 51" not in text:
                detail = log.read_text(encoding="utf-8", errors="replace")
                raise SystemExit(f"stage1 did not return its deterministic checksum: {detail}")
            image_bytes.append(image.read_bytes())
        if image_bytes[0] != image_bytes[1]:
            raise SystemExit("stage1 MZ64 image is not deterministic across builds")
    print("self-host: CC64-built stage1 returned exit 51 twice")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
