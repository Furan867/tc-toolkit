"""一条命令跑完热点分析：编译 → sidecar → 采样 → 报表。

只做编排：每个阶段调用现成的可执行文件/脚本，不复制它们的逻辑。

不给 `--src` 就用 `work/hook` 里最近一次编译 hook 的源；仿真没在跑时自己点运行，
只在「是我们启动的」情况下才在结束时停（原本在跑的电路不去动）。
`--artifact` + `--sidecar` 能拿现成产物跳过编译，拿它做同一份产物的重采/重算。
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
SIMPLEXC = ROOT / "simplexc/build/simplexc_cli.exe"
CONSUMER = ROOT / "hotspot/consumer/build/hotspot-consumer.exe"
MAKE_SIDECAR = ROOT / "hotspot/tools/make_sidecar.py"
REPORT = ROOT / "hotspot/analysis/report.py"
READ_FREQ = ROOT / "tools/read_freq.py"
SIMCTL = ROOT / "tools/simctl.py"
DEFAULT_CONFIG = ROOT / "hotspot/config.toml"
SIDECAR_CACHE = ROOT / "hotspot/workspace/cache/sidecar"
WORK_TMP = ROOT / "work/tmp/hotspot_run"
RUNS_DIR = ROOT / "hotspot/workspace/runs"

SIM_SPEED = re.compile(r"sim_speed\s+([0-9]+(?:\.[0-9]+)?)")
GAME_PID = re.compile(r"\bPID\s+(\d+)")
WINDOW_STATS = re.compile(
    r"样本\s+(\d+).*?bucket0\s+(\d+).*?bucket1\s+(\d+).*?bucket2\s+(\d+)",
    re.S,
)


class StageError(RuntimeError):
    """某个阶段失败；带上阶段名、命令和输出尾部，便于定位。"""

    def __init__(
        self,
        stage: str,
        message: str,
        command: list[str] | None = None,
        output: str | None = None,
    ) -> None:
        super().__init__(message)
        self.stage = stage
        self.command = command
        self.output = output


def _load_tc_common() -> ModuleType:
    """按路径加载 `tools/tc_common.py`。

    `tools/` 不是包（没有 `__init__.py`），别的脚本靠往 `sys.path` 插目录来 `import
    tc_common`；这里按路径加载，不动全局导入路径（hotspot/tools 下也有同名目录，插进去
    会把两个 `tools` 命名空间包混在一起）。

    加载前必须登记进 `sys.modules`：`@dataclass` 处理字段时按 `cls.__module__` 反查
    模块字典，查不到就 `AttributeError`。
    """
    path = TOOLS / "tc_common.py"
    spec = importlib.util.spec_from_file_location("tc_common", path)
    if spec is None or spec.loader is None:
        raise StageError("preflight", f"加载不了 {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tc_common = _load_tc_common()


def _resolve(value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else ROOT / path


def _run(
    command: list[str | Path],
    *,
    stage: str,
    input_text: str | None = None,
    timeout: float = 120,
) -> str:
    parts = [str(part) for part in command]
    try:
        result = subprocess.run(
            parts,
            cwd=ROOT,
            input=input_text,
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise StageError(stage, f"超时（{timeout:g} s）", parts) from exc
    except OSError as exc:
        raise StageError(stage, f"启动失败：{exc}", parts) from exc

    if result.returncode != 0:
        tail = (result.stderr or result.stdout or "").strip().splitlines()[-12:]
        raise StageError(stage, f"退出码 {result.returncode}", parts, "\n".join(tail))
    return result.stdout


_SIDECAR_MODULE: ModuleType | None = None


def _sidecar_module() -> ModuleType:
    """按路径加载 `hotspot/tools/sidecar.py` —— 缓存键里的 functions_version 要跟着它走。

    把这个数值抄一份进 run.py 就会静默漂移：schema 改了、键没改，旧缓存继续命中。
    模块名不用 "sidecar"：那跟 `python hotspot/tools/make_sidecar.py` 的导入名撞车。
    """
    global _SIDECAR_MODULE
    if _SIDECAR_MODULE is None:
        path = ROOT / "hotspot/tools/sidecar.py"
        spec = importlib.util.spec_from_file_location("hotspot_sidecar", path)
        if spec is None or spec.loader is None:
            raise StageError("sidecar", f"加载不了 {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _SIDECAR_MODULE = module
    return _SIDECAR_MODULE


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sidecar_artifact_sha256(path: Path) -> str | None:
    """sidecar.json 里记的产物指纹；读不到（缺文件 / 坏 JSON / 缺字段）返回 None。"""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    artifact = document.get("artifact")
    sha = artifact.get("sha256") if isinstance(artifact, dict) else None
    return str(sha).lower() if sha else None


def _offsets_line_offset(path: Path) -> int:
    """offsets 的 `line_offset`（stdlib 前缀行数）—— sidecar 的行号归属取决于它。

    旧 offsets 没有这个键（W1 加之前产的），按 0 算：那时 sidecar 本来就没有文件绑定。
    """
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise StageError("sidecar", f"读不了 offsets {path}：{exc}") from exc
    value = document.get("line_offset", 0)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise StageError("sidecar", f"offsets {path} 的 line_offset={value!r} 不是非负整数")
    return value


def read_sim_state() -> tuple[int | None, float]:
    """一次读数：游戏 PID 与当前仿真主频（MHz）。

    游戏没开时 read_freq 退出码非 0（消息在 stderr），这里折算成「没有 PID」而不是
    阶段失败 —— 调用方要分开「游戏没开」和「游戏开着但仿真停着」。
    """
    try:
        output = _run(
            [sys.executable, READ_FREQ, "--samples", "1"],
            stage="preflight",
            timeout=30,
        )
    except StageError:
        return None, 0.0

    pid = GAME_PID.search(output)
    speed = SIM_SPEED.search(output)
    return (
        int(pid.group(1)) if pid else None,
        float(speed.group(1)) if speed else 0.0,
    )


def _game_running() -> bool:
    """仿真在跑（主频 > 0）；游戏没开也是 False。"""
    return read_sim_state()[1] > 0


def _start_sim() -> None:
    _run([sys.executable, SIMCTL, "run"], stage="preflight", timeout=60)


def _wait_sim_running(timeout: float = 15.0) -> None:
    """轮询主频 > 0。F6 是切换键，点错方向会变成暂停，所以必须验主频而不是验发过键。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _game_running():
            return
        time.sleep(1.0)
    raise StageError(
        "preflight",
        f"注入运行后 {timeout:g} s 内主频仍是 0（F6 是切换键，可能被点成暂停了）",
    )


