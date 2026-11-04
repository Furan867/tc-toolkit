"""自检：合成 run 目录 → 跑 report.py → 断言四表数字 / HTML 自包含 / G4 / G3′ / 错误路径。

    python hotspot/analysis/test_report.py -v

testdata 全部由本文件按下面的台账**程序生成**（`ELEMENTS` / `SEGMENTS` / `LINES` /
`STUBS` 加样本表），断言里的数字是照那张台账手算出来的常量。
坐标口径：bucket=0 的 offset 是 blob 偏移（spec §2）。
"""
from __future__ import annotations

import configparser
import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPORT = HERE / "report.py"

CODE_LEN = 1024
FILE_LEN = CODE_LEN + 8

# (id, type, width, 源行区间)
ELEMENTS = [
    (1, "com_register_word", 8, [[10, 12]]),
    (2, "com_register_word", 8, [[20, 22]]),
    (3, "com_adder", 1, [[30, 34]]),
    (4, "com_cc_output", 0, [[40, 44]]),
    (5, "com_counter", 8, [[50, 54]]),
    (6, "com_cc_input", 0, [[60, 64]]),
    (7, "com_memory", 8, [[70, 74]]),
]
# (start, end, element_id)
SEGMENTS = [(0, 100, 1), (100, 200, 2), (200, 300, 3), (300, 400, 4),
            (400, 500, 5), (500, 600, 6), (700, 800, 7)]
# (start, end, 源码行) —— [600,700) 与 [800,1024) 没被任何元件段覆盖 = 公共/根代码
LINES = [(0, 50, 10), (50, 100, 11), (100, 200, 20), (600, 700, 99)]
STUBS = [(900, 912, "stub", "kernel32.VirtualAlloc")]
SHA256 = "0123456789abcdef" * 4

# run A：覆盖 bucket0 已归属 / 桩 / 公共根（有行号 + 无行号）/ bucket1 / bucket2 / 零值类型
SAMPLES_A = [
    (0, 10, 1, 30),     # reg#1 行 10
    (0, 60, 1, 20),     # reg#1 行 11
    (0, 150, 1, 40),    # reg#2 行 20
    (0, 250, 1, 10),    # adder#3，机器段无行号
    (0, 350, 1, 8),     # cc_output#4
    (0, 450, 1, 4),     # counter#5
    (0, 905, 1, 5),     # 外部调用桩
    (0, 650, 1, 12),    # 公共根，有行号
    (0, 850, 1, 3),     # 公共根，无行号
    (1, 4096, 2, 30),
    (2, 65536, 2, 20),
]
# G3′ 正例：与 A 的 top-5 重合 4
SAMPLES_D = [
    (0, 10, 1, 45), (0, 60, 1, 15),     # reg#1 = 60
    (0, 150, 1, 30),                   # reg#2 = 30
    (0, 550, 1, 20),                   # cc_input#6 = 20
    (0, 250, 1, 12),                   # adder#3 = 12
    (0, 350, 1, 6),                    # cc_output#4 = 6
]
# G3′ 负例：与 A 的 top-5 只重合 3
SAMPLES_C = [
    (0, 550, 1, 70),    # cc_input#6
    (0, 750, 1, 60),    # memory#7
    (0, 10, 1, 40),     # reg#1
    (0, 150, 1, 30),    # reg#2
    (0, 250, 1, 20),    # adder#3
]
# G4 负例：bucket0 份额掉到 24/182
SAMPLES_LOW = [
    (0, 10, 1, 10), (0, 150, 1, 10), (0, 250, 1, 4),
    (1, 4096, 2, 100),
    (2, 65536, 2, 58),
]
# 函数表用例：在 A 的公共根上多两笔 —— 610 落在内联块里（应算 stdlib 函数）、1000 没有函数信息
SAMPLES_F = SAMPLES_A + [(0, 610, 1, 5), (0, 1000, 1, 4)]
# 函数表：ranges 已经是「最内层」切分过的（全局互不重叠），decl_line 是**文件内**行号
FUNCTIONS = [
    # mode_run 自己的码：内联块 [600,620) 已经切掉给别人
    {"func_id": 0, "ranges": [[620, 700]], "name": "mode_run", "decl_line": 99,
     "file": "hook_src_009.si", "tag": "subprogram", "depth": 1, "origin": None},
    # 内联实例：名字/行号从抽象实例（func_id 2）追回，角色看文件 = stdlib
    {"func_id": 1, "ranges": [[600, 620]], "name": "memcpy", "decl_line": 120,
     "file": "stdlib_prefix.si", "tag": "inline", "depth": 2, "origin": 2},
    # 抽象实例：没有函数体，只提供身份（origin 指回它）
    {"func_id": 2, "ranges": [], "name": "memcpy", "decl_line": 120,
     "file": "stdlib_prefix.si", "tag": "subprogram", "depth": 1, "origin": None},
    {"func_id": 3, "ranges": [[800, 860]], "name": "reset_sim", "decl_line": 42,
     "file": "stdlib_prefix.si", "tag": "subprogram", "depth": 1, "origin": None},
    # 元件机器段 [0,200) 在某个函数的区间里：表 5 分母是 bucket0 全部样本，这些也要算进来
    {"func_id": 4, "ranges": [[0, 200]], "name": "run_circuit", "decl_line": 210,
     "file": "hook_src_009.si", "tag": "subprogram", "depth": 1, "origin": None},
]

