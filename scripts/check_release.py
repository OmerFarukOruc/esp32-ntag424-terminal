#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
#
# SPDX-License-Identifier: Apache-2.0
"""Rebuild a release candidate from its source archive and compare it with the release.

Usage: check_release.py DIST_DIR WORK_DIR

DIST_DIR holds what package_release.py wrote. The archive is extracted into the
empty WORK_DIR, outside any Git checkout, and built with the ESP-IDF on PATH.
The check fails unless the merged image was made from the released ELF, the
released ELF, version.txt and the rebuilt ELF report the same version, the
rebuilt ELF loads the same bytes as the released one, and the build left the
archived sdkconfig settings, dependencies.lock and managed components unchanged.
"""

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath

from release_files import ELF, PROJECT, SOURCE_SUFFIX, image, label_of_source_archive, source_prefix

BUILD_TIMEOUT_S = 20 * 60
APP_DESC_MAGIC = struct.pack("<I", 0xABCD5432)
APP_DESC_SECTION = b".flash.appdesc"
APP_DESC_VERSION = slice(16, 48)
APP_DESC_ELF_SHA256 = slice(144, 176)
SHT_PROGBITS = 1
SHF_ALLOC = 0x2
SDKCONFIG_UNSET = re.compile(r"# CONFIG_\w+ is not set")


class CheckError(Exception):
    pass


