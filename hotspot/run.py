"""一条命令跑完热点分析：编译 → sidecar → 采样 → 报表。

只做编排：每个阶段调用现成的可执行文件/脚本，不复制它们的逻辑。

不给 `--src` 就用 `work/hook` 里最近一次编译 hook 的源；仿真没在跑时自己点运行，
只在「是我们启动的」情况下才在结束时停（原本在跑的电路不去动）。
`--artifact` + `--sidecar` 能拿现成产物跳过编译，拿它做同一份产物的重采/重算。
config 里 `sampler.backend` 是 etw 且当前不是管理员时，先 UAC 提权重启自己（见 maybe_relaunch）；
suspend 免管理员，不提权也不弹框。提权重启会在参数里加 `--pause-on-exit`：新控制台跟着
进程一起关，不停一下的话结果和报错都看不到。
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
import ctypes
import hashlib
import importlib.util
import json
import os
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
DEFAULT_CONFIG = ROOT / "hotspot/config.ini"
SIDECAR_CACHE = ROOT / "hotspot/workspace/cache/sidecar"
WORK_TMP = ROOT / "work/tmp/hotspot_run"
RUNS_DIR = ROOT / "hotspot/workspace/runs"
RUN_SCRIPT = Path(__file__).resolve()
LOG_DIR = ROOT / "work/tmp"
LOG_STAMP = "%Y-%m-%d_%H-%M-%S"
LOG_FILE_OPTION = "--log-file"

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


# ------------------------------------------------------------------- 运行日志
#
# 一次运行一份日志，提权重启前后两个进程追加**同一个文件**（子进程靠 `--log-file` 拿到路径）。
# 提权后新进程活在一个随退出就关掉的新控制台里，日志是事后唯一的现场，所以每个子进程调用
# 的命令、输入、退出码、输出都要落下来 —— 报错时人只记得住「刚才那步失败了」。
class RunLog:
    """一次运行的日志文件；写入永远追加，写失败一律吞掉。

    写失败不能带崩分析：磁盘满、路径被占、文件被别的程序锁住都可能发生，那时该丢的是
    日志这一份旁证，而不是跑了一半的分析。
    """

    def __init__(self, path: Path) -> None:
        self.path = path

    def write(self, text: str) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a", encoding="utf-8") as handle:
                handle.write(text)
                if not text.endswith("\n"):
                    handle.write("\n")
        except OSError:
            pass


_LOG: RunLog | None = None


def log_write(text: str) -> None:
    """往本次运行的日志里追加一条记录（每行带时间戳）；没有日志时什么都不做。"""
    if _LOG is None:
        return
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    _LOG.write("\n".join(f"[{stamp}] {line}" for line in text.splitlines() or [""]))


def log_block(label: str, body: str) -> None:
    """多行内容缩进四格跟在 label 后面；空内容写 `（空）`，好区分「没输出」和「没记」。"""
    lines = body.splitlines()
    if not lines:
        log_write(f"{label}（空）")
        return
    log_write(f"{label}\n" + "\n".join(f"    {line}" for line in lines))


def _log_path(opts: argparse.Namespace) -> Path:
    """日志路径：`--log-file` 优先，否则 `work/tmp/run_<时间戳>.log`。

    同一秒内跑两次不能糊进一个文件：那样两次运行的输出交错，谁都读不出来。
    """
    if opts.log_file:
        return _resolve(opts.log_file)
    stamp = datetime.now().strftime(LOG_STAMP)
    path = LOG_DIR / f"run_{stamp}.log"
    serial = 2
    while path.exists():
        path = LOG_DIR / f"run_{stamp}-{serial}.log"
        serial += 1
    return path


def open_run_log(opts: argparse.Namespace, arguments: list[str]) -> Path:
    """开本次运行的日志并写下启动段，返回路径（提权重启要把它传给子进程）。"""
    global _LOG
    path = _log_path(opts)
    _LOG = RunLog(path)
    log_write("=" * 64)
    log_write(
        f"运行开始\n"
        f"argv：{subprocess.list2cmdline([sys.executable, str(RUN_SCRIPT), *arguments])}\n"
        f"cwd：{Path.cwd()}\n"
        f"pid：{os.getpid()}\n"
        f"backend：{sampler_backend(_resolve(opts.config))}\n"
        f"log：{path}"
    )
    return path


def _run(
    command: list[str | Path],
    *,
    stage: str,
    input_text: str | None = None,
    timeout: float = 120,
) -> str:
    parts = [str(part) for part in command]
    log_write(f"[{stage}] 调用：{subprocess.list2cmdline(parts)}")
    if input_text is not None:
        log_block(f"[{stage}] 输入：", input_text)
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
        log_write(f"[{stage}] 超时（{timeout:g} s）")
        raise StageError(stage, f"超时（{timeout:g} s）", parts) from exc
    except OSError as exc:
        log_write(f"[{stage}] 启动失败：{exc}")
        raise StageError(stage, f"启动失败：{exc}", parts) from exc

    log_write(f"[{stage}] 退出码 {result.returncode}")
    log_block(f"[{stage}] stdout：", result.stdout or "")
    log_block(f"[{stage}] stderr：", result.stderr or "")

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


# ------------------------------------------------------- ETW 提权（suspend 绝不提权）
#
# etw 要开 NT Kernel Logger（SeSystemProfilePrivilege），非管理员必然失败。提权放在 run.py
# 而不是让 consumer 自己提：consumer 一旦用 runas 重启就换到**新控制台**，它 stdin/stdout
# 上的管道全断，`open/window/save/quit` 喂不进去、统计也读不回来。反过来 run.py 提权后
# consumer 继承管理员令牌，它自己那次判断看到「已是管理员」就直接过，不会二次 UAC。
# suspend 免管理员：提权只会白弹一次框，所以判据里必须有后端。
BACKEND_REQUIRING_ADMIN = "etw"
# 提权重启打开的是一个新控制台，它跟着进程一起关：加这个参数让新进程退出前停一下。
PAUSE_ON_EXIT = "--pause-on-exit"
TOKEN_QUERY = 0x0008
TOKEN_ELEVATION_CLASS = 20
SHELL_EXECUTE_MIN_SUCCESS = 32  # ShellExecuteW 返回值 <= 32 都是 SE_ERR_* 或 ERROR_CANCELLED
SE_ERR_ACCESSDENIED = 5
ERROR_CANCELLED = 1223


class _TokenElevation(ctypes.Structure):
    """GetTokenInformation(TokenElevation) 的输出：一个 BOOL。"""

    _fields_ = [("token_is_elevated", ctypes.c_int)]


def is_elevated() -> bool:
    """当前进程是否已在管理员令牌下。

    判据跟 consumer 的 TokenElevation 一模一样：两边判得不一样就会出现「run.py 认为不用
    提权、consumer 自己又提一次」，那一次会把管道换成新控制台。所以不用 IsUserAnAdmin
    —— UAC 关掉时它对管理员账户返回 True，而那时候进程并没有提升。
    """
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    advapi32.OpenProcessToken.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    advapi32.OpenProcessToken.restype = ctypes.c_int
    advapi32.GetTokenInformation.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    advapi32.GetTokenInformation.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    token = ctypes.c_void_p()
    if not advapi32.OpenProcessToken(
        kernel32.GetCurrentProcess(), TOKEN_QUERY, ctypes.byref(token)
    ):
        return False
    try:
        elevation = _TokenElevation()
        returned = ctypes.c_uint32()
        queried = advapi32.GetTokenInformation(
            token,
            TOKEN_ELEVATION_CLASS,
            ctypes.byref(elevation),
            ctypes.sizeof(elevation),
            ctypes.byref(returned),
        )
        return bool(queried) and bool(elevation.token_is_elevated)
    finally:
        kernel32.CloseHandle(token)


def sampler_backend(config: Path) -> str:
    """config.ini 里的 `sampler.backend`；文件读不到就按 consumer 的默认值 etw。

    值按冻结的 INI 子集解：字符串是 JSON 编码的（`backend = "suspend"`），解析不了就当没写。
    不认识的 backend 值照原样返回，由 consumer 报错，而不是在这里猜。
    """
    parser = configparser.ConfigParser(
        delimiters=("=",),
        interpolation=None,
        comment_prefixes=("#", ";"),
        inline_comment_prefixes=("#", ";"),
    )
    try:
        with config.open(encoding="utf-8-sig") as handle:
            parser.read_file(handle)
        value = json.loads(parser.get("sampler", "backend", raw=True))
    except (OSError, UnicodeDecodeError, configparser.Error, json.JSONDecodeError):
        return BACKEND_REQUIRING_ADMIN
    return value if isinstance(value, str) and value else BACKEND_REQUIRING_ADMIN


def needs_elevation(backend: str, elevated: bool) -> bool:
    """只有 etw 需要管理员；suspend 免管理员，提权只会白弹一次 UAC 还换掉控制台。"""
    return backend == BACKEND_REQUIRING_ADMIN and not elevated


def _shell_execute_runas(target: Path, parameters: str, directory: Path) -> int:
    """ShellExecuteW("runas") 的薄壳：> 32 才算起来了（其余值是 SE_ERR_* 或 ERROR_CANCELLED）。"""
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    shell32.ShellExecuteW.restype = ctypes.c_void_p  # 实际是 HINSTANCE
    result = shell32.ShellExecuteW(
        None, "runas", str(target), parameters, str(directory), 1  # 1 = SW_SHOWNORMAL
    )
    return int(result or 0)


def _with_log_file(arguments: list[str], log_file: Path) -> list[str]:
    """参数里补上 `--log-file <路径>`，已有的（含 `--log-file=…` 形式）先删掉 —— 只留一个。

    提权重启换了一个进程，日志要接着往同一份文件里写：不传这个参数，子进程会自己
    按时间戳新建一份，原进程那半段就成了孤儿。
    """
    cleaned: list[str] = []
    skip_value = False
    for argument in arguments:
        if skip_value:
            skip_value = False
        elif argument == LOG_FILE_OPTION:
            skip_value = True
        elif argument.startswith(f"{LOG_FILE_OPTION}="):
            continue
        else:
            cleaned.append(argument)
    return [*cleaned, LOG_FILE_OPTION, str(log_file)]


def maybe_relaunch(
    config: Path, arguments: list[str], *, log_file: Path | None = None
) -> int | None:
    """etw 且非管理员时用 runas 把自己重启一遍。

    返回 None = 原地继续跑；返回 int = 原进程该以这个码退出（0 表示已经交给提权进程）。
    参数用 `subprocess.list2cmdline` 拼（CRT 的引号规则）—— 含空格的路径不会被拆成两段。
    新进程那边自动带上 `--pause-on-exit`（已经给了就不重复加），以及 `log_file`（同一份日志）。
    """
    if not needs_elevation(sampler_backend(config), is_elevated()):
        return None
    relaunch_arguments = list(arguments)
    if PAUSE_ON_EXIT not in relaunch_arguments:
        relaunch_arguments.append(PAUSE_ON_EXIT)
    if log_file is not None:
        relaunch_arguments = _with_log_file(relaunch_arguments, log_file)
    parameters = subprocess.list2cmdline([str(RUN_SCRIPT), *relaunch_arguments])
    print("→ etw 需要管理员：请求提权（UAC）…", flush=True)
    result = _shell_execute_runas(Path(sys.executable), parameters, Path.cwd())
    # 先判取消：ERROR_CANCELLED 是 1223，落在「> 32 就是成功」之上，单靠大小会当成成功。
    if result in (0, SE_ERR_ACCESSDENIED, ERROR_CANCELLED):
        print("✗ 已取消提权（或 UAC 拒绝）：etw 后端要开内核日志会话（SeSystemProfilePrivilege）",
              file=sys.stderr)
        return 1
    if result <= SHELL_EXECUTE_MIN_SUCCESS:
        print(f"✗ 提权重启失败：ShellExecuteW 返回 {result}", file=sys.stderr)
        return 1
    return 0


def _stdin_is_tty() -> bool:
    """stdin 是不是终端；被换掉/关掉的 stdin（管道、StringIO、None）一律当非 TTY。"""
    try:
        return sys.stdin.isatty()
    except (AttributeError, ValueError):
        return False


def pause_before_exit(opts: argparse.Namespace) -> None:
    """提权后的进程活在一个随退出就关掉的新控制台里：不停一下，报错和结果一起没。

    只看 `--pause-on-exit` 而不看「是不是提权过」：管理员终端直接跑不带它就不停。
    管道调用（别的脚本调 run.py、单测）也不等 —— 等下去只会把调用方挂住。
    """
    if not opts.pause_on_exit or not _stdin_is_tty():
        return
    print("按回车退出…", end="", flush=True)
    with contextlib.suppress(OSError, ValueError):
        sys.stdin.readline()


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
    print(f"→ 用 hook 产物 {product.num}")
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
            f'找不到 {config}；复制 hotspot/config.example.ini 并把 sampler.backend 改成 "suspend"',
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

    for name in ("run.ini", "samples.tsv"):
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
    html = run_dir.parent / "html" / f"{run_dir.name}.html"
    if not html.is_file():
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
        "-s",
        "--src",
        help="源文件 .si（不给就取 work/hook 里最近一次编译 hook 的源）",
    )
    parser.add_argument("-l", "--stdlib", help="stdlib 前缀 .si（复现 hook 产物时用）")
    parser.add_argument(
        "-a",
        "--artifact",
        help="现成的产物 .bin（配合 --sidecar，跳过编译与 sidecar 生成）",
    )
    parser.add_argument("-S", "--sidecar", help="--artifact 对应的 sidecar.json")
    parser.add_argument("-w", "--window", type=int, default=10, help="采样窗口秒数（默认 10）")
    parser.add_argument("-p", "--pid", type=int, help="游戏 PID（默认按进程名找）")
    parser.add_argument("-i", "--interval-ms", type=int, help="采样间隔毫秒")
    parser.add_argument(
        "-o",
        "--out",
        help="run 目录（默认 hotspot/workspace/runs/<ts>_<name>_t<秒>）",
    )
    parser.add_argument("-c", "--config", default=str(DEFAULT_CONFIG), help="config.ini 路径")
    parser.add_argument("-C", "--clean", action="store_true", help="成功后删除中间产物")
    parser.add_argument(
        "-k",
        "--keep-running",
        action="store_true",
        help="采样后不停电路（就算是我们启动的也留着）",
    )
    parser.add_argument(
        "-n",
        "--no-sim-control",
        action="store_true",
        help="不自动启停仿真（仿真没在跑就报错）",
    )
    parser.add_argument(
        "-g",
        "--skip-game-check",
        action="store_true",
        help="跳过 sim_speed 检查（离线/测试；也是「不启停」）",
    )
    parser.add_argument(
        "-P",
        PAUSE_ON_EXIT,
        dest="pause_on_exit",
        action="store_true",
        help="退出前等一次回车（提权重启时自动加；stdin 非 TTY 不等）",
    )
    parser.add_argument(
        "-L",
        LOG_FILE_OPTION,
        dest="log_file",
        help="内部用：日志文件路径（默认 work/tmp/run_<时间戳>.log；提权重启时原样给子进程）",
    )
    parser.add_argument(
        "-r",
        "--runs",
        action="store_true",
        help="clean 时连 hotspot/workspace/runs 一起删（默认保留）",
    )
    parser.add_argument(
        "-d",
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
        log_write(f"[{exc.stage}] 失败：{exc}")
        print(f"⚠ 停电路失败：{exc}", file=sys.stderr)


def _run_stem(opts: argparse.Namespace, target: Path) -> str:
    """run/工作目录的名字主干：无参模式标 hook 编号，否则用文件名主干。"""
    if opts.hook_num is not None:
        return f"hook{opts.hook_num}"
    return target.stem


def main(argv: list[str] | None = None) -> int:
    """跑一次分析（或 clean），结束前按需等一次回车。"""
    opts = parse_args(argv)
    arguments = list(sys.argv[1:] if argv is None else argv)
    open_run_log(opts, arguments)
    code = _pipeline(opts, arguments)
    log_write(f"退出码 {code}")
    pause_before_exit(opts)
    return code


def _pipeline(opts: argparse.Namespace, arguments: list[str]) -> int:
    """main 的实体：clean 子命令、提权交接、四阶段流程各自给退出码。"""
    if opts.command == "clean":
        return clean_command(opts)

    # 提权在 preflight 之前：非管理员的 etw 跑下去只会卡在开会话那一步。
    relaunched = maybe_relaunch(
        _resolve(opts.config),
        arguments,
        log_file=_LOG.path if _LOG is not None else None,
    )
    if relaunched is not None:
        log_write(f"已交给提权进程：原进程退出码 {relaunched}")
        return relaunched

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
        log_write(f"preflight 通过：config={config}，目标={target}")

        if opts.artifact is not None:
            blob = target
            sidecar = _resolve(opts.sidecar)
        else:
            paths = compile_stage(opts, work_dir, target)
            sidecar = sidecar_stage(paths)
            blob = paths["blob"]
        log_write(f"产物：blob={blob}，sidecar={sidecar}")

        stats = sample_stage(opts, config, blob, run_dir)
        html = report_stage(run_dir, sidecar)
    except StageError as exc:
        log_write(f"[{exc.stage}] 失败：{exc}")
        if exc.command:
            log_write(f"[{exc.stage}] 命令：{subprocess.list2cmdline(exc.command)}")
        if exc.output:
            log_block(f"[{exc.stage}] 输出尾部：", exc.output)
        print(f"✗ [{exc.stage}] {exc}", file=sys.stderr)
        if exc.command:
            print("  命令：" + " ".join(exc.command), file=sys.stderr)
        if exc.output:
            print("  输出尾部：", file=sys.stderr)
            for line in exc.output.splitlines()[-12:]:
                print(f"    {line}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        log_write("中断（KeyboardInterrupt）")
        print("✗ 中断", file=sys.stderr)
        return 130
    finally:
        if not opts.keep_running:
            stop_stage(opts)

    share = 100 * stats["bucket0"] / stats["samples"] if stats["samples"] else 0.0
    log_write(
        f"report：{html}\n"
        f"样本 {stats['samples']}（bucket0 {stats['bucket0']} / "
        f"bucket1 {stats['bucket1']} / bucket2 {stats['bucket2']}），"
        f"bucket0 份额 {share:.1f}%"
    )
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