def ensure_sim_running(opts: argparse.Namespace) -> None:
    """保证仿真在跑：主频 > 0 就不碰，停着就自己启动并记下「是我们启动的」。"""
    pid, speed = read_sim_state()
    if pid is None:
        raise StageError(
            "preflight",
            f"游戏没在跑（{READ_FREQ.name} 读不到 PID）；先 python tools/simctl.py run",
        )
    if speed > 0:
        print(f"→ 仿真已在跑（PID {pid}，{speed:g} MHz），不启动也不停")
        return
    if opts.no_sim_control:
        raise StageError(
            "preflight",
            "仿真没在跑，又给了 --no-sim-control（不自动启停）；先在游戏里点运行",
        )

    _start_sim()
    opts.started_by_us = True
    _wait_sim_running()
    print(f"→ 已启动仿真（PID {pid}），结束后停掉")


def _pick_hook_src(opts: argparse.Namespace) -> Path:
    """无参模式：最近一次编译 hook 落下的源（编号记进 opts，run 目录名要用）。"""
    product = tc_common.latest_hook_product()
    if product is None:
        raise StageError(
            "preflight",
            "work/hook 里没有可用的 hook 产物：要 impl=ours 的 "
            "hook_out_*.bin + hook_src_*.si，或者直接给 --src",
        )
    opts.hook_num = product.num
    if product.stdlib:
        opts.stdlib = ROOT / "work/hook/all/stdlib_prefix.si"
    print(f"→ 用 hook 产物 {product.num}：{product.src}")
    return product.src


def _check_artifact_pair(opts: argparse.Namespace) -> None:
    """现成产物模式：两份文件都在，且 sidecar 确实是这份产物的。"""
    for label, value in (("产物", opts.artifact), ("侧车", opts.sidecar)):
        if not _resolve(value).is_file():
            raise StageError("preflight", f"{label}文件不存在：{_resolve(value)}")

    artifact = _resolve(opts.artifact)
    sidecar = _resolve(opts.sidecar)
    want = _sha256_file(artifact)
    got = _sidecar_artifact_sha256(sidecar)
    if got is None:
        raise StageError("preflight", f"侧车里读不到 artifact.sha256：{sidecar}")
    if got != want:
        raise StageError(
            "preflight",
            f"侧车与产物不配对：sidecar artifact.sha256={got}，产物 sha256={want}",
        )


