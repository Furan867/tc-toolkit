#!python3
"""`tc_common.latest_hook_product` 的单测：临时目录伪造 hook 产物，不碰真机。

判据全在文件 mtime 与 `manifest.tsv` 上，所以每个用例自己摆时间和 manifest 行。
"""
from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

import tc_common

HEADER = (
    "seq\ttime\tflags\tsrc\tsrc_len\tsrc_sha256\tout\tout_len\tout_sha256\t"
    "r0\tr1\tr2\tr3\tr4\timpl\tstdlib\n"
)


def _write(path: Path, mtime: float, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    os.utime(path, (mtime, mtime))
    return path


def _row(num: str, impl: str = "ours", stdlib: str = "on") -> str:
    return (
        f"{num}\t2026-11-03 20:00:00\t0x0\thook_src_{num}.si\t10\tsha\t"
        f"hook_out_{num}.bin\t10\tsha\t0x0\t0x0\t0x0\t0x0\t0x0\t{impl}\t{stdlib}\n"
    )


class LatestHookProductTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.hook = Path(temp.name)

    def _manifest(self, *rows: str) -> None:
        _write(self.hook / "manifest.tsv", 1.0, HEADER + "".join(rows))

    def _product(self, num: str, mtime: float) -> None:
        _write(self.hook / f"hook_src_{num}.si", mtime)
        _write(self.hook / f"hook_out_{num}.bin", mtime)

    def _latest(self) -> tc_common.HookProduct:
        product = tc_common.latest_hook_product(self.hook)
        if product is None:
            self.fail("没有选中任何 hook 产物")
        return product

    def test_picks_newest_by_mtime(self):
        self._product("001", 1_000.0)
        self._product("002", 2_000.0)
        self._manifest(_row("001"), _row("002"))

        product = self._latest()

        self.assertEqual(product.num, "002")
        self.assertEqual(product.src.name, "hook_src_002.si")
        self.assertEqual(product.out.name, "hook_out_002.bin")
        self.assertEqual(product.impl, "ours")
        self.assertTrue(product.stdlib)

    def test_same_mtime_picks_larger_num(self):
        for num in ("003", "004"):
            self._product(num, 3_000.0)
        self._manifest(_row("003"), _row("004"))

        self.assertEqual(self._latest().num, "004")

    def test_missing_src_is_skipped(self):
        self._product("001", 1_000.0)
        _write(self.hook / "hook_out_005.bin", 9_000.0)      # 源没落盘
        self._manifest(_row("001"), _row("005"))

        self.assertEqual(self._latest().num, "001")

    def test_impl_not_ours_is_skipped(self):
        self._product("001", 1_000.0)
        self._product("006", 9_000.0)
        self._manifest(_row("001"), _row("006", impl="reference"))

        self.assertEqual(self._latest().num, "001")

    def test_manifest_row_missing_pairs_by_name(self):
        self._product("007", 1_000.0)

        product = self._latest()

        self.assertEqual(product.num, "007")
        self.assertEqual(product.impl, "ours")
        self.assertFalse(product.stdlib)

    def test_no_products_returns_none(self):
        self.assertIsNone(tc_common.latest_hook_product(self.hook))
        self.assertIsNone(tc_common.latest_hook_product(self.hook / "nope"))


if __name__ == "__main__":
    unittest.main()