RUN_INI = """\
; consumer 初写；analysis 只回写 [buckets]（spec §10）
[run]
schema = 1
created_at = "2026-11-03T10:00:00+08:00"
backend = "etw"

[artifact]
path = "work/hook/c001.bin"
sha256 = "{sha}"
file_len = {file_len}
code_len = {code_len}
num = "001"

[locate]
bases = ["0x10000000", "0x10001000"]
feature_off = 1024

[window]
sec = 10.0
interval_ms = 1.0
sample_count = {sample_count}
tids = [100, 101]

[freq]
start_hz = 1000000.0
end_hz = 1002000.0
start_at = "2025-11-01T00:06:40"
end_at = "2025-11-01T00:06:50"
interval_sec = 1.0
"""


def sidecar_doc() -> dict:
    return {
        "version": 1,
        "compiler": "ours",
        "artifact": {"len": FILE_LEN, "sha256": SHA256},
        "meta": {"opt_level": "default<O2>", "pass_pipeline": "default<O2>", "stages": {}},
        "elements": [
            {"id": eid, "type": etype, "width": width, "lines": lines, "tail": None}
            for eid, etype, width, lines in ELEMENTS
        ],
        "segments": [{"start": s, "end": e, "id": i} for s, e, i in SEGMENTS],
        "lines": [{"start": s, "end": e, "line": ln} for s, e, ln in LINES],
        "stubs": [{"start": s, "end": e, "kind": k, "name": n} for s, e, k, n in STUBS],
    }


def read_ini(path: Path) -> dict[str, dict[str, Any]]:
    """按冻结的 INI 子集读回（值都是 JSON 编码）；断言用它看回写结果。"""
    parser = configparser.ConfigParser(
        delimiters=("=",),
        interpolation=None,
        comment_prefixes=("#", ";"),
        inline_comment_prefixes=("#", ";"),
    )
    try:
        parser.read_string(path.read_text(encoding="utf-8"))
    except (OSError, configparser.Error) as exc:
        raise AssertionError(f"读不了 {path}：{exc}") from exc
    return {section: {key: json.loads(raw) for key, raw in parser.items(section, raw=True)}
            for section in parser.sections()}


def write_run(root: Path, name: str, samples: list[tuple[int, int, int, int]],
              sidecar: dict | None = None, sample_count: int | None = None) -> Path:
    run_dir = root / name
    run_dir.mkdir(parents=True)
    total = sample_count if sample_count is not None else sum(s[3] for s in samples)
    (run_dir / "run.ini").write_text(
        RUN_INI.format(sha=SHA256, file_len=FILE_LEN, code_len=CODE_LEN, sample_count=total),
        encoding="utf-8")
    rows = ["# samples.tsv v1", "bucket\tblob_offset\tthread_id\tcount"]
    rows += [f"{b}\t{o}\t{t}\t{c}" for b, o, t, c in samples]
    (run_dir / "samples.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    (run_dir / "sidecar.json").write_text(
        json.dumps(sidecar if sidecar is not None else sidecar_doc(), ensure_ascii=False, indent=2),
        encoding="utf-8")
    return run_dir


