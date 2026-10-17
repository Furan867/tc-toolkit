"""产物身份与归属：编号、指纹，以及**这一趟到底跑了什么**。

归属只认 `hook.log`，而且三类行回答的是三个不同的问题，互相不能替代
（`docs/facts.md` §产物 rewrite 模式的权威来源）：

| 行 | 键 | 回答 |
|---|---|---|
| `cfg`  | `rewrite=`              | **配了什么**（转发层起改写子进程前读到的 opt.ini） |
| `opt`  | `mode=` `rewritten=`    | 改写器**实际做了什么**（被防御挡回时与 cfg 不同） |
| `impl` | `impl=` `mode=` `result=` | **实际走了哪个实现**；`mode=` 是配的 `compiler`，不是 rewrite |

早先的写法是读 `opt.ini` 的当前值当归属 —— 那是「检测时刻」的配置，用户在两次
编译之间切开关必然错位（实测 #005 标了 off、#006 标了 on，而两者产物 sha 完全相同）。
`impl` 是 per-call 的：配了 `ours_with_fallback` 而 ours 失败的那次写 `impl=fallback`，
只看 `cfg` 就会把回退当成我们的实现（词表唯一处在 `simplexc-interfaces` §5）。

生成物所在：

    work/hook/  转发层捕获：hook_src_NNN.si / hook_out_NNN.bin / manifest.tsv（每次加载清空）
    work/opt/   转发层工作区：NNN.raw.bin / NNN.opt.bin（**改写后**、真正写回游戏的那份）
"""
from __future__ import annotations

import hashlib
import re
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .. import hook_log

ROOT = Path(__file__).resolve().parents[2]
HOOK_DIR = ROOT / "work" / "hook"
OPT_DIR = ROOT / "work" / "opt"

PRODUCT_RE = re.compile(r"^(\d+)\.opt\.bin$")
CAPTURE_RE = re.compile(r"^hook_out_(\d+)\.bin$")

#: 归属取不到的占位符 —— 空串看着像「没有」，`?` 明确是「不知道」。
UNKNOWN = "?"


@dataclass(frozen=True)
class Product:
    """一个改写后产物：编号、文件、mtime。"""

    num: str
    path: Path
    mtime: float

    @property
    def fp(self) -> str:
        return fingerprint(self.path)


def fingerprint(path: Path) -> str:
    """这份机器码的 sha256 前 8 位；读不到（产物被下一次游戏启动清掉）返回 `-`。

    自己算，不去读日志里那串 sha256 —— 两者同源（日志那行就是改写器对这份数据算
    的），但改写器**最后**才写日志，产物出现的瞬间日志可能还差几毫秒。
    """
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    except OSError:
        return "-"
    return h.hexdigest()[:8]


def product_fp(num: str | None) -> str:
    """编号对应的改写后产物的指纹（`-` = 没有编号 / 产物读不到）。"""
    if not num:
        return "-"
    return fingerprint(OPT_DIR / f"{num}.opt.bin")


def latest_product() -> Product | None:
    """工作区里**编号最大**的改写后产物（启动时补报「现在跑的是哪个」）。

    合并前的两份实现判名字形状的方式不一致：一份取文件名前 3 字符的字典序最大
    （编号 ≥1000、或名字不补零时会错），一份用 `^(\\d+)\\.opt\\.bin$` 按整数取最大
    并带上 mtime。编号必须按**整数**比，`wait_new_product` 还要用 mtime 判「这是不是
    本次编译产生的」。
    """
    best: Product | None = None
    try:
        entries = list(OPT_DIR.iterdir())
    except OSError:
        return None
    for p in entries:
        m = PRODUCT_RE.match(p.name)
        if not m or not p.is_file():
            continue
        if best is not None and int(m.group(1)) <= int(best.num):
            continue
        try:
            mtime = p.stat().st_mtime
        except OSError:
            continue
        best = Product(m.group(1), p, mtime)
    return best


def _scan(directory: Path, pattern: re.Pattern[str], seen: set[str]) -> list[str]:
    """目录里比 `seen` 新出现的编号，并把现状登记进 `seen`。

    `seen` 由调用方持有。**首次调用（`seen` 为空）会返回目录里全部编号** —— 那些
    是既有现状、不是刚编译出来的，调用方按惯例丢弃它；从第二次起才只报新编号。
    """
    found: list[tuple[int, str]] = []
    try:
        for p in directory.iterdir():
            if p.is_file():
                m = pattern.match(p.name)
                if m:
                    found.append((int(m.group(1)), m.group(1)))
    except OSError:
        return []
    found.sort()
    nums = [num for _n, num in found]
    new = [num for num in nums if num not in seen]
    seen.clear()
    seen.update(nums)
    return new


def scan_products(seen: set[str]) -> list[str]:
    """新出现的**改写后产物**编号（`NNN.opt.bin`）；首次调用的返回值要丢弃。

    编译点以它为准：改写器是「先写产物 → 自校验 → 写 entry → **最后**写日志」，
    所以产物一出现就代表这一趟改写走完了 —— 此时算指纹、读 `hook.log` 都有值。
    """
    return _scan(OPT_DIR, PRODUCT_RE, seen)


def scan_captures(seen: set[str]) -> list[str]:
    """新出现的**转发层捕获**编号（`work/hook/hook_out_NNN.bin`）；首次调用的返回值要丢弃。

    它只用来发现「编译发生了」：转发层刚落盘、改写子进程还没跑完，这时没有可报的
    身份。一次编译会先后落 `hook_src_NNN.si` 与 `hook_out_NNN.bin`，只认后者，
    免得同一个编译点报两次。
    """
    return _scan(HOOK_DIR, CAPTURE_RE, seen)


