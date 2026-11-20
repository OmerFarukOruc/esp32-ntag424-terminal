# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
# SPDX-License-Identifier: Apache-2.0
import hashlib
import io
import pathlib
import struct
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))

import check_release
from release_files import LABEL_PATTERN, source_archive, source_prefix

LABEL = "v1.0.0"
PREFIX = source_prefix(LABEL)
MAGIC = struct.pack("<I", 0xABCD5432)
PROGBITS, NOBITS = 1, 8
ALLOC, WRITE, EXEC = 0x2, 0x1, 0x4


def descriptor(version: bytes, elf_sha256: bytes = bytes(32)) -> bytes:
    return MAGIC + bytes(12) + version.ljust(32, b"\0")[:32] + bytes(96) + elf_sha256 + bytes(80)


def elf(sections: list[tuple[bytes, int, int, bytes]], ident: bytes = b"\x7fELF\x01\x01\x01") -> bytes:
    names = b"\0" + b"".join(name + b"\0" for name, _, _, _ in sections) + b".shstrtab\0"
    body = bytearray(52)
    headers = [bytes(40)]
    name_at = 1
    for name, kind, flags, data in sections:
        size = len(data)
        headers.append(struct.pack("<10I", name_at, kind, flags, 0, len(body), size, 0, 0, 4, 0))
        if kind != NOBITS:
            body += data
        name_at += len(name) + 1
    headers.append(struct.pack("<10I", name_at, 3, 0, 0, len(body), len(names), 0, 0, 1, 0))
    body += names
    body += bytes(-len(body) % 4)
    body[0:7] = ident
    struct.pack_into("<HHI", body, 0x10, 2, 94, 1)
    struct.pack_into("<I", body, 0x20, len(body))
    struct.pack_into("<HHHHHH", body, 0x28, 52, 0, 0, 40, len(headers), len(headers) - 1)
    return bytes(body) + b"".join(headers)


def archive(members: list[tarfile.TarInfo], contents: dict[str, bytes] | None = None) -> bytes:
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tar:
        for m in members:
            if m.isreg():
                data = (contents or {}).get(m.name, b"x")
                m.size = len(data)
                tar.addfile(m, io.BytesIO(data))
            else:
                tar.addfile(m)
    return out.getvalue()


def member(name: str, kind: bytes = tarfile.REGTYPE, link: str = "") -> tarfile.TarInfo:
    m = tarfile.TarInfo(name)
    m.type = kind
    m.linkname = link
    return m


class TempDir(unittest.TestCase):
    def temp(self) -> pathlib.Path:
        return pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))


class Extract(TempDir):
    def prepare(self, members: list[tarfile.TarInfo], contents: dict[str, bytes] | None = None):
        root = self.temp()
        path = root / source_archive(LABEL)
        path.write_bytes(archive(members, contents))
        work = root / "work"
        work.mkdir()
        return path, work

    def assert_refused(self, members: list[tarfile.TarInfo]) -> None:
        path, work = self.prepare(members)
        with self.assertRaises(check_release.CheckError):
            check_release.extract(path, work)
        self.assertEqual(list(work.rglob("*")), [], "a refused archive wrote files")

    def test_regular_files_extract_under_the_prefix(self):
        contents = {f"{PREFIX}/main/main.c": b"int x;", f"{PREFIX}/version.txt": b"v1.0.0\n"}
        members = [member(name) for name in contents]
        members[0].mode = 0o6777
        path, work = self.prepare(members, contents)
        src = check_release.extract(path, work)
        self.assertEqual(src, work / PREFIX)
        found = {str(p.relative_to(src)): p.read_bytes() for p in src.rglob("*") if p.is_file()}
        self.assertEqual(found, {"main/main.c": b"int x;", "version.txt": b"v1.0.0\n"})
        self.assertEqual((src / "main/main.c").stat().st_mode & 0o7022, 0, "setuid, setgid or shared write bits kept")

    def test_a_parent_step_is_refused(self):
        self.assert_refused([member(f"{PREFIX}/ok"), member(f"{PREFIX}/../evil")])

    def test_another_root_is_refused(self):
        self.assert_refused([member(f"{PREFIX}/ok"), member(f"{PREFIX}-x/evil")])

    def test_a_bare_prefix_file_is_refused(self):
        self.assert_refused([member(PREFIX)])

    def test_a_symlink_is_refused(self):
        self.assert_refused([member(f"{PREFIX}/ok"), member(f"{PREFIX}/link", tarfile.SYMTYPE, "ok")])

    def test_two_members_for_one_path_are_refused(self):
        for second in (f"{PREFIX}/a", f"{PREFIX}/./a", f"{PREFIX}//a"):
            with self.subTest(second=second):
                self.assert_refused([member(f"{PREFIX}/a"), member(second)])