def elf_sections(elf: Path) -> dict[bytes, tuple[int, int, bytes]]:
    data = elf.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise CheckError(f"{elf} is not a 32-bit little-endian ELF")
    (shoff,) = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    headers = [struct.unpack_from("<IIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    names_at = headers[shstrndx][4]
    sections = {}
    for name_at, kind, flags, _, offset, size in headers:
        name = data[names_at + name_at : data.index(b"\0", names_at + name_at)]
        body = data[offset : offset + size] if kind == SHT_PROGBITS else b""
        if len(body) != (size if kind == SHT_PROGBITS else 0):
            raise CheckError(f"{elf} is truncated in section {name.decode(errors='replace')}")
        sections[name] = (kind, flags, body)
    return sections


def descriptor_version(descriptor: bytes, where: str) -> str:
    if len(descriptor) < APP_DESC_ELF_SHA256.stop or not descriptor.startswith(APP_DESC_MAGIC):
        raise CheckError(f"{where} has no application descriptor")
    field = descriptor[APP_DESC_VERSION]
    if b"\0" not in field:
        raise CheckError(f"{where} has a version with no terminating NUL")
    return field.split(b"\0")[0].decode("ascii")


def elf_version(sections: dict[bytes, tuple[int, int, bytes]], elf: Path) -> str:
    if APP_DESC_SECTION not in sections:
        raise CheckError(f"{elf} has no {APP_DESC_SECTION.decode()} section")
    return descriptor_version(sections[APP_DESC_SECTION][2], str(elf))


def loaded_bytes(sections: dict[bytes, tuple[int, int, bytes]]) -> dict[bytes, bytes]:
    return {
        name: body
        for name, (kind, flags, body) in sections.items()
        if kind == SHT_PROGBITS and flags & SHF_ALLOC and name != APP_DESC_SECTION
    }


def image_elf_sha256(path: Path) -> bytes:
    data = path.read_bytes()
    at = data.find(APP_DESC_MAGIC)
    if at < 0 or data.find(APP_DESC_MAGIC, at + 1) >= 0:
        raise CheckError(f"{path} must hold exactly one application descriptor")
    descriptor = data[at : at + APP_DESC_ELF_SHA256.stop]
    descriptor_version(descriptor, str(path))
    return descriptor[APP_DESC_ELF_SHA256]


def only(paths: list[Path], what: str) -> Path:
    if len(paths) != 1:
        raise CheckError(f"expected one {what}, found {len(paths)}")
    return paths[0]


def extract(archive: Path, work: Path) -> Path:
    prefix = source_prefix(label_of_source_archive(archive.name))
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        seen = set()
        for m in members:
            parts = PurePosixPath(m.name).parts
            if len(parts) < 2 or parts[0] != prefix or ".." in parts or not (m.isreg() or m.isdir()):
                raise CheckError(f"archive member {m.name!r} is not a file or folder under {prefix}/")
            if m.name in seen:
                raise CheckError(f"archive member {m.name!r} appears twice")
            seen.add(m.name)
        tar.extractall(work, members=members, filter="data")
    return work / prefix


def semantic_sdkconfig(path: Path) -> list[str]:
    lines = path.read_text().splitlines()
    return sorted(line for line in lines if line.startswith("CONFIG_") or SDKCONFIG_UNSET.fullmatch(line))


def tree_hashes(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob("*") if p.is_file()}


def check(dist: Path, work: Path) -> None:
    if work.exists() and any(work.iterdir()):
        raise CheckError(f"{work} is not empty")
    work.mkdir(parents=True, exist_ok=True)
    archive = only(sorted(dist.glob(f"{PROJECT}-*{SOURCE_SUFFIX}")), "source archive")
    released_elf = only(sorted(dist.glob(ELF)), "ELF")
    merged = only(sorted(dist.glob(image(label_of_source_archive(archive.name)))), "merged image")
    if image_elf_sha256(merged) != hashlib.sha256(released_elf.read_bytes()).digest():
        raise CheckError(f"{merged.name} was not made from {released_elf.name}")
    released = elf_sections(released_elf)
    released_version = elf_version(released, released_elf)

    src = extract(archive, work)
    archived = (src / "version.txt").read_text().strip()
    if released_version != archived:
        raise CheckError(f"the released firmware reports {released_version!r}, version.txt says {archived!r}")
    sdkconfig = semantic_sdkconfig(src / "sdkconfig")
    lock = (src / "dependencies.lock").read_bytes()
    managed = tree_hashes(src / "managed_components")
    log = work / "build.log"
    with open(log, "wb") as out:
        result = subprocess.run(
            ["idf.py", "-C", str(src), "build"], stdout=out, stderr=subprocess.STDOUT, timeout=BUILD_TIMEOUT_S
        )
    if result.returncode != 0:
        raise CheckError(f"idf.py build exited {result.returncode}; see {log}")

    rebuilt_elf = src / "build" / ELF
    rebuilt = elf_sections(rebuilt_elf)
    rebuilt_version = elf_version(rebuilt, rebuilt_elf)
    if rebuilt_version != archived:
        raise CheckError(f"the rebuilt firmware reports {rebuilt_version!r}, version.txt says {archived!r}")
    before, after = loaded_bytes(released), loaded_bytes(rebuilt)
    differing = sorted(name.decode() for name in before.keys() | after.keys() if before.get(name) != after.get(name))
    if differing:
        raise CheckError(f"the rebuilt firmware loads other bytes in {', '.join(differing)}")
    if semantic_sdkconfig(src / "sdkconfig") != sdkconfig:
        raise CheckError("the build changed a setting in the archived sdkconfig")
    if (src / "dependencies.lock").read_bytes() != lock:
        raise CheckError("the build changed dependencies.lock")
    if tree_hashes(src / "managed_components") != managed:
        raise CheckError("the build changed managed_components")
    print(f"{archive.name} rebuilds as {rebuilt_version} and loads the same bytes as the released firmware")


def main() -> int:
    parser = argparse.ArgumentParser(description="Rebuild a release candidate from its source archive.")
    parser.add_argument("dist_dir", type=Path)
    parser.add_argument("work_dir", type=Path)
    args = parser.parse_args()
    try:
        check(args.dist_dir.resolve(), args.work_dir.resolve())
    except (CheckError, tarfile.TarError, OSError, struct.error, ValueError, subprocess.TimeoutExpired) as e:
        print(f"check_release: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