def source_bound_doc(functions: list[dict] | None = None) -> dict:
    """新格式侧车：行号/元件/函数都绑到来源文件；`functions=None` = 有文件绑定、无函数表。

    `func_id` 按「整段落在同一个函数里才算」算过：行表在 [600,700) 已把内联块与宿主合并成
    一条，所以它是 null（跨函数边界）—— 分析层的函数归因不靠这列，拿 `functions[].ranges`
    自己查，所以那些带 `func_id` 的样本照样能归到函数。
    """
    doc = sidecar_doc()
    doc["meta"] = {**doc["meta"], "sources": [
        {"file": "stdlib_prefix.si", "lines": 1503, "role": "stdlib", "path": ""},
        {"file": "hook_src_009.si", "lines": 9053, "role": "source", "path": ""}]}
    for span in doc["lines"]:
        span["file"] = "hook_src_009.si"
    for element in doc["elements"]:
        element["source"] = "hook_src_009.si"
    if functions is not None:
        doc["meta"]["functions_version"] = 1
        doc["functions"] = copy.deepcopy(functions)     # 用的是模块级台账，负例会在上面改字段
        for span in doc["lines"] + doc["segments"]:
            span["func_id"] = None
    return doc


def read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AssertionError(f"读不了 {path}：{exc}") from exc


def run_cli(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(REPORT), *args],
                          capture_output=True, text=True, encoding="utf-8")


def table_rows(markdown: str, heading: str) -> list[list[str]]:
    """取出某个 `## <heading>` 小节里第一张 markdown 表格的数据行（去掉表头与分隔行）。"""
    start = markdown.index(f"## {heading}")
    body = markdown[start:].split("\n")
    rows: list[list[str]] = []
    seen = 0
    for line in body:
        if not line.startswith("|"):
            if rows:
                break
            continue
        seen += 1
        if seen <= 2:
            continue
        rows.append([cell.strip() for cell in line.strip("|").split("|")])
    return rows


class ReportCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.workspace = self.tmp / "ws"
        self.run_a = write_run(self.workspace / "runs", "run_a", SAMPLES_A)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_four_tables_and_buckets(self) -> None:
        proc = run_cli("--run", str(self.run_a))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        markdown = (self.run_a / "report.md").read_text(encoding="utf-8")

        # 分母：表 1–3 = bucket0 已归属 112；总样本 = 182
        self.assertIn("| 总样本数 | 182 |", markdown)
        self.assertIn("| bucket0 已归属（表 1–3 分母） | 112 |", markdown)

        types = {row[0]: row[1:] for row in table_rows(markdown, "表 1")}
        self.assertEqual(types["com_register_word"], ["2", "90", "80.4%"])
        self.assertEqual(types["com_adder"], ["1", "10", "8.9%"])
        self.assertEqual(types["com_cc_output"], ["1", "8", "7.1%"])
        self.assertEqual(types["com_cc_input"], ["1", "0", "0.0%"])   # 零值类型必须列出
        self.assertEqual(types["com_memory"], ["1", "0", "0.0%"])
        self.assertEqual(len(types), len({etype for _, etype, _, _ in ELEMENTS}))

        instances = table_rows(markdown, "表 2")
        # top-N 按热度排序；实例不足 N 时零值实例排到尾部（不隐藏）
        self.assertEqual([row[0] for row in instances],
                         ["com_register_word #1", "com_register_word #2", "com_adder #3",
                          "com_cc_output #4", "com_counter #5",
                          "com_cc_input #6", "com_memory #7"])
        self.assertEqual(instances[0][1:], ["10-12", "50", "44.6%"])

        lines = {row[0]: row[1:] for row in table_rows(markdown, "表 3 热点元件行级明细")}
        self.assertEqual(lines["10"], ["30", "60.0%"])   # reg#1 行 10
        self.assertEqual(lines["11"], ["20", "40.0%"])

        buckets = table_rows(markdown, "表 4")
        self.assertEqual(buckets[0][1:], ["112", "61.5%"])     # 已归属
        self.assertEqual(buckets[1][1:], ["5", "2.7%"])        # 桩
        self.assertEqual(buckets[2][1:], ["5", "100.0%（占该桶）"])
        self.assertEqual(buckets[3][1:], ["15", "8.2%"])       # 公共根
        self.assertEqual(buckets[4][1:], ["12", "80.0%（占该桶）"])
        self.assertEqual(buckets[5][1:], ["3", "20.0%（占该桶）"])
        self.assertEqual(buckets[6][1:], ["30", "16.5%"])      # bucket1
        self.assertEqual(buckets[7][1:], ["20", "11.0%"])      # bucket2

        doc = read_ini(self.run_a / "run.ini")
        self.assertEqual(doc["buckets"]["total"], 182)
        self.assertEqual(doc["buckets"]["bucket0"], 132)
        self.assertEqual(doc["buckets"]["bucket0_attributed"], 112)
        self.assertEqual(doc["buckets"]["bucket0_stub"], 5)
        self.assertEqual(doc["buckets"]["bucket0_root"], 15)
        self.assertAlmostEqual(doc["buckets"]["bucket0_share"], 132 / 182, places=6)
        self.assertAlmostEqual(doc["buckets"]["bucket2_share"], 20 / 182, places=6)
        # 回写只碰 buckets：其余 section 与键原样还在
        self.assertEqual(doc["run"]["schema"], 1)
        self.assertEqual(doc["artifact"]["code_len"], CODE_LEN)
        self.assertEqual(doc["window"]["sample_count"], 182)

    def test_line_numbers_carry_source_file(self) -> None:
        """新 sidecar 的行号带 `file`：表 2/3 显示 `file:line`，表 4 按文件拆有行号信息的根代码。"""
        doc = sidecar_doc()
        doc["meta"] = {**doc["meta"], "sources": [
            {"file": "stdlib_prefix.si", "lines": 1503, "role": "stdlib", "path": ""},
            {"file": "hook_src_009.si", "lines": 9053, "role": "source", "path": ""}]}
        for span in doc["lines"]:
            span["file"] = "hook_src_009.si"
        for element in doc["elements"]:
            element["source"] = "hook_src_009.si"
        run_dir = write_run(self.workspace / "runs", "run_named", SAMPLES_A, sidecar=doc)

        proc = run_cli("--run", str(run_dir))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        markdown = (run_dir / "report.md").read_text(encoding="utf-8")
        instances = table_rows(markdown, "表 2")
        self.assertEqual(instances[0][0], "com_register_word #1")
        self.assertEqual(instances[0][1], "hook_src_009.si:10-12")
        lines = {row[0]: row[1:] for row in table_rows(markdown, "表 3 热点元件行级明细")}
        self.assertEqual(lines["hook_src_009.si:10"], ["30", "60.0%"])
        self.assertEqual(lines["hook_src_009.si:11"], ["20", "40.0%"])
        buckets = {row[0]: row[1:] for row in table_rows(markdown, "表 4")}
        self.assertEqual(buckets["└ 有行号信息 · hook_src_009.si"], ["12", "80.0%（占该桶）"])
        page = (run_dir / "report.html").read_text(encoding="utf-8")
        self.assertIn("hook_src_009.si:10", page)

    def test_meta_and_approximation_notes(self) -> None:
        proc = run_cli("--run", str(self.run_a))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        markdown = (self.run_a / "report.md").read_text(encoding="utf-8")
        self.assertIn(f"sha256 {SHA256}", markdown)
        self.assertIn("| 窗口 | 10 s，采样间隔 1 ms |", markdown)
        self.assertIn("近似口径", markdown)          # spec §9 的精度口径必须标注
        self.assertIn("10 s ≈", markdown)            # 粗 tick 估算

    def test_html_self_contained(self) -> None:
        proc = run_cli("--run", str(self.run_a))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        page = (self.run_a / "report.html").read_text(encoding="utf-8")
        self.assertNotIn("http", page)               # 无外链 / 无 CDN / 无 SVG 命名空间
        self.assertNotIn("<script src", page)
        self.assertNotIn("<link", page)
        for dom in ('id="table-types"', 'id="table-instances"', 'id="table-lines"',
                    'id="table-buckets"', 'id="panel-types"', 'id="panel-instances"',
                    'id="panel-lines"', 'id="panel-buckets"', 'class="row expandable"',
                    'id="detail-inst-0"', 'id="lines-inst-0"', 'class="filter"'):
            self.assertIn(dom, page)
        self.assertEqual(page.count("<button class=\"tab"), 4)

    def test_function_table_splits_root_and_ranks_functions(self) -> None:
        """表 4 的公共根按函数拆成 stdlib/source/无函数信息，表 5 按函数排名（分母 = bucket0 全部）。"""
        run_dir = write_run(self.workspace / "runs", "run_func", SAMPLES_F,
                            sidecar=source_bound_doc(FUNCTIONS))

        proc = run_cli("--run", str(run_dir))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        markdown = (run_dir / "report.md").read_text(encoding="utf-8")

        # 公共根 24 = 610(在 memcpy 内联块里，5) + 650(mode_run，12) + 850(reset_sim，3) + 1000(无函数，4)
        buckets = {row[0]: row[1:] for row in table_rows(markdown, "表 4")}
        self.assertEqual(buckets["bucket0 未归属 · 公共/根代码"], ["24", "12.6%"])
        self.assertEqual(buckets["└ stdlib 函数"], ["8", "33.3%（占该桶）"])
        self.assertEqual(buckets["└ source 函数"], ["12", "50.0%（占该桶）"])
        self.assertEqual(buckets["└ 无函数信息"], ["4", "16.7%（占该桶）"])
        self.assertNotIn("└ 有行号信息", markdown)     # 函数表在时不再按行号信息拆

        # 表 5 分母 = bucket0 全部 141：元件段 90（run_circuit）+ 公共根 20，31 个样本不在任何函数区间里
        self.assertIn("## 表 5 函数热度（分母 = bucket0 全部 141 样本）", markdown)
        functions = table_rows(markdown, "表 5")
        self.assertEqual([row[0] for row in functions],
                         ["run_circuit", "mode_run", "memcpy", "reset_sim"])
        self.assertEqual(functions[0][1:], ["hook_src_009.si:210", "90", "63.8%", "source"])
        self.assertEqual(functions[1][1:], ["hook_src_009.si:99", "12", "8.5%", "source"])
        # 两段 memcpy（内联块自己 + 抽象实例）按 name/file/line 合并；行号已拆回文件内行
        self.assertEqual(functions[2][1:], ["stdlib_prefix.si:120", "5", "3.5%", "stdlib"])
        self.assertEqual(functions[3][1:], ["stdlib_prefix.si:42", "3", "2.1%", "stdlib"])

        page = (run_dir / "report.html").read_text(encoding="utf-8")
        self.assertEqual(page.count('<button class="tab'), 5)
        self.assertIn('id="panel-functions"', page)
        self.assertIn('id="table-functions"', page)
        self.assertIn("bucket0 全部 141 样本", page)
        self.assertIn("stdlib_prefix.si:120", page)

    def test_old_sidecar_has_no_function_table(self) -> None:
        """没有 `functions` 的旧侧车：表 4 拆法与表数量都保持旧样子。"""
        proc = run_cli("--run", str(self.run_a))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        markdown = (self.run_a / "report.md").read_text(encoding="utf-8")
        self.assertNotIn("表 5", markdown)
        buckets = {row[0]: row[1:] for row in table_rows(markdown, "表 4")}
        self.assertEqual(buckets["└ 有行号信息"], ["12", "80.0%（占该桶）"])
        self.assertEqual(buckets["└ 无行号信息"], ["3", "20.0%（占该桶）"])
        page = (self.run_a / "report.html").read_text(encoding="utf-8")
        self.assertEqual(page.count('<button class="tab'), 4)
        self.assertNotIn("panel-functions", page)

    def test_bad_function_table_is_loud(self) -> None:
        """函数表的三种不自洽必须报错：版本不对 / 区间重叠（没切最内层）/ origin 悬空。"""
        def with_functions(mutate) -> dict:
            doc = source_bound_doc(FUNCTIONS)
            mutate(doc)
            return doc

        cases = [
            ("no_version", lambda d: d["meta"].pop("functions_version"), "functions_version"),
            ("overlap", lambda d: d["functions"][0].update(ranges=[[600, 700]]), "互不重叠"),
            ("bad_origin", lambda d: d["functions"][1].update(origin=99), "不在 functions 里"),
        ]
        for name, mutate, needle in cases:
            run_dir = write_run(self.workspace / "runs", name, SAMPLES_F,
                                sidecar=with_functions(mutate))
            proc = run_cli("--run", str(run_dir))
            self.assertEqual(proc.returncode, 2, f"{name}: {proc.stdout}{proc.stderr}")
            self.assertIn(needle, proc.stderr, f"{name}: {proc.stderr}")
            self.assertFalse((run_dir / "report.md").exists(), f"{name} 不该产出报告")

    def test_baseline_created_then_g4_alerts(self) -> None:
        first = run_cli("--run", str(self.run_a))
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("G4 覆盖水位：通过", first.stdout)
        baseline = read_json(self.workspace / "baseline.json")
        self.assertEqual(baseline["version"], 1)
        self.assertEqual(baseline["warn_ratio"], 0.8)
        self.assertAlmostEqual(baseline["baseline"]["bucket0_share"], 132 / 182, places=6)
        self.assertEqual(baseline["baseline"]["run"], "run_a")

        low = write_run(self.workspace / "runs", "run_low", SAMPLES_LOW)
        second = run_cli("--run", str(low))
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("G4 告警", second.stdout)
        self.assertIn("bucket0 份额 13.2%", second.stdout)
        self.assertIn('id="watermark"', (low / "report.html").read_text(encoding="utf-8"))
        self.assertIn("**G4 覆盖水位告警**", (low / "report.md").read_text(encoding="utf-8"))
        again = read_json(self.workspace / "baseline.json")
        self.assertEqual(again, baseline)            # 基线只由首跑建，后续 run 不覆盖

        index = (self.workspace / "index.html").read_text(encoding="utf-8")
        self.assertIn("run_a", index)
        self.assertIn("run_low", index)
        self.assertIn('href="runs/run_a/report.html"', index)

    def test_g3prime_overlap_four_passes(self) -> None:
        run_d = write_run(self.workspace / "runs", "run_d", SAMPLES_D)
        proc = run_cli("--compare", str(self.run_a), str(run_d))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("重合 4 项", proc.stdout)
        self.assertIn("verdict: PASS", proc.stdout)

    def test_g3prime_overlap_three_fails(self) -> None:
        run_c = write_run(self.workspace / "runs", "run_c", SAMPLES_C)
        proc = run_cli("--compare", str(self.run_a), str(run_c))
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("重合 3 项", proc.stdout)
        self.assertIn("verdict: FAIL", proc.stdout)

    def test_errors_are_loud(self) -> None:
        cases = []

        missing = self.workspace / "runs" / "no_samples"
        missing.mkdir(parents=True)
        (missing / "run.ini").write_text(RUN_INI.format(
            sha=SHA256, file_len=FILE_LEN, code_len=CODE_LEN, sample_count=0), encoding="utf-8")
        (missing / "sidecar.json").write_text(json.dumps(sidecar_doc()), encoding="utf-8")
        cases.append((missing, "samples.tsv", "缺文件"))

        for name, mutate, needle in [
            ("bad_header", lambda d: (d / "samples.tsv").write_text(
                "# samples.tsv v0\nbucket\tblob_offset\tthread_id\tcount\n", encoding="utf-8"),
             "首行必须是"),
            ("bad_range", lambda d: (d / "samples.tsv").write_text(
                "# samples.tsv v1\nbucket\tblob_offset\tthread_id\tcount\n0\t2000\t1\t5\n",
                encoding="utf-8"), "坐标越界"),
            ("bad_sidecar", lambda d: (d / "sidecar.json").write_text(
                json.dumps({**sidecar_doc(), "version": 2}), encoding="utf-8"), "version 必须是 1"),
            ("bad_overlap", lambda d: (d / "sidecar.json").write_text(
                json.dumps({**sidecar_doc(), "segments": [
                    {"start": 0, "end": 200, "id": 1},
                    {"start": 100, "end": 300, "id": 2}]}), encoding="utf-8"),
             "互不重叠"),
            ("bad_coord", lambda d: (d / "sidecar.json").write_text(
                json.dumps({**sidecar_doc(), "segments": [
                    {"start": 0, "end": 4096, "id": 1}]}), encoding="utf-8"), "坐标越界"),
            # 冻结格式要求值都是 JSON 编码：裸写一个字符串/带单位的数字必须报错，不能猜
            ("bad_value", lambda d: (d / "run.ini").write_text(
                "[run]\nschema = 1\n\n[artifact]\nfile_len = 4096 oops\n", encoding="utf-8"),
             "不是 JSON 编码"),
        ]:
            run_dir = write_run(self.workspace / "runs", name, SAMPLES_A)
            mutate(run_dir)
            cases.append((run_dir, needle, name))

        for run_dir, needle, label in cases:
            proc = run_cli("--run", str(run_dir))
            self.assertEqual(proc.returncode, 2, f"{label}: {proc.stdout}{proc.stderr}")
            self.assertIn(needle, proc.stderr, f"{label}: {proc.stderr}")
            self.assertFalse((run_dir / "report.md").exists(), f"{label} 不该产出报告")

        no_ini = self.workspace / "runs" / "no_ini"
        no_ini.mkdir(parents=True)
        proc = run_cli("--run", str(no_ini))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("缺 run.ini", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