class ElfReading(TempDir):
    def write(self, data: bytes) -> pathlib.Path:
        path = self.temp() / "app.elf"
        path.write_bytes(data)
        return path

    def version_of(self, section: bytes) -> str:
        path = self.write(elf([(b".flash.appdesc", PROGBITS, ALLOC, section)]))
        return check_release.elf_version(check_release.elf_sections(path), path)

    def test_the_descriptor_version_is_read(self):
        self.assertEqual(self.version_of(descriptor(b"v1.0.0")), "v1.0.0")

    def test_a_version_of_31_characters_is_read(self):
        self.assertEqual(self.version_of(descriptor(b"v" * 31)), "v" * 31)

    def test_a_version_without_a_terminator_is_refused(self):
        with self.assertRaises(check_release.CheckError):
            self.version_of(descriptor(b"v" * 32))

    def test_a_descriptor_with_another_magic_or_cut_short_is_refused(self):
        for section in (b"\0" * 4 + descriptor(b"v1")[4:], descriptor(b"v1")[:175]):
            with self.subTest(length=len(section)), self.assertRaises(check_release.CheckError):
                self.version_of(section)

    def test_a_missing_descriptor_is_refused(self):
        path = self.write(elf([(b".flash.text", PROGBITS, ALLOC | EXEC, bytes(16))]))
        with self.assertRaises(check_release.CheckError):
            check_release.elf_version(check_release.elf_sections(path), path)

    def test_only_32_bit_little_endian_elf_is_read(self):
        sections = [(b".flash.text", PROGBITS, ALLOC | EXEC, bytes(16))]
        for ident in (b"\x7fELF\x02\x01\x01", b"\x7fELF\x01\x02\x01"):
            with self.subTest(ident=ident), self.assertRaises(check_release.CheckError):
                check_release.elf_sections(self.write(elf(sections, ident)))

    def test_a_section_longer_than_the_file_is_refused(self):
        data = bytearray(elf([(b".flash.text", PROGBITS, ALLOC | EXEC, bytes(64))]))
        (shoff,) = struct.unpack_from("<I", data, 0x20)
        struct.pack_into("<I", data, shoff + 40 + 20, 4096)
        with self.assertRaises(check_release.CheckError):
            check_release.elf_sections(self.write(bytes(data)))

    def test_only_loaded_progbits_sections_other_than_the_descriptor_are_compared(self):
        path = self.write(
            elf(
                [
                    (b".flash.appdesc", PROGBITS, ALLOC, descriptor(b"v1")),
                    (b".flash.text", PROGBITS, ALLOC | EXEC, b"text"),
                    (b".flash.rodata", PROGBITS, ALLOC, b"rodata"),
                    (b".data", PROGBITS, ALLOC | WRITE, b"data"),
                    (b".bss", NOBITS, ALLOC | WRITE, bytes(8)),
                    (b".debug_info", PROGBITS, 0, b"debug"),
                ]
            )
        )
        self.assertEqual(
            check_release.loaded_bytes(check_release.elf_sections(path)),
            {b".flash.text": b"text", b".flash.rodata": b"rodata", b".data": b"data"},
        )

    def test_the_image_must_hold_exactly_one_whole_descriptor(self):
        sha = hashlib.sha256(b"elf").digest()
        one = self.write(b"\xff" * 16 + descriptor(b"v1", sha) + b"\xff" * 16)
        self.assertEqual(check_release.image_elf_sha256(one), sha)
        for data in (b"\xff" * 64, descriptor(b"v1", sha) * 2, b"\xff" * 16 + descriptor(b"v1", sha)[:175]):
            with self.subTest(length=len(data)), self.assertRaises(check_release.CheckError):
                check_release.image_elf_sha256(self.write(data))


class BuildInputs(TempDir):
    def test_sdkconfig_settings_compare_without_comments_blanks_or_order(self):
        root = self.temp()
        a, b, c, d = root / "a", root / "b", root / "c", root / "d"
        a.write_text("# header\nCONFIG_A=y\n\n# CONFIG_B is not set\nCONFIG_C=1\n")
        b.write_text("CONFIG_C=1\n# CONFIG_B is not set\n# other comment\nCONFIG_A=y\n")
        c.write_text("CONFIG_A=y\nCONFIG_B=y\nCONFIG_C=1\n")
        d.write_text("CONFIG_A=y\nCONFIG_C=1\n")
        self.assertEqual(check_release.semantic_sdkconfig(a), check_release.semantic_sdkconfig(b))
        for other in (c, d):
            with self.subTest(other=other.name):
                self.assertNotEqual(check_release.semantic_sdkconfig(a), check_release.semantic_sdkconfig(other))

    def test_tree_hashes_change_with_content_and_files(self):
        root = self.temp()
        (root / "comp").mkdir()
        (root / "comp" / "a.c").write_bytes(b"a")
        before = check_release.tree_hashes(root)
        (root / "comp" / "a.c").write_bytes(b"b")
        changed = check_release.tree_hashes(root)
        (root / "comp" / "new.c").write_bytes(b"n")
        added = check_release.tree_hashes(root)
        (root / "comp" / "a.c").unlink()
        removed = check_release.tree_hashes(root)
        self.assertEqual(set(before), {"comp/a.c"})
        self.assertNotEqual(changed["comp/a.c"], before["comp/a.c"])
        self.assertEqual(set(added), {"comp/a.c", "comp/new.c"})
        self.assertEqual(set(removed), {"comp/new.c"})


class Labels(unittest.TestCase):
    def test_label_rule(self):
        cases = {
            "v" + "1" * 30: True,
            "v1.0+b-2_x": True,
            "RC1": True,
            "1.0": True,
            "v" + "1" * 31: False,
            "v1/0": False,
            "": False,
            "v 1": False,
            "vé1": False,
            "v1\n": False,
        }
        for label, ok in cases.items():
            with self.subTest(label=label):
                self.assertEqual(LABEL_PATTERN.fullmatch(label) is not None, ok)


if __name__ == "__main__":
    unittest.main()
