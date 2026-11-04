#!python3
"""把 hotspot 的 TOML 配置与 run 记录迁成冻结的 INI 子集（config.toml → config.ini、run.toml → run.ini）。

    python hotspot/tools/migrate_toml_to_ini.py --dry-run
    python hotspot/tools/migrate_toml_to_ini.py

目标：`hotspot/config.toml` 与 `hotspot/workspace/runs/*/run.toml`。

格式（与 consumer 侧一致）：`[section]` + `key = value`，`;` / `#` 注释；每个值都是 JSON 编码
（`path = "x"`、`bases = ["0x1"]`、`tids = [1,2]`、`null`），数字也走 JSON 所以是裸写的。
run.toml 的三个顶层键（schema / created_at / backend）进 `[run]` section —— 纯 INI 不允许
section 之前的裸键。

每个文件按「tomllib 读 → 渲染 → 读回逐键比对 → 落盘 → 再从磁盘读回比对 → 备份 → 删旧文件」
走：比对不过就删掉新文件、留着旧文件并计一次失败，不留半迁移现场。备份在
`work/tmp/toml_backup/<相对路径>`。
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
import json
import shutil
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKUP_SUBDIR = "work/tmp/toml_backup"

# 迁移目标，以及顶层键在 INI 里归入哪个 section（纯 INI 不允许 section 之前的裸键）。
# config.toml 的键全在 section 里，CONFIG_ROOT_SECTION 只防万一。
CONFIG_TOML = "hotspot/config.toml"
RUNS_GLOB = "hotspot/workspace/runs/*/run.toml"
CONFIG_ROOT_SECTION = "config"
RUN_ROOT_SECTION = "run"

NOTE = "由 hotspot/tools/migrate_toml_to_ini.py 从 {source} 迁移（spec §10）"


class MigrationError(RuntimeError):
    """单个文件没迁成：带文件与原因，由 CLI 汇总成非零退出码。"""


def render_ini(doc: dict[str, object], *, root_section: str, note: str) -> str:
    """把 tomllib 读出的表渲染成 INI 子集。顶层标量进 root_section，子表各成一个 section。"""
    lines = [f"; {note}"]
    scalars = {key: value for key, value in doc.items() if not isinstance(value, dict)}
    tables = {key: value for key, value in doc.items() if isinstance(value, dict)}
    if scalars:
        lines += _section_lines(root_section, scalars)
    for name, table in tables.items():
        lines += _section_lines(name, table)
    return "\n".join(lines) + "\n"


def _section_lines(name: str, table: dict[str, object]) -> list[str]:
    lines = ["", f"[{name}]"]
    lines += [f"{key} = {json.dumps(value, ensure_ascii=False)}" for key, value in table.items()]
    return lines


def _parse_ini(text: str, where: str, root_section: str) -> dict[str, object]:
    # 键名由 configparser 折成小写；源文件与下游用的都是小写键，折了不影响比对。
    parser = configparser.ConfigParser(
        delimiters=("=",),
        interpolation=None,
        comment_prefixes=("#", ";"),
        inline_comment_prefixes=("#", ";"),
    )
    try:
        parser.read_string(text)
    except configparser.Error as exc:
        raise MigrationError(f"{where}: 不是合法 INI：{exc}") from exc

    doc: dict[str, object] = {}
    for section in parser.sections():
        table: dict[str, object] = {}
        for key, raw in parser.items(section, raw=True):
            try:
                table[key] = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise MigrationError(
                    f"{where}: [{section}] {key} 的值不是 JSON 编码（{raw!r}）：{exc.msg}"
                ) from exc
        if section == root_section:
            doc.update(table)
        else:
            doc[section] = table
    return doc


def read_ini(path: Path, root_section: str) -> dict[str, object]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise MigrationError(f"{path}: 读不了（{exc.strerror or exc}）") from exc
    return _parse_ini(text, str(path), root_section)


def _read_toml(path: Path) -> dict[str, object]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise MigrationError(f"{path}: 读不了（{exc.strerror or exc}）") from exc
    try:
        return tomllib.loads(text)
    except tomllib.TOMLDecodeError as exc:
        raise MigrationError(f"{path}: 不是合法 TOML：{exc}") from exc


def _backup_path(root: Path, source: Path) -> Path:
    """备份保留相对路径（不同 run 目录下的同名 run.toml 不会互相覆盖）；撞名就加序号。"""
    backup = root / BACKUP_SUBDIR / source.relative_to(root)
    serial = 2
    while backup.exists():
        backup = backup.with_name(f"{source.name}.{serial}")
        serial += 1
    return backup


def find_targets(root: Path) -> list[tuple[Path, str]]:
    targets: list[tuple[Path, str]] = []
    config = root / CONFIG_TOML
    if config.is_file():
        targets.append((config, CONFIG_ROOT_SECTION))
    targets += [(path, RUN_ROOT_SECTION) for path in sorted(root.glob(RUNS_GLOB))]
    return targets


def migrate(root: Path, source: Path, root_section: str, *, dry_run: bool) -> str:
    """迁一个文件，返回一行结果；失败抛 MigrationError（此时旧文件还在、新文件已清干净）。"""
    target = source.with_suffix(".ini")
    if target.exists():
        raise MigrationError(f"{target} 已存在，不覆盖（先确认哪一份是新的）")

    doc = _read_toml(source)
    text = render_ini(doc, root_section=root_section, note=NOTE.format(source=source.name))
    if _parse_ini(text, str(target), root_section) != doc:
        raise MigrationError(f"{source}: 渲染出的 INI 读回与源不一致，未写盘")
    backup = _backup_path(root, source)
    if dry_run:
        return f"[dry-run] {source} → {target}（备份到 {backup}，然后删旧文件）"

    try:
        target.write_text(text, encoding="utf-8")
    except OSError as exc:
        raise MigrationError(f"{target}: 写不了（{exc.strerror or exc}）") from exc
    # 从磁盘读回来的那份才是下游会看到的东西：写坏了就是写坏了，不能只信内存里的字符串。
    if read_ini(target, root_section) != doc:
        target.unlink(missing_ok=True)
        raise MigrationError(f"{target}: 落盘后读回与源不一致，已删掉新文件")

    backup.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(source, backup)
    except OSError as exc:
        target.unlink(missing_ok=True)
        raise MigrationError(f"{source}: 备份到 {backup} 失败（{exc.strerror or exc}），已删掉新文件") from exc
    try:
        source.unlink()
    except OSError as exc:
        raise MigrationError(f"{source}: 删不掉（{exc.strerror or exc}）；新文件已写成 {target}") from exc
    return f"{source} → {target}（旧文件已备份到 {backup} 并删除）"


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        # Windows 下走管道时默认 GBK，中文会变乱码；非 TextIOWrapper（测试替身）没有 reconfigure
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            with contextlib.suppress(ValueError):
                reconfigure(encoding="utf-8", errors="replace")

    parser = argparse.ArgumentParser(description="hotspot 的 TOML → INI 子集迁移")
    parser.add_argument("--dry-run", action="store_true", help="只打印计划，不写不删")
    parser.add_argument("--root", default=str(ROOT), help="仓库根（默认本脚本的上级两级）")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    targets = find_targets(root)
    if not targets:
        print("没有要迁移的文件")
        return 0

    failures = 0
    for source, root_section in targets:
        try:
            print(migrate(root, source, root_section, dry_run=args.dry_run))
        except MigrationError as exc:
            failures += 1
            print(f"失败：{exc}", file=sys.stderr)
    print(f"共 {len(targets)} 个文件，{failures} 个失败" + ("（dry-run，未动磁盘）" if args.dry_run else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
