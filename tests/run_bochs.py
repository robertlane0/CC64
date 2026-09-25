#!/usr/bin/env python3
"""Run the target image under Bochs when the emulator is installed."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"


def main() -> int:
    bochs = shutil.which("bochs")
    if bochs is None or not TARGET.is_dir():
        print("bochs: skipped (Bochs or target checkout unavailable)")
        return 0
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
        subprocess.run(["make", "lean"], cwd=TARGET, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(image), "BOCH"], cwd=ROOT, check=True)
        config = work / "bochs.cfg"
        config.write_text(
            f"megs: 128\nromimage: file={disk}, format=raw\n"
            "display: disabled\nserial: stdio\nboot: c\n",
            encoding="utf-8",
        )
        result = subprocess.run([bochs, "-q", "-f", str(config)],
                                input="BOCH\n", text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=45,
                                check=False)
        if "Exit 7" not in result.stdout:
            raise SystemExit("Bochs did not reach target exit 7")
    print("bochs: target returned exit 7")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
