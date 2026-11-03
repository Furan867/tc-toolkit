"""report.md / report.html / index.html 的排版（只读 Tables，不做任何口径计算）。

report.html 必须自包含：无外链、无 CDN、无第三方库 —— 现场常常只有一份拷出来的
run 目录，页面要能双击就开（spec §11）。
"""
from __future__ import annotations

import html
from dataclasses import dataclass

from tables import BucketRow, InstRow, Tables

CSS = """
:root { --fg:#1b1b1b; --dim:#6a6a6a; --line:#d8d8d8; --bg:#fdfdfd; --bar:#4a78c8;
        --warn:#b52b1e; --warnbg:#fdf0ee; }
* { box-sizing: border-box; }
body { margin:0; padding:24px 28px 60px; background:var(--bg); color:var(--fg);
       font:14px/1.55 -apple-system,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif; }
h1 { font-size:22px; margin:0 0 4px; }
h2 { font-size:17px; margin:28px 0 8px; }
h3 { font-size:14px; margin:18px 0 6px; color:#333; }
.dim { color:var(--dim); }
.meta { font-size:13px; }
table { border-collapse:collapse; }
table.kv { margin:10px 0 4px; font-size:13px; }
table.kv th { text-align:left; color:var(--dim); font-weight:500; padding:2px 14px 2px 0;
              vertical-align:top; white-space:nowrap; }
table.kv td { padding:2px 0; }
ul.notes { margin:6px 0 0 18px; padding:0; font-size:13px; color:var(--dim); }
ul.notes li { margin:2px 0; }
.alert { margin:14px 0; padding:10px 14px; border-left:4px solid var(--warn);
         background:var(--warnbg); color:var(--warn); font-weight:600; }
nav.tabs { display:flex; gap:4px; margin:22px 0 0; border-bottom:1px solid var(--line); }
nav.tabs .tab { border:1px solid var(--line); border-bottom:none; background:#f2f2f2;
                color:var(--dim); padding:7px 16px; font:inherit; cursor:pointer;
                border-radius:5px 5px 0 0; }
nav.tabs .tab.active { background:#fff; color:var(--fg); font-weight:600;
                       position:relative; top:1px; }
.panel { display:none; padding-top:14px; }
.panel.active { display:block; }
.toolbar { margin:0 0 8px; font-size:13px; }
.toolbar input { padding:4px 8px; border:1px solid var(--line); border-radius:4px;
                 width:240px; font:inherit; }
table.grid { width:100%; font-size:13px; background:#fff; }
table.grid thead th { text-align:left; padding:6px 10px; border-bottom:2px solid #bbb;
                      cursor:pointer; user-select:none; white-space:nowrap; }
table.grid thead th::after { content:"\\2195"; color:#bbb; margin-left:6px; font-size:11px; }
table.grid tbody td { padding:5px 10px; border-bottom:1px solid #ececec; }
table.grid td.num, table.grid th.num { text-align:right; font-variant-numeric:tabular-nums; }
table.grid tr.row:hover { background:#f6f8fc; }
table.grid tr.expandable { cursor:pointer; }
table.grid tr.expandable td:first-child::before { content:"\\25B8"; color:#999;
                                                  display:inline-block; width:12px; }
table.grid tr.detail > td { background:#fafbfd; padding:8px 10px 12px 22px; }
table.grid tr.sub > td { background:#fafbfd; color:#444; }
table.grid tr.detail table.grid { width:auto; min-width:340px; }
tr.hidden { display:none; }
.mono { font-family:ui-monospace,Consolas,monospace; font-size:12px; }
.bar { display:inline-block; width:120px; height:9px; background:#eef0f4;
       border-radius:2px; overflow:hidden; vertical-align:middle; margin-left:8px; }
.bar > span { display:block; height:100%; background:var(--bar); }
.share { white-space:nowrap; }
footer { margin-top:32px; padding-top:10px; border-top:1px solid var(--line);
         font-size:12px; color:var(--dim); }
"""

