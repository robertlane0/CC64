#!/usr/bin/env python3
"""Ask the target-built compiler to include each staged header, one at a time.

A single self-hosted run that cannot open an include is hard to attribute, and
the diagnostic for it prints the directive text rather than the name, so this
stages one tiny source per header and runs each in its own boot. The result per
header is what says whether the flat include search resolves a qualified name.
"""

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
import target_revision  # noqa: E402  (path is set above)
import selfhost_stage as stage  # noqa: E402


def main() -> int:
    if shutil.which("qemu-system-x86_64") is None:
        print("include probe: skipped (QEMU unavailable)")
        return 0
    target_revision.check(TARGET)
    headers = stage.include_tree()
    with tempfile.TemporaryDirectory(prefix="cc64-include-probe-") as temp:
        work = pathlib.Path(temp)
        stage1, _ = stage.build_stage1(work)
        tree: list[tuple[str, pathlib.Path]] = [(stage.IMAGE_NAME, stage1)]
        for header in headers:
            probe = work / f"{header.stem}.probe"
            probe.write_text(f'#include "{header.name}"\nint main(void) {{ return 0; }}\n',
                             encoding="utf-8")
            tree.append((f"{header.stem.upper()[:8]}.C", probe))
            # The header itself has to be on the volume: the probe measures
            # whether the flat include search finds it, not whether the
            # compiler can report a missing one.
            tree.append((header.name.upper(), header))
        manifest = work / "tree.txt"
        stage.write_manifest(manifest, tree)
        disk = work / "probe.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        stage.embed(disk, manifest)
        for header in headers:
            name = f"{header.stem.upper()[:8]}"
            try:
                text = stage.boot(disk, f"CC64S -c {name}.C -o {name}.O",
                                 marker="Exit 0", timeout=150.0, expect=False)
                code = " ".join(text.split())
                code = code[code.index("Exit ") + 5:]
                code = code[:code.index(" ")] if " " in code else code
                if code == "0":
                    print(f"  ok    {header.name}", flush=True)
                else:
                    print(f"  exit{code} {header.name}", flush=True)
            except SystemExit as error:
                print(f"  TIMEOUT {header.name}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
