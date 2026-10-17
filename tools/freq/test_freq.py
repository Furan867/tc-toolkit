#!python3
"""`tools/freq` 数据层的最小回归测试。

    python tools/freq/test_freq.py          # 不依赖 pytest（本机没装）
    python -m pytest tools/freq/test_freq.py -q

覆盖：probe 的偏移常量自洽（含「读哪个地址、读几字节」）、variants 的 opt.ini 往返
与缺失键兜底、products 的归属解析与 `scan_*` 的「首次只登记现状」契约。
采样口径（分段 / 相位 / 停机判据）不在本批，等 `sampler` 落地再补。
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]        # reversing/
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.freq import probe, products, variants  # noqa: E402

BASE = probe.BASE_VA

INI_TEMPLATE = """\
; 模板，含与真 opt.ini 同形的注释行
[opt]
; compiler = ours
compiler = reference
; on / short / short_cse / off
rewrite = off
; rewrite = short
"""


class ProbeOffsetsTest(unittest.TestCase):
    def test_offsets_are_link_addr_minus_base(self):
        self.assertEqual(probe.OFF_CYCLE, 0x14624D608 - 0x140000000)
        self.assertEqual(probe.OFF_TIME, 0x14624D610 - 0x140000000)
        self.assertEqual(probe.OFF_SIM_SPEED, 0x14624D618 - 0x140000000)
        # facts.md §主频读数 记的模块内偏移（三处旧实现都算成这组数）
        self.assertEqual(
            (probe.OFF_CYCLE, probe.OFF_TIME, probe.OFF_SIM_SPEED),
            (0x624D608, 0x624D610, 0x624D618))

    def _patch_rpm(self, values: dict[int, bytes]):
        """把 winmem.rpm 换成按「相对基址的偏移」发牌的桩，返回 (恢复函数, 调用记录)。"""
        calls: list[tuple[int, int]] = []

        def fake_rpm(h, addr, size):
            calls.append((addr - BASE, size))
            return values.get(addr - BASE)

        real = probe.winmem.rpm
        probe.winmem.rpm = fake_rpm
        return (lambda: setattr(probe.winmem, "rpm", real)), calls

    def test_read_hz_reads_float_at_sim_speed(self):
        restore, calls = self._patch_rpm({probe.OFF_SIM_SPEED: struct.pack("<f", 1.5e6)})
        try:
            self.assertEqual(probe.read_hz(1, BASE), 1.5e6)   # 1.5e6 在 float32 里可精确表示
        finally:
            restore()
        self.assertEqual(calls, [(probe.OFF_SIM_SPEED, 4)])

    def test_snapshot_reads_all_three_fields(self):
        blob = {probe.OFF_SIM_SPEED: struct.pack("<f", 2.0e6),
                probe.OFF_TIME: struct.pack("<d", 1000.5),
                probe.OFF_CYCLE: struct.pack("<q", 42)}
        restore, calls = self._patch_rpm(blob)
        try:
            snap = probe.snapshot(1, BASE)
        finally:
            restore()
        assert snap is not None
        self.assertEqual((snap.hz, snap.game_time, snap.cycle), (2.0e6, 1000.5, 42))
        self.assertEqual(sorted(calls), sorted([(probe.OFF_SIM_SPEED, 4),
                                               (probe.OFF_TIME, 8),
                                               (probe.OFF_CYCLE, 8)]))

    def test_snapshot_is_none_without_hz(self):
        restore, _calls = self._patch_rpm({})
        try:
            self.assertIsNone(probe.snapshot(1, BASE))
            self.assertIsNone(probe.read_hz(1, BASE))
            self.assertIsNone(probe.read_speed_time(1, BASE))
        finally:
            restore()

    def test_hz_from_cycle(self):
        t0 = probe.Snapshot(0.0, 1000.0, 100)
        t1 = probe.Snapshot(1.0e6, 1001.0, 100 + 1_000_000)
        self.assertAlmostEqual(probe.hz_from_cycle(t0, t1), 1.0e6)
        # 时刻没前进（停机时它照常刷新，所以这不该被当成"在跑"）→ 算不出
        self.assertIsNone(probe.hz_from_cycle(t0, probe.Snapshot(0.0, 1000.0, 500)))
        self.assertIsNone(probe.hz_from_cycle(t0, probe.Snapshot(1.0, None, 100)))
        self.assertIsNone(probe.hz_from_cycle(t0, probe.Snapshot(1.0, 1001.0, None)))


class VariantsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.ini = Path(self.dir.name) / "opt.ini"
        self.ini.write_text(INI_TEMPLATE, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def test_roundtrip(self):
        variants.set_variant(variants.Variant("ours", "off"), self.ini)
        self.assertEqual(variants.read_variant(self.ini), variants.Variant("ours", "off"))
        variants.set_variant(variants.Variant("ours_with_fallback", "short_cse"), self.ini)
        self.assertEqual(variants.read_variant(self.ini),
                         variants.Variant("ours_with_fallback", "short_cse"))

    def test_write_touches_only_the_effective_lines(self):
        variants.set_variant(variants.Variant("ours", "on"), self.ini)
        text = self.ini.read_text(encoding="utf-8")
        self.assertEqual(text.count("\ncompiler = "), 1)      # 不新增行
        self.assertIn("; compiler = ours\n", text)            # 注释行原样留着
        self.assertIn("; rewrite = short\n", text)
        self.assertIn("\ncompiler = ours\n", text)
        self.assertIn("\nrewrite = on\n", text)

    def test_missing_compiler_key_falls_back_to_reference(self):
        self.ini.write_text("[opt]\nrewrite = short\n", encoding="utf-8")
        self.assertEqual(variants.read_variant(self.ini), variants.Variant("reference", "short"))

    def test_missing_rewrite_key_falls_back_to_on(self):
        self.ini.write_text("[opt]\ncompiler = ours\n", encoding="utf-8")
        self.assertEqual(variants.read_variant(self.ini), variants.Variant("ours", "on"))

    def test_illegal_value_raises(self):
        self.ini.write_text("[opt]\ncompiler = nope\nrewrite = off\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            variants.read_variant(self.ini)
        with self.assertRaises(ValueError):
            variants.Variant(rewrite="bogus")
        with self.assertRaises(ValueError):
            variants.Variant.parse("ours")

    def test_write_without_key_refuses_and_leaves_file_alone(self):
        self.ini.write_text("[opt]\nrewrite = off\n", encoding="utf-8")
        before = self.ini.read_bytes()
        with self.assertRaises(ValueError):
            variants.set_variant(variants.Variant("ours", "on"), self.ini)
        self.assertEqual(self.ini.read_bytes(), before)

    def test_parse_str(self):
        self.assertEqual(variants.Variant.parse(" ours : off "), variants.Variant("ours", "off"))
        self.assertEqual(str(variants.Variant("reference", "short")), "reference:short")


class ProductsOwnershipTest(unittest.TestCase):
    LOG = (
        "2026-10-17 17:21:36\t008\timpl\timpl=fallback\tmode=ours_with_fallback\tresult=reference\n"
        "2026-10-17 17:21:36\t008\tcfg\trewrite=short\n"
        "2026-10-17 17:21:37\t008\topt\tmode=on\trewritten=1590\tlen=219088\t"
        "sha256=deadbeef00cafe11\tnote=把 short 挡回 on\n"
        "2026-10-17 17:21:37\t008\twriteback\tlen=219088\n"
        "2026-10-17 17:21:40\t009\tcfg\trewrite=off\n"
    )

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.log = Path(self.dir.name) / "hook.log"
        self.log.write_text(self.LOG, encoding="utf-8")

    def tearDown(self):
        self.dir.cleanup()

    def test_three_lines_are_kept_apart(self):
        own = products.ownership("008", self.log)
        self.assertEqual(own.cfg_rewrite, "short")                  # 配了什么
        self.assertEqual(own.opt_mode, "on")                        # 改写器做了什么
        self.assertEqual(own.impl, "fallback")                      # 实际走了哪个实现
        self.assertEqual(own.compiler_cfg, "ours_with_fallback")    # impl 行的 mode 是 compiler
        self.assertEqual(own.impl_result, "reference")
        self.assertEqual(own.rewritten, "1590")
        self.assertEqual(own.sha_log, "deadbeef")

    def test_missing_lines_are_unknown_not_empty(self):
        own = products.ownership("009", self.log)
        self.assertEqual((own.opt_mode, own.impl, own.sha_log), ("?", "?", "-"))
        self.assertEqual(products.ownership(None, self.log).num, "?")
        self.assertEqual(products.ownership("999", self.log).cfg_rewrite, "?")

    def test_legacy_accessors_agree(self):
        self.assertEqual(products.opt_info("008", self.log),
                         ("deadbeef", "1590", "on"))
        self.assertEqual(products.mode_from_hook_log("008", self.log), "short")

    def test_hook_log_lines_counts_every_line(self):
        self.assertEqual(len(products.hook_log_lines(self.log)), 5)
        self.assertEqual(products.hook_log_lines(Path(self.dir.name) / "没这个文件"), [])


class ScanTest(unittest.TestCase):
    """`_scan` 是唯一实现，`scan_products` / `scan_captures` 只绑定了目录与名字形状。"""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name)
        for name in ("001.opt.bin", "001.raw.bin", "manifest.tsv"):
            (self.path / name).write_bytes(b"x")

    def tearDown(self):
        self.dir.cleanup()

    def test_baseline_call_then_only_new(self):
        """首次调用把现状一次登记进 `seen`（返回值由调用方丢弃），之后只报新编号。"""
        seen: set[str] = set()
        self.assertEqual(products._scan(self.path, products.PRODUCT_RE, seen), ["001"])
        (self.path / "002.opt.bin").write_bytes(b"y")
        self.assertEqual(products._scan(self.path, products.PRODUCT_RE, seen), ["002"])
        self.assertEqual(products._scan(self.path, products.PRODUCT_RE, seen), [])
        self.assertEqual(seen, {"001", "002"})

    def test_only_rewritten_products_and_by_integer_order(self):
        for name in ("009.opt.bin", "010.opt.bin"):
            (self.path / name).write_bytes(b"z")
        seen: set[str] = set()
        products._scan(self.path, products.PRODUCT_RE, seen)          # 登记现状
        self.assertEqual(sorted(seen), ["001", "009", "010"])

    def test_captures_pattern(self):
        (self.path / "hook_out_007.bin").write_bytes(b"c")
        (self.path / "hook_src_007.si").write_bytes(b"c")
        self.assertEqual(products._scan(self.path, products.CAPTURE_RE, set()), ["007"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