JS = r"""
(function () {
  "use strict";
  function all(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }

  all(".tab").forEach(function (btn) {
    btn.addEventListener("click", function () {
      all(".tab").forEach(function (b) { b.classList.toggle("active", b === btn); });
      all(".panel").forEach(function (p) {
        p.classList.toggle("active", p.id === btn.getAttribute("data-panel"));
      });
    });
  });

  all("table.grid").forEach(function (table) {
    var heads = all("thead th", table);
    heads.forEach(function (th, col) {
      th.addEventListener("click", function () {
        var tbody = table.tBodies[0];
        var items = all("tr.row", tbody);
        var asc = th.getAttribute("data-dir") !== "asc";
        heads.forEach(function (o) { o.removeAttribute("data-dir"); });
        th.setAttribute("data-dir", asc ? "asc" : "desc");
        var dir = asc ? 1 : -1;
        items.sort(function (a, b) {
          var ac = a.children[col], bc = b.children[col];
          var av = ac.getAttribute("data-v") || ac.textContent.trim();
          var bv = bc.getAttribute("data-v") || bc.textContent.trim();
          var an = parseFloat(av), bn = parseFloat(bv);
          if (!isNaN(an) && !isNaN(bn) && an !== bn) { return (an - bn) * dir; }
          return av < bv ? -dir : (av > bv ? dir : 0);
        });
        items.forEach(function (tr) {
          tbody.appendChild(tr);
          var det = tr.getAttribute("data-detail");
          if (det) { tbody.appendChild(document.getElementById(det)); }
        });
      });
    });
  });

  all("input.filter").forEach(function (box) {
    box.addEventListener("input", function () {
      var q = box.value.toLowerCase();
      var table = document.getElementById(box.getAttribute("data-table"));
      all("tr.row", table.tBodies[0]).forEach(function (tr) {
        var hit = tr.textContent.toLowerCase().indexOf(q) >= 0;
        tr.classList.toggle("hidden", !hit);
        var det = tr.getAttribute("data-detail");
        if (det) {
          var row = document.getElementById(det);
          if (row) {
            row.classList.toggle("hidden", !hit || row.getAttribute("data-open") !== "1");
          }
        }
      });
    });
  });

  all("tr.row[data-detail]").forEach(function (tr) {
    tr.addEventListener("click", function () {
      var row = document.getElementById(tr.getAttribute("data-detail"));
      if (!row) { return; }
      var open = row.getAttribute("data-open") === "1";
      row.setAttribute("data-open", open ? "0" : "1");
      row.classList.toggle("hidden", open);
    });
  });
})();
"""


def _pct(share: float) -> str:
    return f"{100 * share:.1f}%"


def _bar(share: float) -> str:
    width = max(0.0, min(1.0, share)) * 100
    return f'<span class="bar"><span style="width:{width:.1f}%"></span></span>'


def _share_cell(share: float) -> str:
    return (f'<td class="num share" data-v="{share:.6f}">{_pct(share)}'
            f"{_bar(share)}</td>")


def _lines_text(file: str | None, pairs: tuple[tuple[int, int], ...]) -> str:
    """行区间文本：带来源文件时写成 `file:lo-hi`（旧 sidecar 没有 file 就只写行区间）。"""
    if not pairs:
        return "（无）"
    head = f"{file}:" if file else ""
    return ", ".join(f"{head}{lo}-{hi}" for lo, hi in pairs)


def _line_label(file: str | None, line: int | None) -> str:
    """`file:line`；行号或文件缺哪个就只写能确定的那一部分。"""
    if line is None:
        return file or "（无行号）"
    return f"{file}:{line}" if file else f"{line}"


def _artifact_text(t: Tables) -> str:
    m = t.meta
    size = "（未记录）" if m.artifact_file_len is None else f"{m.artifact_file_len} B"
    code = "（未记录）" if m.artifact_code_len is None else f"代码 {m.artifact_code_len} B"
    return f"{m.artifact_path}（sha256 {m.artifact_sha256}，{size}，{code}）"


