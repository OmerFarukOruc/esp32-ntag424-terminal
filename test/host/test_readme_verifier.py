# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
# SPDX-License-Identifier: Apache-2.0
import ast
import math
import pathlib
import re
import unittest

README = pathlib.Path(__file__).resolve().parents[2] / "README.md"
PICC = "EF963FF7828658A599F3041510671E88"
CMAC = "94EED9EE65337086"
ACCEPTED = ("04DE5F1EACC040", 61)
OPENING_FENCE = re.compile(r" {0,3}(`{3,}|~{3,})\s*(\S*)")
PYTHON_INFO = {"py", "python", "python3"}


class Count(int):
    pass


def fenced_python_blocks(text):
    blocks = []
    fence = None
    for line in text.splitlines(keepends=True):
        if fence is None:
            opening = OPENING_FENCE.match(line)
            if opening:
                fence = (opening.group(1), opening.group(2) in PYTHON_INFO, [])
        elif re.fullmatch(r" {0,3}" + re.escape(fence[0][0]) + "{" + str(len(fence[0])) + r",}\s*", line):
            if fence[1]:
                blocks.append("".join(fence[2]))
            fence = None
        else:
            fence[2].append(line)
    return blocks


def load_verify():
    blocks = [
        block
        for block in fenced_python_blocks(README.read_text(encoding="utf-8"))
        if any(isinstance(node, ast.FunctionDef) and node.name == "verify" for node in ast.parse(block).body)
    ]
    if len(blocks) != 1:
        raise AssertionError(f"README.md has {len(blocks)} Python blocks that define verify, expected 1")
    namespace = {}
    exec(compile(blocks[0], str(README), "exec"), namespace)
    return namespace["verify"]


verify = load_verify()


class ReadmeVerifier(unittest.TestCase):
    def test_counter_above_history_is_accepted(self):
        for history in (60, -1):
            with self.subTest(history=history):
                self.assertEqual(verify(PICC, CMAC, history), ACCEPTED)

    def test_lowercase_hex_is_accepted(self):
        self.assertEqual(verify(PICC.lower(), CMAC.lower(), 60), ACCEPTED)

    def test_counter_not_above_history_is_refused(self):
        for history in (61, 62, 0xFFFFFF):
            with self.subTest(history=history):
                self.assertIsNone(verify(PICC, CMAC, history))

    def test_changed_cmac_is_refused(self):
        self.assertIsNone(verify(PICC, "94EED9EE65337087", 60))

    def test_malformed_wire_input_is_refused(self):
        for picc, cmac in ((PICC[:30], CMAC), (PICC, CMAC[:14]), (PICC[:31] + "G", CMAC), (None, CMAC)):
            with self.subTest(picc=picc, cmac=cmac):
                self.assertIsNone(verify(picc, cmac, 60))

    def test_history_that_is_not_a_counter_is_refused(self):
        for history in (None, "60", True, False, 60.0, math.nan, -math.inf, -2, Count(60)):
            with self.subTest(history=history):
                self.assertIsNone(verify(PICC, CMAC, history))


if __name__ == "__main__":
    unittest.main()