def hook_log_path() -> Path:
    """`hook.log` 的路径 —— **唯一来源是 `hook_log.PATH`**（下一批把它从 `work/opt/`
    挪到 `work/hook/` 时只改那一处，这里自动跟着走）。"""
    return Path(hook_log.PATH)


def hook_log_lines(path: Path | None = None) -> list[str]:
    """日志的全部行。编译是它唯一的增长来源，于是行数就是「发出去的按键到底有没有
    触发编译」的判据（SendInput 是异步的，`simctl.recompile()` 的 ok 只表示键发出去了）。"""
    try:
        text = (path or hook_log_path()).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return text.splitlines()


@dataclass(frozen=True)
class Ownership:
    """一次编译的归属 —— `cfg` / `opt` / `impl` 三行的合成，取不到的是 `?`。"""

    num: str
    cfg_rewrite: str
    opt_mode: str
    rewritten: str
    sha_log: str
    impl: str
    compiler_cfg: str
    impl_result: str


def ownership(num: str | None, path: Path | None = None) -> Ownership:
    """按编号把三类行合成一份归属；同号多行取最后一条（值的来源只有日志本身）。"""
    fields = {"num": num or UNKNOWN, "cfg_rewrite": UNKNOWN, "opt_mode": UNKNOWN,
              "rewritten": "", "sha_log": "-", "impl": UNKNOWN,
              "compiler_cfg": UNKNOWN, "impl_result": UNKNOWN}
    where = None if path is None else str(path)      # hook_log 只收 str 路径
    if num:
        for rec in hook_log.find("cfg", num, where):
            if "rewrite" in rec.kv:
                fields["cfg_rewrite"] = rec.kv["rewrite"]
        for rec in hook_log.find("opt", num, where):
            kv = rec.kv
            if "mode" in kv:
                fields["opt_mode"] = kv["mode"]
            if "rewritten" in kv:
                fields["rewritten"] = kv["rewritten"]
            if kv.get("sha256"):
                fields["sha_log"] = kv["sha256"][:8]
        for rec in hook_log.find("impl", num, where):
            kv = rec.kv
            if "impl" in kv:
                fields["impl"] = kv["impl"]
            if "mode" in kv:            # impl 行的 mode = 配的 compiler（≠ opt 行的 mode）
                fields["compiler_cfg"] = kv["mode"]
            if "result" in kv:
                fields["impl_result"] = kv["result"]
    return Ownership(**fields)


def opt_info(num: str | None, path: Path | None = None) -> tuple[str, str, str]:
    """(指纹(日志记的), 改写处数, **实际执行**的 rewrite 模式) —— 取不到退化成占位符。

    读的是 `opt` 行（改写器写的），已按结构化键取，不解析 note 里的中文（文案会改）。
    新代码请用 `ownership()` 一次拿全三行；这个旧签名还在被 watch/bench 的语义引用。
    """
    own = ownership(num, path)
    return own.sha_log, own.rewritten, own.opt_mode


def mode_from_hook_log(num: str | None, path: Path | None = None) -> str:
    """转发层起改写子进程**之前**按当时的 opt.ini 写下的 `rewrite`（`cfg` 行）。

    它同时也是转发层传给改写器的 `--mode`（2026-10-05 起）—— 两边同源，正常必然
    一致。万一不一致（历史上 C 侧自己维护过一份白名单，把 `short_cse` 当非法值归成
    `on`），编译点行会把差异标出来，那正是要查的信号。
    """
    return ownership(num, path).cfg_rewrite


def wait_new_product(prev: str | None, timeout: float, hook_before: int,
                     tag: str, since: float, dead_after: float = 6.0,
                     say: Callable[[str], None] = print) -> tuple[str | None, str]:
    """等**本次发键之后**产生的新产物，返回 (编号, 失败原因)；成功时失败原因为空串。

    必须同时满足「编号更大」与「mtime >= since」：只比编号会误判 —— 上一次运行
    遗留的产物编号就比 prev 大，会被 0.0s 认领（实测 #031 就这样被认成 off 那轮的
    产物，其实那轮根本没编译，游戏没重新加载电路，采样全 0）。
    另外一路盯 hook.log：发键后 dead_after 秒内一条新编译行都没有，就断定按键没
    生效（焦点不在电路编辑区 / 抢前台后第一次发键丢掉），立刻失败，不必等满超时。
    """
    t0 = time.time()
    warned = False
    while True:
        cur = latest_product()
        if cur and (prev is None or int(cur.num) > int(prev)) and cur.mtime >= since:
            return cur.num, ""
        waited = time.time() - t0
        if waited >= timeout:
            return None, f"等了 {timeout:.0f}s 没等到本次编译的产物"
        new_lines = len(hook_log_lines()) - hook_before
        if new_lines <= 0 and waited >= dead_after:
            return None, (f"发键后 {dead_after:.0f}s 内 hook.log 没有新编译行 —— "
                          "两种可能：① 撤销栈空了（在游戏里做几次编辑就能继续）；"
                          "② 按键没送到游戏：游戏被最小化、前台被别的窗口占着抢不过来"
                          "（自动重试 3 次）")
        if waited >= 5.0 and not warned:        # 只提醒一次，别刷屏
            warned = True
            what = (f"编译已触发（hook.log +{new_lines} 行），等改写子进程"
                    if new_lines > 0 else "还没看到编译行")
            say(f"  [{tag}] 等编译产物 {waited:.0f}s …（{what}）")
        time.sleep(0.3)