def _window_text(t: Tables) -> str:
    m = t.meta
    if m.window_sec is None and m.interval_ms is None:
        return "（未记录）"
    window = "（未记录）" if m.window_sec is None else f"{m.window_sec:g} s"
    interval = "（未记录）" if m.interval_ms is None else f"{m.interval_ms:g} ms"
    return f"{window}，采样间隔 {interval}"


# ------------------------------------------------------------------ report.md


def render_markdown(t: Tables) -> str:
    m = t.meta
    out: list[str] = []
    out.append(f"# 仿真热点报告 — {m.run_name}")
    out.append("")
    out.append("## 口径与元数据")
    out.append("")
    out.append("| 项 | 值 |")
    out.append("|---|---|")
    out.append(f"| 总样本数 | {t.total_samples} |")
    out.append(f"| bucket0 已归属（表 1–3 分母） | {t.attributed_samples} |")
    out.append(f"| 窗口 | {_window_text(t)} |")
    out.append(f"| 产物 | {_artifact_text(t)} |")
    out.append(f"| 产物编号 | {m.artifact_num} |")
    out.append(f"| 落盘时刻 | {m.created_at} |")
    out.append(f"| run 目录 | {m.run_path} |")
    out.append("")
    for note in t.notes:
        out.append(f"- {note}")
    out.append("- 表 1–3 分母 = bucket0 已归属样本；表 4 分母 = 全样本")
    if t.functions:
        out.append("- 表 5 分母 = bucket0 未归属样本（公共/根代码），函数取最内层 DIE")
    if t.watermark:
        out.append("")
        out.append(f"**G4 覆盖水位告警**：{t.watermark}")
    out.append("")

    out.append(f"## 表 1 元件类型（分母 = bucket0 已归属 {t.attributed_samples} 样本）")
    out.append("")
    out.append("| 类型 | 元件数 | 样本数 | 占比 |")
    out.append("|---|---:|---:|---:|")
    for row in t.types:
        out.append(f"| {row.type} | {row.instances} | {row.samples} | {_pct(row.share)} |")
    out.append("")

    out.append(f"## 表 2 最热 {len(t.instances)} 个元件实例（主键 (type,id)，分母同上）")
    out.append("")
    out.append("| 元件 | 源码行区间 | 样本数 | 占比 |")
    out.append("|---|---|---:|---:|")
    for row in t.instances:
        out.append(f"| {row.type} #{row.id} | {_lines_text(row.src_file, row.src_lines)} "
                   f"| {row.samples} | {_pct(row.share)} |")
    out.append("")

    out.append("## 表 3 热点元件行级明细")
    out.append("")
    if not any(row.line_rows for row in t.instances):
        out.append("（表 2 里的元件都没有行号信息 —— 侧车 `lines` 未覆盖它们的机器段）")
        out.append("")
    for row in t.instances:
        out.append(f"### {row.type} #{row.id}（{row.samples} 样本，占已归属 {_pct(row.share)}）")
        out.append("")
        if not row.line_rows:
            out.append("（该元件的机器段没有行号信息）")
            out.append("")
            continue
        out.append("| 源码行 | 样本数 | 占该元件 |")
        out.append("|---:|---:|---:|")
        for line in row.line_rows:
            out.append(f"| {_line_label(line.file, line.line)} | {line.samples} | {_pct(line.share)} |")
        out.append("")

    out.append(f"## 表 4 未归属分桶（分母 = 全样本 {t.total_samples}）")
    out.append("")
    out.append("| 分桶 | 样本数 | 占比 |")
    out.append("|---|---:|---:|")
    for bucket in t.buckets:
        out.append(f"| {bucket.label} | {bucket.samples} | {_pct(bucket.share)} |")
        for name, samples, share in bucket.detail:
            out.append(f"| └ {name} | {samples} | {_pct(share)}（占该桶） |")
    out.append("")

    if t.functions:
        root = next(b.samples for b in t.buckets if b.key == "bucket0_root")
        out.append(f"## 表 5 未归属样本的函数热度（分母 = 公共/根代码 {root} 样本）")
        out.append("")
        out.append("| 函数 | 声明位置 | 样本数 | 占比 |")
        out.append("|---|---|---:|---:|")
        for row in t.functions:
            out.append(f"| {row.name} | {_line_label(row.file, row.line)} "
                       f"| {row.samples} | {_pct(row.share)} |")
        out.append("")
        out.append("> 函数取最内层：元件机器段（表 1–3）不进这张表，它们已经归到元件了")
        out.append("")

    out.append("> 生成：hotspot/analysis/report.py（analysis 只回写 run.toml 的 `buckets`）")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------- report.html


