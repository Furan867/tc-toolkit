"""一条命令跑完热点分析：编译 → sidecar → 采样 → 报表。

只做编排：每个阶段调用现成的可执行文件/脚本，不复制它们的逻辑。
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIMPLEXC = ROOT / "simplexc/build/simplexc_cli.exe"
CONSUMER = ROOT / "hotspot/consumer/build/hotspot-consumer.exe"
MAKE_SIDECAR = ROOT / "hotspot/tools/make_sidecar.py"
REPORT = ROOT / "hotspot/analysis/report.py"
READ_FREQ = ROOT / "tools/read_freq.py"
DEFAULT_CONFIG = ROOT / "hotspot/config.toml"

SIM_SPEED = re.compile(r"sim_speed\s+([0-9]+(?:\.[0-9]+)?)")
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


def _game_running() -> bool:
    if not READ_FREQ.is_file():
        return False
    try:
        output = _run(
            [sys.executable, READ_FREQ, "--samples", "1"],
            stage="preflight",
            timeout=20,
        )
    except StageError:
        return False
    match = SIM_SPEED.search(output)
    return bool(match and float(match.group(1)) > 0)


def preflight(opts: argparse.Namespace) -> tuple[Path, Path]:
    config = _resolve(opts.config)
    if not config.is_file():
        raise StageError(
            "preflight",
            f'找不到 {config}；复制 hotspot/config.example.toml 并把 sampler.backend 改成 "suspend"',
        )

    for name, path in (("simplexc_cli", SIMPLEXC), ("hotspot-consumer", CONSUMER)):
        if not path.is_file():
            raise StageError("preflight", f"缺少 {name}：{path}，先构建")

    src = _resolve(opts.src)
    if not src.is_file():
        raise StageError("preflight", f"源文件不存在：{src}")
    if opts.stdlib and not _resolve(opts.stdlib).is_file():
        raise StageError("preflight", f"stdlib 前缀不存在：{_resolve(opts.stdlib)}")

    if not opts.skip_game_check and not _game_running():
        raise StageError(
            "preflight",
            "游戏没在跑（sim_speed=0 或读不到）；先 python tools/simctl.py run",
        )
    return config, src


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


def sidecar_stage(paths: dict[str, Path]) -> Path:
    sidecar = paths["blob"].parent / "sidecar.json"
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


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="一条命令跑完热点分析")
    parser.add_argument("--src", required=True, help="源文件 .si")
    parser.add_argument("--stdlib", help="stdlib 前缀 .si（复现 hook 产物时用）")
    parser.add_argument("--window", type=int, default=10, help="采样窗口秒数（默认 10）")
    parser.add_argument("--pid", type=int, help="游戏 PID（默认按进程名找）")
    parser.add_argument("--interval-ms", type=int, help="采样间隔毫秒")
    parser.add_argument("--out", help="run 目录（默认 hotspot/workspace/runs/<ts>_<name>_t<秒>）")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="config.toml 路径")
    parser.add_argument("--clean", action="store_true", help="成功后删除中间产物")
    parser.add_argument(
        "--keep-running",
        action="store_true",
        help="采样后不停电路（默认停）",
    )
    parser.add_argument(
        "--skip-game-check",
        action="store_true",
        help="跳过 sim_speed 检查（离线/测试）",
    )
    return parser.parse_args(argv)


def stop_stage() -> None:
    """采样结束后停电路；失败只告警，不影响主流程。"""
    try:
        _run(
            [sys.executable, ROOT / "tools/simctl.py", "stop"],
            stage="stop",
            timeout=30,
        )
    except StageError as exc:
        print(f"⚠ 停电路失败：{exc}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    opts = parse_args(argv)
    preflight_done = False
    try:
        config, src = preflight(opts)
        preflight_done = True
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        stem = src.stem
        work_dir = ROOT / "work/tmp/hotspot_run" / f"{stamp}_{stem}"
        run_dir = (
            _resolve(opts.out)
            if opts.out
            else ROOT / "hotspot/workspace/runs" / f"{stamp}_{stem}_t{opts.window}"
        )

        paths = compile_stage(opts, work_dir, src)
        sidecar = sidecar_stage(paths)
        stats = sample_stage(opts, config, paths["blob"], run_dir)
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
        if preflight_done and not opts.keep_running:
            stop_stage()

    share = 100 * stats["bucket0"] / stats["samples"] if stats["samples"] else 0.0
    print(f"完成：{run_dir}")
    print(f"  report：{html}")
    print(
        f"  样本 {stats['samples']}（bucket0 {stats['bucket0']} / "
        f"bucket1 {stats['bucket1']} / bucket2 {stats['bucket2']}），"
        f"bucket0 份额 {share:.1f}%"
    )

    if opts.clean:
        try:
            shutil.rmtree(work_dir)
        except OSError as exc:
            print(f"⚠ 中间产物没删掉：{exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
