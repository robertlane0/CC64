#!/usr/bin/env python3
"""Compile CC64 with CC64 on the target and compare every translation unit.

The bootstrap compiler builds a complete target image of itself. That image is
then staged on a volume together with the project's own sources and headers, and
it compiles those sources on the target with its own front end, lowering and
encoder. Every object it produces is read back off the volume and compared byte
for byte with the object the bootstrap compiler produced from the same source.
Finally the self-hosted linker links those objects, and the image it produces is
compared with the bootstrap-built image, so the compiler built by the compiler
is byte-identical to the compiler built by the bootstrap compiler.

The target volume holds no directories, so the tree is staged flat and the
preprocessor's documented base-name fallback resolves the qualified includes.
The volume is a fixed size, so the translation units are compiled in batches
whose objects fit alongside the image, the sources and the headers.
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
import runtime_sources  # noqa: E402  (path is set above)
import target_revision  # noqa: E402  (path is set above)

IMAGE_NAME = "CC64S.COM"
TIMEOUT = 900.0
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


production_sources = runtime_sources.production_sources
library_sources = runtime_sources.compiler_library_sources


def include_tree() -> list[pathlib.Path]:
    """Every header the tree needs, staged under its own base name."""
    headers = [path for path in (ROOT / "include/target").glob("*.h")]
    headers += [ROOT / "include/cc64/dos64.h"]
    headers += [path for path in (ROOT / "src").rglob("*.h")]
    names = [path.name.upper() for path in headers]
    duplicates = {name for name in names if names.count(name) > 1}
    if duplicates:
        raise SystemExit(f"self-host stage cannot flatten {sorted(duplicates)}")
    return headers


def compile_unit(source: pathlib.Path, output: pathlib.Path,
                 work: pathlib.Path, tag: str) -> None:
    command = [str(ROOT / "cc64"), "-I", str(ROOT / "include/target"),
               "-I", str(ROOT / "include/cc64"), "-I", str(ROOT / "src"),
               "-c", str(source), "-o", str(output)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"self-host stage cannot compile {source} ({tag}):\n"
                         f"{result.stdout}{result.stderr}")


def build_stage1(work: pathlib.Path) -> tuple[pathlib.Path, dict[str, pathlib.Path],
                                             list[pathlib.Path]]:
    """Build the target image of the compiler and the reference objects.

    The runtime library objects are returned as well: the self-hosted linker
    resolves the runtime's own symbols from them, exactly as the bootstrap
    link does.
    """
    objects: dict[str, pathlib.Path] = {}
    stage_objects: list[pathlib.Path] = []
    library_objects: list[pathlib.Path] = []
    for source in production_sources():
        output = work / f"reference-{source.stem}.cc64o"
        compile_unit(source, output, work, "reference")
        objects[source.stem] = output
        stage_objects.append(output)
    for source in library_sources():
        output = work / f"library-{source.stem}.cc64o"
        compile_unit(source, output, work, "library")
        stage_objects.append(output)
        library_objects.append(output)
    image = work / "stage1.mz64"
    command = [str(ROOT / "cc64"), "--link", "--format", "mz64"]
    command += [str(path) for path in stage_objects]
    command += ["-o", str(image)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"self-host stage cannot link stage1:\n"
                         f"{result.stdout}{result.stderr}")
    return image, objects, library_objects


def check_volume(image: pathlib.Path) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed: {detail}")


def write_manifest(path: pathlib.Path, entries: list[tuple[str, pathlib.Path]]) -> None:
    lines = [f"{name} {source}" for name, source in entries]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def embed(disk: pathlib.Path, manifest: pathlib.Path) -> None:
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    "--manifest", str(manifest)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)


def boot(disk: pathlib.Path, command: str, marker: str = "Exit 0",
        timeout: float = TIMEOUT, expect: bool = True) -> str:
    """Run one command on the target and wait for its result in the transcript."""
    transcript = disk.with_suffix(".log")
    process = None
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
             "-serial", "stdio", "-display", "none"],
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
            if not expect and "Exit " in text and "A> " in text[text.index("Exit ") + 5:]:
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
                         f"{text[-600:]}")
    return text


def extract(disk: pathlib.Path, name: str, output: pathlib.Path) -> bytes:
    result = subprocess.run(["python3", str(ROOT / "tests/extract_fat12.py"),
                             str(disk), name, "-o", str(output)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"cannot read {name} back off the volume:\n"
                         f"{result.stdout}{result.stderr}")
    return output.read_bytes()


def batches(sources: list[pathlib.Path], sizes: dict[str, int],
            limit_bytes: int) -> list[list[pathlib.Path]]:
    """Group translation units so one volume holds their objects at once."""
    groups: list[list[pathlib.Path]] = []
    current: list[pathlib.Path] = []
    current_bytes = 0
    for source in sources:
        size = sizes[source.stem]
        if current and current_bytes + size > limit_bytes:
            groups.append(current)
            current = []
            current_bytes = 0
        current.append(source)
        current_bytes += size
    if current:
        groups.append(current)
    return groups


def staged_name(index: int) -> str:
    """A one-letter staged base name, so a link of every object fits on one line.

    A command tail is bounded to 143 bytes on the target. A link names every
    object, the image format, and the output, which leaves about four
    characters per object: twenty objects plus the options need one letter each
    and a one-letter extension. The name comes from the position in the list it
    was drawn from, so a given object is always staged and read back under the
    same name.
    """
    if index >= 26:
        raise SystemExit(f"self-host stage has no staged name for item {index}")
    return chr(ord('A') + index)


def volume_names(sources: list[pathlib.Path]) -> dict[str, str]:
    """Volume names for the staged sources.

    The volume holds bare 8.3 names and the link volume is even tighter, so a
    source is staged under one letter. The mapping is derived only from the
    source list, so a given source is always staged and read back under the
    same name.
    """
    names: {str, str} = {}
    for index, source in enumerate(sources):
        names[source.stem] = staged_name(index)
    return names


def main() -> int:
    if not TARGET.is_dir():
        message = "self-host stage: skipped (QEMU or target checkout unavailable)"
        if not target_revision.require_emulator(
                f"{tool}", f"{message}"):
            return 0
    target_revision.check(TARGET)
    sources = production_sources()
    selected = sys.argv[1] if len(sys.argv) > 1 else None
    if selected:
        wanted = {name.strip().removesuffix(".c") for name in selected.split(",")}
        unknown = wanted - {path.stem for path in sources}
        if unknown:
            raise SystemExit(f"self-host stage does not know {sorted(unknown)}")
        sources = [path for path in sources if path.stem in wanted]
    if not sources:
        raise SystemExit("self-host stage found no production sources")
    headers = include_tree()
    names = volume_names(sources)
    with tempfile.TemporaryDirectory(prefix="cc64-self-host-stage-") as temp:
        work = pathlib.Path(temp)
        stage1, reference, library_objects = build_stage1(work)
        tree: list[tuple[str, pathlib.Path]] = [(IMAGE_NAME, stage1)]
        for source in sources:
            tree.append((f"{names[source.stem]}.C", source))
        for header in headers:
            tree.append((header.name.upper(), header))
        # The volume is 1440 KiB and must hold the image, the tree, and one
        # batch of objects at once, so the batch budget is what is left over
        # with room to spare.
        sizes = {stem: path.stat().st_size for stem, path in reference.items()}
        limit = 300 * 1024
        groups = batches(sources, sizes, limit)
        manifest = work / "tree.txt"
        write_manifest(manifest, tree)
        produced: dict[str, pathlib.Path] = {}
        for index, group in enumerate(groups, start=1):
            disk = work / f"batch{index}.img"
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk)
            embed(disk, manifest)
            check_volume(disk)
            for source in group:
                name = names[source.stem]
                boot(disk, f"CC64S -c {name}.C -o {name}.O")
            check_volume(disk)
            for source in group:
                name = names[source.stem]
                output = work / f"produced-{source.stem}.cc64o"
                payload = extract(disk, f"{name}.O", output)
                if payload != reference[source.stem].read_bytes():
                    raise SystemExit(
                        f"self-hosted object for {source.name} differs from the "
                        f"bootstrap object")
                produced[source.stem] = output
            check_volume(disk)
            print(f"self-host stage: batch {index}/{len(groups)} compiled "
                  f"{len(group)} translation units identically")
        # The self-hosted linker links the objects the self-hosted compiler made.
        disk = work / "link.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", disk)
        check_volume(disk)
        link_manifest = work / "link.txt"
        write_manifest(link_manifest, [(IMAGE_NAME, stage1)])
        embed(disk, link_manifest)
        for source in sources:
            name = names[source.stem]
            subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                            str(disk), str(produced[source.stem]),
                            f"{name}.O"], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
        library_names: list[str] = []
        for index, library_object in enumerate(library_objects):
            name = f"{staged_name(len(sources) + index)}.O"
            library_names.append(name)
            subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                            str(disk), str(library_object), name], cwd=ROOT,
                           check=True, stdout=subprocess.DEVNULL)
        check_volume(disk)
        inputs = " ".join([f"{names[source.stem]}.O" for source in sources] +
                          library_names)
        # The volume holds the image, the objects, and the image the link
        # produces, and that is more than its data area. The linker reads
        # each object once, so the stage asks it to release them as it
        # goes; the copies it releases are the stage's own, already
        # compared with the bootstrap objects.
        boot(disk, f"CC64S --link --format mz64 --free {inputs} -o S2.COM")
        check_volume(disk)
        stage2 = work / "stage2.mz64"
        extract(disk, "S2.COM", stage2)
        if stage2.read_bytes() != stage1.read_bytes():
            raise SystemExit("the image the self-hosted linker produced differs "
                             "from the bootstrap image")
    print(f"self-host stage: {len(sources)} translation units compiled on the "
          f"target match the bootstrap objects, and the relinked compiler image "
          f"is byte-identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