@dataclass(frozen=True)
class IndexEntry:
    run_name: str
    created_at: str
    artifact: str
    freq_note: str
    bucket0_share: float | None
    report: str


def render_html(t: Tables) -> str:
    m = t.meta
    out: list[str] = []
    out.append("<!DOCTYPE html>")
    out.append('<html lang="zh-CN">')
    out.append("<head>")
    out.append('<meta charset="utf-8">')
    out.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
    out.append(f"<title>仿真热点报告 — {html.escape(m.run_name)}</title>")
    out.append(f"<style>{CSS}</style>")
    out.append("</head>")
    out.append("<body>")
    out.append(f"<h1>仿真热点报告 — {html.escape(m.run_name)}</h1>")
    out.append(f'<p class="dim meta">{html.escape(m.run_path)}</p>')

    out.append('<table class="kv">')
    kv = [
        ("总样本数", f"{t.total_samples}"),
        ("表 1–3 分母", f"bucket0 已归属 {t.attributed_samples}"),
        ("窗口", _window_text(t)),
        ("产物", _artifact_text(t)),
        ("产物编号", m.artifact_num or "（无）"),
        ("落盘时刻", m.created_at),
    ]
    for key, value in kv:
        out.append(f"<tr><th>{html.escape(key)}</th><td>{html.escape(value)}</td></tr>")
    out.append("</table>")

    if t.notes:
        out.append('<ul class="notes">')
        for note in t.notes:
            out.append(f"<li>{html.escape(note)}</li>")
        out.append('<li>表 1–3 分母 = bucket0 已归属样本；表 4 分母 = 全样本</li>')
        if t.functions:
            out.append("<li>表 5 分母 = bucket0 未归属样本（公共/根代码），函数取最内层 DIE</li>")
        out.append("</ul>")
    if t.watermark:
        out.append(f'<div class="alert" id="watermark">G4 覆盖水位告警：'
                   f"{html.escape(t.watermark)}</div>")

    panels = [
        ("panel-types", "表 1 元件类型", _panel_types(t)),
        ("panel-instances", "表 2 最热实例", _panel_instances(t)),
        ("panel-lines", "表 3 行级明细", _panel_lines(t)),
        ("panel-buckets", "表 4 未归属分桶", _panel_buckets(t)),
    ]
    if t.functions:
        panels.append(("panel-functions", "表 5 函数热度", _panel_functions(t)))
    out.append('<nav class="tabs">')
    for i, (panel, label, _body) in enumerate(panels):
        cls = "tab active" if i == 0 else "tab"
        out.append(f'<button class="{cls}" data-panel="{panel}">{html.escape(label)}</button>')
    out.append("</nav>")
    for i, (panel, _label, body) in enumerate(panels):
        cls = "panel active" if i == 0 else "panel"
        out.append(f'<section id="{panel}" class="{cls}">')
        out.append(body)
        out.append("</section>")

    out.append('<footer>生成：hotspot/analysis/report.py · 单文件自包含（无外链、无 CDN）·'
               " analysis 只回写 run.toml 的 <code>buckets</code></footer>")
    out.append(f"<script>{JS}</script>")
    out.append("</body>")
    out.append("</html>")
    return "\n".join(out)


