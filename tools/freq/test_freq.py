#!python3
"""`tools/freq` 数据层的最小回归测试。

    python tools/freq/test_freq.py          # 不依赖 pytest（本机没装）
    python -m pytest tools/freq/test_freq.py -q

覆盖：probe 的偏移常量自洽（含「读哪个地址、读几字节」）、variants 的 opt.ini 往返
与缺失键兜底、products 的归属解析与 `scan_*` 的「首次只登记现状」契约、
sampler 的采样口径（相位对齐 / 丢首个 / 停机与低压判据 / 分段）、report 的两张表与
JSON（schema=1）、driver 的前置断言（游戏没跑时返回结构化结果）。
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

from tools.freq import driver, probe, products, report, sampler, variants  # noqa: E402

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
        calc = probe.hz_from_cycle(t0, t1)
        assert calc is not None
        self.assertAlmostEqual(calc, 1.0e6)
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


class _FakeClock:
    def __init__(self, t: float = 0.0):
        self.t = t

    def __call__(self) -> float:
        return self.t


def _reader(seq):
    """按顺序发牌的 read 桩：每次 poll 取一个 (hz, game_time)。"""
    it = iter(seq)
    return lambda _h, _b: next(it)


class SamplerLoopTest(unittest.TestCase):
    """采样循环的三条口径：相位对齐 / 兜底间隔 / 丢首个。"""

    def test_phase_alignment_samples_only_on_new_game_time(self):
        # game_time 不变 → 不记（除非到 interval）；变了 → 立刻记
        s = sampler.Sampler(0, 0, interval=999.0,
                            read=_reader([(1e6, 100.0), (1e6, 100.0), (2e6, 101.0)]),
                            clock=_FakeClock())
        first, second, third = s.poll(), s.poll(), s.poll()
        assert first is not None and third is not None
        self.assertEqual(first.hz, 1e6)
        self.assertIsNone(second)            # 同一个相位，且没到兜底间隔
        self.assertEqual(third.hz, 2e6)      # 游戏重算了 → 新样本
        self.assertEqual(s.taken, 2)

    def test_interval_fallback_when_game_time_frozen(self):
        clock = _FakeClock()
        s = sampler.Sampler(0, 0, interval=1.0,
                            read=_reader([(1e6, 5.0), (1e6, 5.0), (1e6, 5.0)]),
                            clock=clock)
        self.assertIsNotNone(s.poll())       # 首个
        clock.t = 0.5
        self.assertIsNone(s.poll())          # 没到 1s、相位没变
        clock.t = 1.0
        self.assertIsNotNone(s.poll())       # 兜底间隔到了

    def test_drop_first(self):
        s = sampler.Sampler(0, 0, interval=0.0, drop=1,
                            read=_reader([(1e6, 1.0), (2e6, 2.0), (3e6, 3.0)]),
                            clock=_FakeClock())
        a, b, c = s.poll(), s.poll(), s.poll()
        self.assertIsNone(a)                 # 开头第一个被丢
        assert b is not None and c is not None
        self.assertEqual(b.hz, 2e6)
        self.assertEqual(c.hz, 3e6)
        self.assertEqual(s.taken, 3)         # 丢的那个也占相位

    def test_read_failure_is_none(self):
        s = sampler.Sampler(0, 0, read=lambda _h, _b: None, clock=_FakeClock())
        self.assertIsNone(s.poll())

    def test_collect_returns_after_drop(self):
        reads = [(float(i), float(i)) for i in range(1, 9)]      # 8 个点
        out = sampler.collect(0, 0, 8, drop=1, interval=0.0,
                              sleep=lambda _d: None, read=_reader(reads),
                              clock=_FakeClock())
        self.assertEqual([s.hz for s in out], [2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0])


class StopLowCriteriaTest(unittest.TestCase):
    """停机与低压判据（facts.md §采样判据 ①②）。"""

    def test_stop_sentinel(self):
        self.assertTrue(sampler.is_stopped(0.0))
        self.assertFalse(sampler.is_stopped(1.0))

    def test_low_nonzero_is_signal_not_noise(self):
        self.assertTrue(sampler.is_low(10_000.0))    # 电路自带限速：真信号
        self.assertFalse(sampler.is_low(0.0))        # 0 是停机，不是「低压」
        self.assertFalse(sampler.is_low(2e6))        # 正常频率
        # 阈值与 report.hz_style 共用一处
        self.assertEqual(sampler.LOW_HZ, 1e6)


class SegmenterTest(unittest.TestCase):
    def _segmenter(self):
        closed: list[sampler.Segment] = []
        return sampler.Segmenter(closed.append), closed

    def test_splits_on_zero_and_on_value_change_keeping_first(self):
        seg, closed = self._segmenter()
        for t, hz in [(0.0, 1e6), (1.0, 1e6), (2.0, 0.0), (3.0, 0.0), (4.0, 2e6)]:
            seg.add(t, hz)
        seg.close()
        self.assertEqual([s.count for s in closed], [2, 2, 1])
        self.assertEqual([s.values for s in closed], [[1e6, 1e6], [0.0, 0.0], [2e6]])

    def test_tolerant_within_seg_tol(self):
        seg, closed = self._segmenter()
        seg.add(0.0, 1_000_000.0)
        seg.add(1.0, 1_040_000.0)        # +4% < 5% → 同段
        seg.close()
        self.assertEqual([s.count for s in closed], [2])

    def test_segment_stats_median_spread_transient_duration(self):
        s = sampler.Segment([sampler.Sample(0.0, 1_000_000.0),
                             sampler.Sample(1.0, 1_100_000.0),
                             sampler.Sample(2.0, 1_200_000.0)])
        self.assertEqual(s.median, 1_100_000.0)
        self.assertAlmostEqual(s.spread, 200_000 / 1_100_000 * 100)
        self.assertFalse(s.transient)               # 3 个不算过渡
        self.assertEqual(s.duration, 3.0)
        self.assertTrue(sampler.Segment([sampler.Sample(0.0, 1.0)] * 2).transient)

    def test_stopped_segment(self):
        s = sampler.Segment([sampler.Sample(0.0, 0.0), sampler.Sample(1.0, 0.0)])
        self.assertTrue(s.stopped)
        self.assertEqual(s.spread, 0.0)


class ReportTableTest(unittest.TestCase):
    """两张表 + TSV 列契约 + JSON schema（决策 3 / 决策 5）。"""

    def _patch(self, fp=None, opt_info=None, mode=None):
        """临时替掉 report 里对 products 的查询 —— 离线喂构造数据（用完恢复）。"""
        if fp is not None:
            self.addCleanup(setattr, report.products, "product_fp",
                            report.products.product_fp)
            report.products.product_fp = fp
        if opt_info is not None:
            self.addCleanup(setattr, report.products, "opt_info",
                            report.products.opt_info)
            report.products.opt_info = opt_info
        if mode is not None:
            self.addCleanup(setattr, report.products, "mode_from_hook_log",
                            report.products.mode_from_hook_log)
            report.products.mode_from_hook_log = mode

    def _rows(self):
        return [
            report.BenchRow(mode="off", num="011", fp="ae1e2f3a",
                            hz=[5333878.0, 5334606.0, 5342389.0], compiler="reference",
                            impl="reference"),
            report.BenchRow(mode="off", num="012", fp="805b43fb",
                            hz=[6791240.0, 6693626.0, 6767778.0], compiler="ours",
                            impl="ours"),
        ]

    def test_human_table_uses_median_and_renamed_header(self):
        self._patch(fp=lambda _n: "-", opt_info=lambda _n: ("-", "", "?"))
        header = report.pad("中位数", 8)
        lines, base_hz = report.bench_table(self._rows())
        self.assertIn(header, lines[0])
        self.assertNotIn("平均", lines[0])
        import statistics
        self.assertEqual(base_hz, statistics.median([5333878.0, 5334606.0, 5342389.0]))

    def test_tsv_columns_and_missing_placeholder(self):
        rows = self._rows()
        rows[0].compiler = ""
        rows[0].impl = ""
        lines = report.bench_tsv(rows).splitlines()
        self.assertEqual(lines[1].split("\t"),
                         ["mode", "num", "fp", "median_hz", "sample_n",
                          "note", "samples", "compiler", "impl"])
        self.assertTrue(lines[2].endswith("\t-\t-"))      # 历史缺列 → `-` 不是空串
        self.assertTrue(lines[3].endswith("\tours\tours"))
        self.assertIn("median_hz", lines[1])
        self.assertNotIn("mean_hz", lines[1])

    def test_json_schema_and_required_fields(self):
        rep = report.build_report(self._rows(), when="2026-10-18 00:00:00")
        self.assertEqual(rep.schema, 1)
        d = rep.to_dict()
        self.assertEqual(d["schema"], 1)
        self.assertEqual(d["baseline"], "reference:off")
        for v in d["variants"]:
            for key in ("compiler", "rewrite", "product_sha", "impl", "samples",
                        "median_hz", "spread_pct", "relative_pct", "fallback_rate"):
                self.assertIn(key, v)
        self.assertEqual(d["variants"][0]["relative_pct"], 0.0)

    def test_fallback_rate_is_per_variant_not_per_rewrite(self):
        rows = self._rows() + [report.BenchRow(
            mode="off", num="013", hz=[1e6, 2e6, 3e6],
            compiler="ours_with_fallback", impl="fallback")]
        variants = {v.variant: v for v in report.build_report(rows).variants}
        self.assertEqual(variants["ours:off"].fallback_rate, 0.0)
        self.assertEqual(variants["ours_with_fallback:off"].fallback_rate, 1.0)
        self.assertEqual(variants["ours_with_fallback:off"].fallback_reasons,
                         {"fallback": 1})

    def test_summarize_row_matches_history_shape(self):
        self._patch(fp=lambda _n: "ae1e2f3a", opt_info=lambda _n: ("ae1e2f3a", "0", "off"),
                    mode=lambda _n: "off")
        region = sampler.Region("#004", "004")
        region.add(sampler.Segment([sampler.Sample(float(i), v) for i, v in enumerate(
            [5297874.0, 5302115.0, 5306357.0])]))
        region.add(sampler.Segment([sampler.Sample(0.0, 0.0)]))
        title, lines, digest = report.summarize([region])
        self.assertEqual(lines[1], " 1) #004  5.302   3   0.16 ae1e2f3a     0  off")
        self.assertTrue(title.startswith("1 个编译区间 · 共 4 个样本"))

    def test_summary_text_ends_with_newline_per_line(self):
        from datetime import datetime
        text = report.summary_text("1 个编译区间 · 共 0 个样本 · 约 0s", [],
                                   when=datetime(2026, 10, 15, 19, 53, 21))
        self.assertEqual(text.splitlines()[0],
                         "=== 2026-10-15 19:53:21  1 个编译区间 · 共 0 个样本 · 约 0s ===")


class DriverPreflightTest(unittest.TestCase):
    """前置断言：游戏没跑时返回结构化 Check（不抛异常）、失败解释可执行。"""

    def test_game_not_running_is_structured(self):
        real = driver.probe.find_game
        driver.probe.find_game = lambda: None
        try:
            checks = driver.preflight()
        finally:
            driver.probe.find_game = real
        names = [c.name for c in checks]
        for want in ("游戏在跑", "PID 稳定", "偏移可读", "无阻塞弹窗",
                     "能抢前台", "撤销栈非空"):
            self.assertIn(want, names)
        first = checks[0]
        self.assertEqual(first.name, "游戏在跑")
        self.assertFalse(first.ok)
        self.assertTrue(first.hint)                          # 可执行的下一步
        self.assertTrue(all(c.hint for c in checks if not c.ok))

    def test_undo_stack_check_is_marked_unverifiable(self):
        real = driver.probe.find_game
        driver.probe.find_game = lambda: None
        try:
            checks = {c.name: c for c in driver.preflight()}
        finally:
            driver.probe.find_game = real
        self.assertFalse(checks["撤销栈非空"].verifiable)      # 查不了，不是通过

    def test_action_wraps_simctl_without_raising(self):
        real = driver.simctl.run
        driver.simctl.run = lambda: driver.simctl.Result(False, "run", "游戏未运行")
        try:
            r = driver.run()
        finally:
            driver.simctl.run = real
        self.assertFalse(r.ok)
        self.assertEqual(r.action, "run")
        self.assertIn("Steam", r.hint)                        # 失败解释可执行


if __name__ == "__main__":
    unittest.main(verbosity=2)
