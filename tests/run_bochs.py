#!/usr/bin/env python3
"""Run the target image under Bochs when the emulator is installed."""

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


def main() -> int:
    bochs = shutil.which("bochs")
    if bochs is None or not TARGET.is_dir():
        message = "bochs: skipped (Bochs or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    check_target()
    with tempfile.TemporaryDirectory(prefix="cc64-bochs-") as temp:
        work = pathlib.Path(temp)
        source = work / "bochs.c"
        obj = work / "bochs.cc64o"
        image = work / "bochs.com"
        disk = work / "dos64.img"
        source.write_text("int main(void) { return 7; }\n", encoding="utf-8")
        subprocess.run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)],
                       cwd=ROOT, check=True)
        subprocess.run([str(ROOT / "cc64"), "--link", str(obj), "-o", str(image)],
                       cwd=ROOT, check=True)
        subprocess.run(["make", "clean", "lean"], cwd=TARGET, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        check_volume(disk)
        subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(image), "BOCH"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        check_volume(disk)
        config = work / "bochs.cfg"
        config.write_text(
            f"megs: 128\nromimage: file={disk}, format=raw\n"
            "display: disabled\nserial: stdio\nboot: c\n",
            encoding="utf-8",
        )
        try:
            result = subprocess.run([bochs, "-q", "-f", str(config)],
                                    input="BOCH\n", text=True,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=45,
                                    check=False)
        except subprocess.TimeoutExpired as error:
            raise SystemExit("Bochs target run timed out") from error
        if result.returncode != 0:
            raise SystemExit(f"Bochs exited with status {result.returncode}")
        check_volume(disk)
        if "Exit 7" not in result.stdout:
            raise SystemExit("Bochs did not reach target exit 7")
    print("bochs: target returned exit 7")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