def _panel_types(t: Tables) -> str:
    out = [f'<p class="dim toolbar">全部类型（含零采样类型）· 分母 = bucket0 已归属 '
           f"{t.attributed_samples} 样本</p>",
           '<table class="grid" id="table-types">',
           "<thead><tr><th>类型</th><th class=\"num\">元件数</th>"
           '<th class="num">样本数</th><th class="num">占比</th></tr></thead>',
           "<tbody>"]
    for row in t.types:
        out.append(
            f'<tr class="row"><td class="mono">{html.escape(row.type)}</td>'
            f'<td class="num" data-v="{row.instances}">{row.instances}</td>'
            f'<td class="num" data-v="{row.samples}">{row.samples}</td>'
            f"{_share_cell(row.share)}</tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def _panel_instances(t: Tables) -> str:
    out = ['<p class="dim toolbar">按样本数排序 · 点一行展开该元件的行级热点 · 分母同上</p>',
           '<p class="toolbar"><input class="filter" data-table="table-instances"'
           ' placeholder="筛选元件 / 类型 / 行号"></p>',
           '<table class="grid" id="table-instances">',
           "<thead><tr><th>元件</th><th>源码行区间</th>"
           '<th class="num">样本数</th><th class="num">占比</th></tr></thead>',
           "<tbody>"]
    for index, row in enumerate(t.instances):
        detail_id = f"detail-inst-{index}"
        out.append(
            f'<tr class="row expandable" data-detail="{detail_id}">'
            f'<td class="mono">{html.escape(row.type)} #{row.id}</td>'
            f'<td class="mono">{html.escape(_lines_text(row.src_file, row.src_lines))}</td>'
            f'<td class="num" data-v="{row.samples}">{row.samples}</td>'
            f"{_share_cell(row.share)}</tr>")
        out.append(f'<tr class="detail hidden" id="{detail_id}" data-open="0">'
                   f'<td colspan="4">{_line_table(row, f"lines-inst-{index}")}</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


def _line_table(row: InstRow, table_id: str) -> str:
    if not row.line_rows:
        return '<p class="dim">该元件的机器段没有行号信息（侧车 lines 未覆盖）</p>'
    out = [f'<table class="grid" id="{table_id}">',
           "<thead><tr><th>源码行</th>"
           '<th class="num">样本数</th><th class="num">占该元件</th></tr></thead>',
           "<tbody>"]
    for line in row.line_rows:
        out.append(f'<tr class="row"><td class="mono" data-v="{line.line}">'
                   f"{html.escape(_line_label(line.file, line.line))}</td>"
                   f'<td class="num" data-v="{line.samples}">{line.samples}</td>'
                   f"{_share_cell(line.share)}</tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


def _panel_lines(t: Tables) -> str:
    out = ['<p class="dim toolbar">表 2 里每个元件的行级热点（占比的分母是该元件自己的样本数）</p>',
           '<p class="toolbar"><input class="filter" data-table="table-lines"'
           ' placeholder="筛选元件 / 行号"></p>',
           '<table class="grid" id="table-lines">',
           "<thead><tr><th>元件</th><th>源码行</th>"
           '<th class="num">样本数</th><th class="num">占该元件</th></tr></thead>',
           "<tbody>"]
    for row in t.instances:
        if row.line_rows:
            for line in row.line_rows:
                out.append(
                    f'<tr class="row"><td class="mono">{html.escape(row.type)} #{row.id}</td>'
                    f'<td class="mono" data-v="{line.line}">'
                    f"{html.escape(_line_label(line.file, line.line))}</td>"
                    f'<td class="num" data-v="{line.samples}">{line.samples}</td>'
                    f"{_share_cell(line.share)}</tr>")
        else:
            out.append(
                f'<tr class="row"><td class="mono">{html.escape(row.type)} #{row.id}</td>'
                f'<td class="dim">无行号信息</td>'
                f'<td class="num" data-v="{row.samples}">{row.samples}</td>'
                f'<td class="num">—</td></tr>')
    out.append("</tbody></table>")
    return "\n".join(out)


def _panel_buckets(t: Tables) -> str:
    out = [f'<p class="dim toolbar">分母 = 全样本 {t.total_samples}（含零值行）</p>',
           '<table class="grid" id="table-buckets">',
           "<thead><tr><th>分桶</th>"
           '<th class="num">样本数</th><th class="num">占比</th></tr></thead>',
           "<tbody>"]
    for bucket in t.buckets:
        out.append(_bucket_row(bucket, "bucket", 0))
        for name, samples, share in bucket.detail:
            out.append(_bucket_row(
                BucketRow(key=f"{bucket.key}:{name}", label=f"└ {name}",
                          samples=samples, share=share), "sub", 1))
    out.append("</tbody></table>")
    return "\n".join(out)


def _bucket_row(bucket: BucketRow, css: str, depth: int) -> str:
    indent = "&nbsp;&nbsp;" * depth
    return (f'<tr class="row {css}"><td>{indent}{html.escape(bucket.label)}</td>'
            f'<td class="num" data-v="{bucket.samples}">{bucket.samples}</td>'
            f"{_share_cell(bucket.share)}</tr>")


def _panel_functions(t: Tables) -> str:
    root = next(bucket.samples for bucket in t.buckets if bucket.key == "bucket0_root")
    out = [f'<p class="dim toolbar">公共/根代码 {root} 样本按函数拆 · 函数取最内层 DIE'
           "（元件机器段不进这张表，它们已经归到元件了）</p>",
           '<p class="toolbar"><input class="filter" data-table="table-functions"'
           ' placeholder="筛选函数 / 文件 / 行号"></p>',
           '<table class="grid" id="table-functions">',
           "<thead><tr><th>函数</th><th>声明位置</th>"
           '<th class="num">样本数</th><th class="num">占比</th></tr></thead>',
           "<tbody>"]
    for row in t.functions:
        out.append(
            f'<tr class="row"><td class="mono">{html.escape(row.name)}</td>'
            f'<td class="mono" data-v="{row.line or 0}">'
            f"{html.escape(_line_label(row.file, row.line))}</td>"
            f'<td class="num" data-v="{row.samples}">{row.samples}</td>'
            f"{_share_cell(row.share)}</tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


# ---------------------------------------------------------------- index.html


def render_index_html(entries: list[IndexEntry], workspace: str) -> str:
    out = ["<!DOCTYPE html>", '<html lang="zh-CN">', "<head>", '<meta charset="utf-8">',
           "<title>热点分析工作区索引</title>", f"<style>{CSS}</style>", "</head>", "<body>",
           "<h1>热点分析工作区索引</h1>",
           f'<p class="dim meta">{html.escape(workspace)}</p>',
           '<table class="grid" id="table-index">',
           "<thead><tr><th>run</th><th>时间</th><th>产物</th><th>频率</th>"
           '<th class="num">bucket0 份额</th><th>报告</th></tr></thead>',
           "<tbody>"]
    for entry in entries:
        share = "未分析" if entry.bucket0_share is None else _pct(entry.bucket0_share)
        value = "0" if entry.bucket0_share is None else f"{entry.bucket0_share:.6f}"
        link = (f'<a href="{html.escape(entry.report)}">report.html</a>'
                if entry.report else '<span class="dim">（还没跑 report.py）</span>')
        out.append(
            f'<tr class="row"><td class="mono">{html.escape(entry.run_name)}</td>'
            f'<td class="mono">{html.escape(entry.created_at)}</td>'
            f'<td class="mono">{html.escape(entry.artifact)}</td>'
            f'<td class="mono">{html.escape(entry.freq_note)}</td>'
            f'<td class="num" data-v="{value}">{share}</td>'
            f"<td>{link}</td></tr>")
    if not entries:
        out.append('<tr class="row"><td colspan="6" class="dim">workspace/runs 下还没有 run</td></tr>')
    out.append("</tbody></table>")
    out.append('<footer>生成：hotspot/analysis/report.py · 单文件自包含</footer>')
    out.append(f"<script>{JS}</script>")
    out.append("</body>")
    out.append("</html>")
    return "\n".join(out)
