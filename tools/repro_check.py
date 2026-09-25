#!/usr/bin/env python3
"""Check path-independent clean-build and target-artifact reproducibility."""

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
IGNORED = {".git", "build", "cc64", "__pycache__", ".cache", "MS-DOS64"}


def build_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for name in ("MAKEFLAGS", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LDLIBS"):
        environment.pop(name, None)
    environment["LC_ALL"] = "C"
    environment["TZ"] = "UTC"
    environment["SOURCE_DATE_EPOCH"] = "0"
    return environment


def run_build(export: pathlib.Path) -> None:
    environment = build_environment()
    subprocess.run(["make", "clean"], cwd=export, env=environment, check=True,
                   stdout=subprocess.DEVNULL, timeout=120)
    subprocess.run(["make", "-j2", "all", "test-unit"], cwd=export,
                   env=environment, check=True, stdout=subprocess.DEVNULL,
                   timeout=120)


def target_artifact_digest(export: pathlib.Path) -> str:
    with tempfile.TemporaryDirectory(prefix="cc64-repro-target-") as temp:
        work = pathlib.Path(temp)
        raw_source = work / "raw.c"
        raw_source.write_text("int main(void) { return 7; }\n", encoding="utf-8")
        mz_source = work / "mz.c"
        mz_source.write_text(
            "int value = 7; int *pointer = &value; "
            "int main(void) { return *pointer; }\n", encoding="utf-8")
        raw_obj = work / "raw.cc64o"
        mz_obj = work / "mz.cc64o"
        raw = work / "fixture.com"
        mz = work / "fixture.mz64"
        compiler = export / "cc64"
        subprocess.run([str(compiler), "-c", str(raw_source), "-o", str(raw_obj)],
                       cwd=export, env=build_environment(), check=True,
                       stdout=subprocess.DEVNULL, timeout=30)
        subprocess.run([str(compiler), "-c", str(mz_source), "-o", str(mz_obj)],
                       cwd=export, env=build_environment(), check=True,
                       stdout=subprocess.DEVNULL, timeout=30)
        subprocess.run([str(compiler), "--link", str(raw_obj), "-o", str(raw)],
                       cwd=export, env=build_environment(), check=True,
                       stdout=subprocess.DEVNULL, timeout=30)
        subprocess.run([str(compiler), "--link", "--format", "mz64", str(mz_obj),
                        "-o", str(mz)], cwd=export, env=build_environment(),
                       check=True, stdout=subprocess.DEVNULL, timeout=30)
        digest = hashlib.sha256()
        for path in (raw_obj, raw, mz_obj, mz):
            digest.update(path.name.encode("ascii"))
            digest.update(b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
        return digest.hexdigest()


def snapshot(export: pathlib.Path) -> tuple[str, int]:
    run_build(export)
    files = [export / "cc64"]
    files.extend(path for path in (export / "build").rglob("*") if path.is_file())
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda item: item.relative_to(export).as_posix()):
        digest.update(path.relative_to(export).as_posix().encode("ascii"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    digest.update(b"target-artifacts\0")
    digest.update(target_artifact_digest(export).encode("ascii"))
    return digest.hexdigest(), len(files)


def copy_export(destination: pathlib.Path) -> None:
    shutil.copytree(ROOT, destination, symlinks=True,
                    ignore=shutil.ignore_patterns(*sorted(IGNORED)))


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cc64-repro-") as temp:
        root = pathlib.Path(temp)
        first = root / "first"
        second = root / "second"
        copy_export(first)
        copy_export(second)
        first_digest, first_count = snapshot(first)
        second_digest, second_count = snapshot(second)
    if first_digest != second_digest or first_count != second_count:
        raise SystemExit("clean path-independent builds are not byte-identical")
    print(f"repro: clean builds match {first_digest} ({first_count} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