def preflight(opts: argparse.Namespace) -> tuple[Path, Path]:
    """返回 (config, 这份分析针对的文件)：编译模式是源文件，`--artifact` 模式是产物。"""
    config = _resolve(opts.config)
    if not config.is_file():
        raise StageError(
            "preflight",
            f'找不到 {config}；复制 hotspot/config.example.toml 并把 sampler.backend 改成 "suspend"',
        )

    for name, path in (("simplexc_cli", SIMPLEXC), ("hotspot-consumer", CONSUMER)):
        if not path.is_file():
            raise StageError("preflight", f"缺少 {name}：{path}，先构建")

    if (opts.artifact is None) != (opts.sidecar is None):
        raise StageError(
            "preflight",
            "--artifact 与 --sidecar 必须成对给出：只给一个就无法确认 sidecar 是这份产物的",
        )

    if opts.artifact is not None:
        _check_artifact_pair(opts)
        target = _resolve(opts.artifact)
    else:
        target = _resolve(opts.src) if opts.src else _pick_hook_src(opts)
        if not target.is_file():
            raise StageError("preflight", f"源文件不存在：{target}")
        if opts.stdlib and not _resolve(opts.stdlib).is_file():
            raise StageError("preflight", f"stdlib 前缀不存在：{_resolve(opts.stdlib)}")

    if not opts.skip_game_check:
        ensure_sim_running(opts)
    return config, target


