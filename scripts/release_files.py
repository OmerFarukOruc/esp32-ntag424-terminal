# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
#
# SPDX-License-Identifier: Apache-2.0
"""File names of a release candidate, shared by package_release.py and check_release.py."""

import re

PROJECT = "esp32-ntag424-terminal"
ELF = f"{PROJECT}.elf"
LABEL_PATTERN = re.compile(r"[A-Za-z0-9._+-]{1,31}")
SOURCE_SUFFIX = "-source.tar.gz"
APP_VERSION_BYTES = 31


def source_prefix(label: str) -> str:
    return f"{PROJECT}-{label}"


def source_archive(label: str) -> str:
    return source_prefix(label) + SOURCE_SUFFIX


def image(label: str) -> str:
    return f"{PROJECT}-{label}.bin"


def label_of_source_archive(name: str) -> str:
    return name.removeprefix(f"{PROJECT}-").removesuffix(SOURCE_SUFFIX)
