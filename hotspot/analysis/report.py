"""热点报表 CLI：run 目录 → report.md + report.html，回写 buckets，维护 baseline/index，跑 G3′。

    python hotspot/analysis/report.py --run hotspot/workspace/runs/<run>
    python hotspot/analysis/report.py --compare <runA> <runB>

退出码：0 正常；1 G3′ 判负（重合不足）；2 输入问题（缺文件 / 坏 schema / 坐标越界）。
对同一 run 幂等：除 run.ini 的 `[buckets]` 与 workspace/baseline.json（首跑建）外不改别的文件。
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
import datetime
import json
import shutil
import sys
from pathlib import Path

import attribution
import render
import tables

RUN_SCHEMA = 1
BUCKET_PRECISION = 6

_MISSING = object()


# ------------------------------------------------------------ run.ini 读取
#
# run.ini 是本项目冻结的 INI 子集：`[section]` + `key = value`，`;` / `#` 注释，
# 每个值都是 JSON 编码（`path = "x"`、`bases = ["0x1"]`、`tids = [1,2]`、`null`）。
# 所以值一律过一遍 json.loads，不在这里按类型二次解析 —— 下游拿到的就是 str / 数字 / 列表。


def _lookup(doc: dict, dotted: str):
    cur: object = doc
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return _MISSING
        cur = cur[part]
    return cur


def _number(doc: dict, dotted: str, path: str, required: bool) -> float | None:
    value = _lookup(doc, dotted)
    if value is _MISSING or value is None:
        if required:
            raise attribution.AnalysisError(path, f"缺少 run.ini 键 {dotted}（consumer save 应初写）")
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise attribution.AnalysisError(path, f"run.ini 键 {dotted} 必须是数字（实测 {value!r}）")
    return value


def _integer(doc: dict, dotted: str, path: str) -> int | None:
    value = _lookup(doc, dotted)
    if value is _MISSING or value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise attribution.AnalysisError(path, f"run.ini 键 {dotted} 必须是整数（实测 {value!r}）")
    return value


def _text(doc: dict, dotted: str, path: str, required: bool) -> str:
    value = _lookup(doc, dotted)
    if value is _MISSING or value is None:
        if required:
            raise attribution.AnalysisError(path, f"缺少 run.ini 键 {dotted}（consumer save 应初写）")
        return ""
    if not isinstance(value, str):
        raise attribution.AnalysisError(path, f"run.ini 键 {dotted} 必须是字符串（实测 {value!r}）")
    return value


def _parse_ini(text: str, path: str) -> dict[str, dict[str, object]]:
    # 键名由 configparser 折成小写，查表的键也全是小写 —— 大小写差异不会变成「键找不到」。
    parser = configparser.ConfigParser(
        delimiters=("=",),
        interpolation=None,
        comment_prefixes=("#", ";"),
        inline_comment_prefixes=("#", ";"),
    )
    try:
        parser.read_string(text)
    except configparser.Error as exc:
        raise attribution.AnalysisError(path, f"不是合法 INI：{exc}") from exc

    doc: dict[str, dict[str, object]] = {}
    for section in parser.sections():
        values: dict[str, object] = {}
        for key, raw in parser.items(section, raw=True):
            try:
                values[key] = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise attribution.AnalysisError(
                    path, f"[{section}] {key} 的值不是 JSON 编码（实测 {raw!r}）：{exc.msg}") from exc
        doc[section] = values
    return doc


def _read_ini(path: Path) -> dict[str, dict[str, object]]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise attribution.AnalysisError(str(path), f"读不了（{exc.strerror or exc}）") from exc
    return _parse_ini(text, str(path))


def find_sidecar(run_dir: Path, artifact_path: str, explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.exists():
            raise attribution.AnalysisError(str(path), "--sidecar 指向的文件不存在")
        return path
    candidates = [run_dir / "sidecar.json"]
    if artifact_path:
        blob = Path(artifact_path)
        candidates.append(blob.with_suffix(".sidecar.json"))
        candidates.append(blob.parent / f"{blob.name}.sidecar.json")
    for path in candidates:
        if path.exists():
            return path
    tried = "、".join(str(c) for c in candidates)
    raise attribution.AnalysisError(
        str(run_dir), f"找不到侧车（试过 {tried}）；用 --sidecar 指定，或把 sidecar.json 拷进 run 目录")


def load_run(run_dir: Path, sidecar_path: str | None = None) -> tuple[tables.RunMeta, attribution.Samples, attribution.Sidecar]:
    run_ini = run_dir / "run.ini"
    if not run_ini.exists():
        raise attribution.AnalysisError(str(run_dir), "不是 run 目录：缺 run.ini")
    path = str(run_ini)
    doc = _read_ini(run_ini)
    schema = _integer(doc, "run.schema", path)
    if schema is not None and schema != RUN_SCHEMA:
        raise attribution.AnalysisError(path, f"schema={schema}，本工具只认 {RUN_SCHEMA}")

    file_len = _integer(doc, "artifact.file_len", path)
    code_len = _integer(doc, "artifact.code_len", path)
    if code_len is None and file_len is not None:
        code_len = file_len - 8  # 产物文件前 8 字节是长度头（spec §2）

    artifact_path = _text(doc, "artifact.path", path, required=False)
    sidecar_file = find_sidecar(run_dir, artifact_path, sidecar_path)
    sidecar = attribution.load_sidecar(str(sidecar_file), code_len=code_len)
    samples = attribution.load_samples(str(run_dir / "samples.tsv"))

    bases_raw = _lookup(doc, "locate.bases")
    bases = tuple(str(b) for b in bases_raw) if isinstance(bases_raw, list) else ()
    sample_count = _integer(doc, "window.sample_count", path)

    meta = tables.RunMeta(
        run_name=run_dir.name,
        run_path=str(run_dir),
        created_at=_text(doc, "run.created_at", path, required=False) or "（未记录）",
        artifact_path=artifact_path or "（未记录）",
        artifact_sha256=_text(doc, "artifact.sha256", path, required=False) or "（未记录）",
        artifact_file_len=file_len,
        artifact_code_len=code_len,
        artifact_num=_text(doc, "artifact.num", path, required=False),
        window_sec=_number(doc, "window.sec", path, required=False),
        interval_ms=_number(doc, "window.interval_ms", path, required=False),
        sample_count=sample_count if sample_count is not None else 0,
        pause_avg_us=_integer(doc, "window.avg_pause_us", path),
        pause_max_us=_integer(doc, "window.max_pause_us", path),
        pause_total_us=_integer(doc, "window.total_pause_us", path),
        bases=bases,
        freq_start_hz=_number(doc, "freq.start_hz", path, required=False),
        freq_end_hz=_number(doc, "freq.end_hz", path, required=False),
    )
    return meta, samples, sidecar


# ---------------------------------------------------------------- buckets


def bucket_map(att: attribution.Attribution) -> dict[str, float]:
    total = att.total
    shares = {b: (att.bucket_totals.get(b, 0) / total if total else 0.0) for b in attribution.BUCKETS}
    return {
        "total": total,
        "bucket0": att.bucket_totals.get(0, 0),
        "bucket1": att.bucket_totals.get(1, 0),
        "bucket2": att.bucket_totals.get(2, 0),
        "bucket0_share": round(shares[0], BUCKET_PRECISION),
        "bucket1_share": round(shares[1], BUCKET_PRECISION),
        "bucket2_share": round(shares[2], BUCKET_PRECISION),
        "bucket0_attributed": att.attributed,
        "bucket0_stub": att.stub_samples,
        "bucket0_root": att.root_samples,
    }


def _buckets_section(buckets: dict[str, float]) -> list[str]:
    lines = ["[buckets]"]
    for key in sorted(buckets):
        value = buckets[key]
        if isinstance(value, int):
            lines.append(f"{key} = {value}")
        else:
            lines.append(f"{key} = {value:.6f}")
    return lines


def write_buckets(run_ini: Path, buckets: dict[str, float]) -> None:
    """只重写 `[buckets]` 整张表，其余 section 与键逐字保留（spec §10：analysis 只回写 buckets）。"""
    text = run_ini.read_text(encoding="utf-8-sig")
    lines = text.splitlines()
    section = _buckets_section(buckets)
    start = next((i for i, line in enumerate(lines) if line.strip() == "[buckets]"), None)
    if start is None:
        while lines and not lines[-1].strip():
            lines.pop()
        candidate = "\n".join(lines + [""] + section) + "\n"
    else:
        end = next((j for j in range(start + 1, len(lines))
                    if lines[j].lstrip().startswith("[")), len(lines))
        candidate = "\n".join(lines[:start] + section + lines[end:]) + "\n"

    path = str(run_ini)
    expected = _parse_ini(text, path)
    expected["buckets"] = _parse_ini("\n".join(section), path)["buckets"]
    actual = _parse_ini(candidate, path)
    if actual != expected:
        raise attribution.AnalysisError(
            path, "回写 buckets 后自校验失败（buckets 以外的键被改动了，请检查 run.ini 格式）")
    run_ini.write_text(candidate, encoding="utf-8")


# ---------------------------------------------------------------- baseline


def update_baseline(workspace: Path, run_name: str, buckets: dict[str, float],
                    ratio: float, now: str) -> str | None:
    """G4 覆盖水位：首跑建基线，之后 bucket0 份额 < 基线 × ratio 就告警。"""
    path = workspace / "baseline.json"
    share = buckets["bucket0_share"]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        doc = {
            "version": 1,
            "warn_ratio": ratio,
            "baseline": {"bucket0_share": share, "run": run_name, "created_at": now},
        }
        path.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return None
    try:
        doc = json.loads(path.read_text(encoding="utf-8-sig"))
    except OSError as exc:
        raise attribution.AnalysisError(str(path), f"读不了（{exc.strerror or exc}）") from exc
    except json.JSONDecodeError as exc:
        raise attribution.AnalysisError(str(path), f"不是合法 JSON（第 {exc.lineno} 行：{exc.msg}）") from exc
    base = doc.get("baseline") if isinstance(doc, dict) else None
    if not isinstance(base, dict):
        raise attribution.AnalysisError(str(path), "baseline 缺失或不是对象")
    base_share = base.get("bucket0_share")
    if not isinstance(base_share, (int, float)) or isinstance(base_share, bool):
        raise attribution.AnalysisError(str(path), "baseline.bucket0_share 缺失或不是数字")
    threshold = doc.get("warn_ratio", ratio)
    if not isinstance(threshold, (int, float)) or isinstance(threshold, bool):
        raise attribution.AnalysisError(str(path), "warn_ratio 不是数字")
    floor = base_share * threshold
    if share < floor:
        return (f"bucket0 份额 {100 * share:.1f}% 低于基线 {100 * base_share:.1f}% × "
                f"{threshold:.2f} = {100 * floor:.1f}%（基线来自 run {base.get('run')}）"
                f"：覆盖水位下降 —— 换电路了？采样漏了？还是 etw 途中新开线程丢了样本？")
    return None


# ---------------------------------------------------------------- index


def update_index(workspace: Path) -> tuple[Path, int]:
    entries: list[render.IndexEntry] = []
    runs_dir = workspace / "runs"
    for run_ini in sorted(runs_dir.glob("*/run.ini")):
        try:
            doc = _read_ini(run_ini)
        except attribution.AnalysisError as exc:
            print(f"跳过 {run_ini.parent.name}：{exc}", file=sys.stderr)
            continue
        buckets_raw = doc.get("buckets")
        share_raw = buckets_raw.get("bucket0_share") if isinstance(buckets_raw, dict) else None
        share = (float(share_raw) if isinstance(share_raw, (int, float))
                 and not isinstance(share_raw, bool) else None)
        start = _lookup(doc, "freq.start_hz")
        end = _lookup(doc, "freq.end_hz")
        freqs = [f"{v / 1e6:.3f} MHz" for v in (start, end)
                 if isinstance(v, (int, float)) and not isinstance(v, bool) and v]
        run_name = run_ini.parent.name
        html_dir = runs_dir / "html"
        central = html_dir / f"{run_name}.html"
        legacy = run_ini.parent / "report.html"
        if not central.exists() and legacy.exists():
            html_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(legacy, central)
        entries.append(render.IndexEntry(
            run_name=run_name,
            created_at=str(_lookup(doc, "run.created_at") if _lookup(doc, "run.created_at") is not _MISSING
                            else "（未记录）"),
            artifact=Path(str(_lookup(doc, "artifact.path") if _lookup(doc, "artifact.path") is not _MISSING
                               else "（未记录）")).name,
            freq_note=" / ".join(freqs) if freqs else "（无）",
            bucket0_share=share,
            report=(f"runs/html/{run_name}.html" if central.exists()
                    else f"runs/{run_name}/report.html" if legacy.exists() else ""),
        ))
    entries.sort(key=lambda e: (e.created_at, e.run_name), reverse=True)
    path = workspace / "index.html"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(render.render_index_html(entries, str(workspace)), encoding="utf-8")
    return path, len(entries)


# ---------------------------------------------------------------- G3′


def top_instances(att: attribution.Attribution, top: int) -> list[tuple[str, int]]:
    keys = sorted(att.per_instance, key=lambda k: (-att.per_instance[k], k[0], k[1]))
    return keys[:top]


def compare_runs(run_a: Path, run_b: Path, top: int, min_overlap: int) -> tuple[bool, list[str]]:
    lines: list[str] = []
    tops = []
    for run_dir in (run_a, run_b):
        meta, samples, sidecar = load_run(run_dir)
        att = attribution.attribute(samples, sidecar, meta.artifact_code_len)
        if att.attributed == 0:
            raise attribution.AnalysisError(str(run_dir), "没有已归属样本，G3′ 无法比较（电路没在跑？）")
        tops.append((meta, att, top_instances(att, top)))
    (meta_a, _, keys_a), (meta_b, _, keys_b) = tops
    overlap = len(set(keys_a) & set(keys_b))
    need = min(min_overlap, len(keys_a), len(keys_b))

    lines.append(f"G3′ 重复性：top-{top} 按 (type,id) 集合重合，判据 ≥{min_overlap}")
    for meta, _, keys in tops:
        lines.append(f"  {meta.run_name}（top-{top} 实际 {len(keys)} 项）：")
        for rank, (etype, eid) in enumerate(keys, start=1):
            lines.append(f"    {rank}. {etype} #{eid}")
    lines.append(f"  重合 {overlap} 项 / 比较 {min(len(keys_a), len(keys_b))} 项（阈值 {need}）")
    if need < min_overlap:
        lines.append(f"  注意：可用实例不足 {min_overlap} 个，阈值按实际实例数降到 {need}")
    verdict = overlap >= need
    lines.append(f"  verdict: {'PASS' if verdict else 'FAIL'}"
                 f"（{'重合足够' if verdict else f'重合不足（{overlap} < {need}）'}）")
    return verdict, lines


# ---------------------------------------------------------------- CLI


def run_report(args: argparse.Namespace) -> int:
    run_dir = Path(args.run).resolve()
    meta, samples, sidecar = load_run(run_dir, args.sidecar)
    att = attribution.attribute(samples, sidecar, meta.artifact_code_len)
    buckets = bucket_map(att)
    now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    workspace = Path(args.workspace).resolve() if args.workspace else _default_workspace(run_dir)
    watermark = update_baseline(workspace, meta.run_name, buckets, args.baseline_ratio, now)
    table_set = tables.build_tables(meta, att, sidecar, args.top, watermark)

    (run_dir / "report.md").write_text(render.render_markdown(table_set), encoding="utf-8")
    html_text = render.render_html(table_set)
    (run_dir / "report.html").write_text(html_text, encoding="utf-8")
    html_dir = workspace / "runs" / "html"
    html_dir.mkdir(parents=True, exist_ok=True)
    central_html = html_dir / f"{meta.run_name}.html"
    central_html.write_text(html_text, encoding="utf-8")
    write_buckets(run_dir / "run.ini", buckets)
    index_path, run_count = update_index(workspace)

    attributed_share = 100 * att.attributed / att.total if att.total else 0.0
    print(f"{meta.run_name}: 总样本 {att.total}，bucket0 份额 {100 * buckets['bucket0_share']:.1f}%"
          f"（已归属 {att.attributed}，占全样本 {attributed_share:.1f}%）")
    print(f"  类型 {len(table_set.types)} 种（含零值）/ 最热实例 {len(table_set.instances)} 个"
          f" / 未归属分桶 {len(table_set.buckets)} 行")
    if table_set.functions:
        print(f"  函数热度 {len(table_set.functions)} 行（bucket0 全部样本按最内层函数归因）")
    print(f"  buckets 已回写 {run_dir / 'run.ini'}")
    print(f"  报告 {run_dir / 'report.md'} + {central_html}")
    print(f"  index {index_path}（{run_count} 个 run）")
    if watermark:
        print(f"  G4 告警：{watermark}")
    else:
        print("  G4 覆盖水位：通过")
    return 0


def _default_workspace(run_dir: Path) -> Path:
    """run 目录按 spec 落在 <workspace>/runs/<name>/；不在这层时退到上一级。"""
    parent = run_dir.parent
    return parent.parent if parent.name == "runs" else parent


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        # Windows 下走管道时默认 GBK，中文会变乱码；非 TextIOWrapper（测试替身）没有 reconfigure
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            with contextlib.suppress(ValueError):
                reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(
        description="热点报表：run 目录 → report.md + report.html（+ G3′ 对比）",
        epilog="退出码：0 正常；1 G3′ 判负；2 输入问题（缺文件 / 坏 schema / 坐标越界）")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--run", help="run 目录（含 run.ini / samples.tsv / sidecar.json）")
    mode.add_argument("--compare", nargs=2, metavar=("RUN_A", "RUN_B"), help="G3′：两个 run 的 top-N 重合")
    parser.add_argument("--sidecar", help="侧车路径（默认 run 目录下 sidecar.json，或产物旁的 .sidecar.json）")
    parser.add_argument("--workspace", help="workspace 根（默认 run 目录的上一级；在 runs/ 下则再上一级）")
    parser.add_argument("--top", type=int, help="表 2 实例数（默认 10）/ G3′ 的 N（默认 5）")
    parser.add_argument("--min-overlap", type=int, default=4, help="G3′ 判定阈值 K（默认 4）")
    parser.add_argument("--baseline-ratio", type=float, default=0.8,
                        help="G4 告警线：bucket0 份额低于基线的这个比例就告警（默认 0.8）")
    args = parser.parse_args(argv)

    if args.compare:
        args.top = 5 if args.top is None else args.top
    else:
        args.top = 10 if args.top is None else args.top
    try:
        if args.compare:
            verdict, lines = compare_runs(Path(args.compare[0]).resolve(),
                                          Path(args.compare[1]).resolve(),
                                          args.top, args.min_overlap)
            print("\n".join(lines))
            return 0 if verdict else 1
        return run_report(args)
    except attribution.AnalysisError as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