def compile_stage(
    opts: argparse.Namespace,
    work_dir: Path,
    src: Path,
) -> dict[str, Path]:
    work_dir.mkdir(parents=True, exist_ok=True)
    blob = work_dir / "blob.bin"
    obj = work_dir / "module.obj"
    elements = work_dir / "el.tsv"
    offsets = work_dir / "offsets.json"

    command: list[str | Path] = [SIMPLEXC, "--src", src]
    if opts.stdlib:
        command += ["--stdlib", _resolve(opts.stdlib)]
    command += [
        "--out",
        blob,
        "--obj-out",
        obj,
        "--debug-info",
        "--elements-out",
        elements,
        "--offsets-out",
        offsets,
    ]
    _run(command, stage="compile", timeout=240)

    paths = {
        "blob": blob,
        "obj": obj,
        "elements": elements,
        "offsets": offsets,
        "stubs": Path(f"{blob}.stubs"),
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise StageError("compile", "产物缺失：" + "、".join(missing))
    return paths


def sidecar_stage(paths: dict[str, Path], cache_dir: Path = SIDECAR_CACHE) -> Path:
    """生成 sidecar.json；同一份产物（blob sha256 相同）直接用缓存，省掉 objdump 那趟。

    缓存键要带 offsets 的 `line_offset`：行号绑定来源文件之前产的 sidecar 里是拼接后行号，
    只按 blob sha 命中会把源码行标成 stdlib 的（产物字节没变，行号口径变了）。
    键里还要带 `functions_version`：没有函数表的旧 sidecar 同样不能命中 —— 它看上去完整，
    只是少了整张函数表与 func_id。
    """
    sidecar = paths["blob"].parent / "sidecar.json"
    blob_sha = _sha256_file(paths["blob"])
    key = (f"{blob_sha}.lo{_offsets_line_offset(paths['offsets'])}"
           f".fv{_sidecar_module().FUNCTIONS_VERSION}")
    cached = cache_dir / key / "sidecar.json"
    if cached.is_file() and _sidecar_artifact_sha256(cached) == blob_sha:
        shutil.copy2(cached, sidecar)
        return sidecar

    _run(
        [
            sys.executable,
            MAKE_SIDECAR,
            "--blob",
            paths["blob"],
            "--obj",
            paths["obj"],
            "--elements",
            paths["elements"],
            "--offsets",
            paths["offsets"],
            "--stubs",
            paths["stubs"],
            "--out",
            sidecar,
        ],
        stage="sidecar",
        timeout=120,
    )
    if not sidecar.is_file():
        raise StageError("sidecar", f"没有生成 {sidecar}")

    try:
        cached.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(sidecar, cached)
    except OSError as exc:
        print(f"⚠ sidecar 缓存没写成：{exc}", file=sys.stderr)
    return sidecar


def sample_stage(
    opts: argparse.Namespace,
    config: Path,
    blob: Path,
    run_dir: Path,
) -> dict[str, int]:
    for label, path in (("blob", blob), ("run 目录", run_dir)):
        if " " in str(path):
            raise StageError("sample", f"{label} 路径含空格，REPL 不支持：{path}")

    if run_dir.exists():
        raise StageError("sample", f"run 目录已存在，拒绝覆盖：{run_dir}")
    window = f"window {opts.window}"
    open_command = f"open {blob}"
    if opts.pid:
        open_command += f" --pid {opts.pid}"
    if opts.interval_ms:
        open_command += f" --interval-ms {opts.interval_ms}"

    script = "\n".join([open_command, window, f"save {run_dir}", "quit"]) + "\n"
    output = _run(
        [CONSUMER, "--config", config],
        stage="sample",
        input_text=script,
        timeout=opts.window + 90,
    )

    for name in ("run.toml", "samples.tsv"):
        if not (run_dir / name).is_file():
            raise StageError("sample", f"save 没有写出 {name}", output=output[-800:])

    match = WINDOW_STATS.search(output)
    if not match:
        raise StageError("sample", "没解析到窗口统计", output=output[-800:])
    samples, bucket0, bucket1, bucket2 = (int(value) for value in match.groups())
    if bucket0 == 0:
        raise StageError(
            "sample",
            "bucket0 为 0：采样没落在产物上，检查产物与游戏加载的是否同一份",
            output=output[-800:],
        )
    return {
        "samples": samples,
        "bucket0": bucket0,
        "bucket1": bucket1,
        "bucket2": bucket2,
    }


def report_stage(run_dir: Path, sidecar: Path) -> Path:
    shutil.copy2(sidecar, run_dir / "sidecar.json")
    output = _run(
        [sys.executable, REPORT, "--run", run_dir],
        stage="report",
        timeout=120,
    )
    html = run_dir / "report.html"
    if not html.is_file():
        raise StageError("report", "没有生成 report.html", output=output[-800:])
    return html


def _tree_stats(path: Path) -> tuple[int, int, int]:
    """(目录数, 文件数, 字节数)；目录数把 path 自己算进去 —— rmtree 会连它一起删。"""
    dirs = files = size = 0
    for child in [path, *path.rglob("*")]:
        try:
            if child.is_dir():
                dirs += 1
                continue
            files += 1
            size += child.stat().st_size
        except OSError:
            continue
    return dirs, files, size


def _clean_targets(opts: argparse.Namespace) -> list[tuple[str, Path]]:
    targets = [("sidecar 缓存", SIDECAR_CACHE), ("run 临时目录", WORK_TMP)]
    if opts.runs:
        targets.append(("run 目录", RUNS_DIR))
    return targets


def clean_command(opts: argparse.Namespace) -> int:
    """`clean` 子命令：删 sidecar 缓存与 work/tmp/hotspot_run。

    runs 里是报表，删了只能重新采样才能再造，所以默认不碰、只跟 `--runs` 走。
    work/hook 里是输入（hook 产物），永远不在清理范围。
    """
    targets = _clean_targets(opts)
    for label, path in targets:
        if not path.exists():
            print(f"→ 没有缓存：{label} {path}")
    plan = [
        (label, path, _tree_stats(path)) for label, path in targets if path.exists()
    ]
    if not plan:
        return 0

    dirs = sum(stats[0] for _, _, stats in plan)
    files = sum(stats[1] for _, _, stats in plan)
    size = sum(stats[2] for _, _, stats in plan)
    if opts.dry_run:
        for label, path, (one_dirs, one_files, one_size) in plan:
            print(
                f"[dry-run] 将删 {label} {path}："
                f"{one_dirs} 个目录 / {one_files} 个文件 / {one_size} 字节"
            )
        print(f"[dry-run] 合计 {dirs} 个目录 / {files} 个文件 / {size} 字节，未删")
        return 0

    failed = False
    for label, path, _ in plan:
        try:
            shutil.rmtree(path)
        except OSError as exc:
            print(f"✗ 删不掉 {label} {path}：{exc}", file=sys.stderr)
            failed = True
    print(f"已删 {dirs} 个目录 / {files} 个文件 / {size} 字节")
    return 1 if failed else 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="一条命令跑完热点分析（默认取 work/hook 里最近一次编译 hook 的源）",
    )
    parser.add_argument(
        "command",
        nargs="?",
        choices=["clean"],
        help="clean：删 sidecar 缓存与 work/tmp/hotspot_run（不跑分析）",
    )
    parser.add_argument(
        "--src",
        help="源文件 .si（不给就取 work/hook 里最近一次编译 hook 的源）",
    )
    parser.add_argument("--stdlib", help="stdlib 前缀 .si（复现 hook 产物时用）")
    parser.add_argument(
        "--artifact",
        help="现成的产物 .bin（配合 --sidecar，跳过编译与 sidecar 生成）",
    )
    parser.add_argument("--sidecar", help="--artifact 对应的 sidecar.json")
    parser.add_argument("--window", type=int, default=10, help="采样窗口秒数（默认 10）")
    parser.add_argument("--pid", type=int, help="游戏 PID（默认按进程名找）")
    parser.add_argument("--interval-ms", type=int, help="采样间隔毫秒")
    parser.add_argument(
        "--out",
        help="run 目录（默认 hotspot/workspace/runs/<ts>_<name>_t<秒>）",
    )
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="config.toml 路径")
    parser.add_argument("--clean", action="store_true", help="成功后删除中间产物")
    parser.add_argument(
        "--keep-running",
        action="store_true",
        help="采样后不停电路（就算是我们启动的也留着）",
    )
    parser.add_argument(
        "--no-sim-control",
        action="store_true",
        help="不自动启停仿真（仿真没在跑就报错）",
    )
    parser.add_argument(
        "--skip-game-check",
        action="store_true",
        help="跳过 sim_speed 检查（离线/测试；也是「不启停」）",
    )
    parser.add_argument(
        "--runs",
        action="store_true",
        help="clean 时连 hotspot/workspace/runs 一起删（默认保留）",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="clean 时只列将删的路径与数量，不删",
    )
    opts = parser.parse_args(argv)
    if opts.command != "clean" and (opts.runs or opts.dry_run):
        parser.error("--runs / --dry-run 只用于 clean 子命令")
    opts.hook_num = None        # 无参模式选中的 hook 编号（run 目录名要用）
    opts.started_by_us = False  # 仿真是不是我们启动的（只有这种才在结束时停）
    return opts


def stop_stage(opts: argparse.Namespace) -> None:
    """结束仿真 —— 只停我们自己启动的，失败只告警。

    判据（是不是我们启动的、有没禁自动控制）放在函数里而不是 main 的 finally 里：
    finally 只管「跑完了该收拾」，要不要真动仿真由这里说。
    """
    if opts.no_sim_control or not opts.started_by_us:
        return
    try:
        _run([sys.executable, SIMCTL, "stop"], stage="stop", timeout=30)
    except StageError as exc:
        print(f"⚠ 停电路失败：{exc}", file=sys.stderr)


def _run_stem(opts: argparse.Namespace, target: Path) -> str:
    """run/工作目录的名字主干：无参模式标 hook 编号，否则用文件名主干。"""
    if opts.hook_num is not None:
        return f"hook{opts.hook_num}"
    return target.stem


def main(argv: list[str] | None = None) -> int:
    opts = parse_args(argv)
    if opts.command == "clean":
        return clean_command(opts)

    work_dir: Path | None = None
    try:
        config, target = preflight(opts)
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        stem = _run_stem(opts, target)
        work_dir = WORK_TMP / f"{stamp}_{stem}"
        run_dir = (
            _resolve(opts.out)
            if opts.out
            else RUNS_DIR / f"{stamp}_{stem}_t{opts.window}"
        )

        if opts.artifact is not None:
            blob = target
            sidecar = _resolve(opts.sidecar)
        else:
            paths = compile_stage(opts, work_dir, target)
            sidecar = sidecar_stage(paths)
            blob = paths["blob"]

        stats = sample_stage(opts, config, blob, run_dir)
        html = report_stage(run_dir, sidecar)
    except StageError as exc:
        print(f"✗ [{exc.stage}] {exc}", file=sys.stderr)
        if exc.command:
            print("  命令：" + " ".join(exc.command), file=sys.stderr)
        if exc.output:
            print("  输出尾部：", file=sys.stderr)
            for line in exc.output.splitlines()[-12:]:
                print(f"    {line}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("✗ 中断", file=sys.stderr)
        return 130
    finally:
        if not opts.keep_running:
            stop_stage(opts)

    share = 100 * stats["bucket0"] / stats["samples"] if stats["samples"] else 0.0
    print(f"完成：{run_dir}")
    print(f"  report：{html}")
    print(
        f"  样本 {stats['samples']}（bucket0 {stats['bucket0']} / "
        f"bucket1 {stats['bucket1']} / bucket2 {stats['bucket2']}），"
        f"bucket0 份额 {share:.1f}%"
    )

    if opts.clean and work_dir is not None and work_dir.exists():
        try:
            shutil.rmtree(work_dir)
        except OSError as exc:
            print(f"⚠ 中间产物没删掉：{exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
