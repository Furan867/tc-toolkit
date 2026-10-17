"""采样循环、分段与事件日志 —— 采样口径的**唯一一处**（设计 §5-2 统一）。

三条口径原来是三份互不相同的实现（`read_freq` 无分段、`watch_freq` 相位对齐 + 分段、
`bench_rewrite` 固定 `sleep(1.0)` + 丢首 1 个），本模块把它们合成一条循环：

    相位对齐  只有游戏重算了 `sim_speed`（`last_speed_check` 变了）才记一个样本，
              这样每个样本都落在「游戏刚写完」那一刻，而不是采样器自己的节拍；
              `interval` 秒没等到新相位时兜底记一次。
    丢首个    `drop` 个开头样本落在「刚启动/刚停止」的切换窗口里（混合值）。
              bench 传 1；watch 不丢（它的分段把过渡段标出来、统计时不选它）。
    聚合      **median**（决策 3）—— 均匀口径，旧 bench 的 mean 已废弃。

停机与低压判据（`docs/facts.md` §采样判据四条）：

    ① 暂停/停止 → 游戏自己把统计归零（`STOP_HZ` 哨兵）；
    ② 电路自带限速 → 停在一个**非 0 的极低值**（`LOW_HZ` 以下）—— 这是真信号，
       **不许当噪声丢**（`is_low` 只做标注，不参与丢弃）；
    ④ 停机时 `last_speed_check` 照常每秒刷新 → 它只能用来判「读数新不新」，
       **不能**用来判仿真在不在跑；判在不在跑看 `hz == 0`，或用
       `probe.hz_from_cycle` 看 `cycle` 有没有前进。
"""
from __future__ import annotations

import statistics
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from . import probe

POLL = 0.1              # 轮询间隔（只为及时捕捉「游戏刚重算」的时刻）
SEG_TOL = 0.05          # 段内允许的相对波动
TRANSIENT_N = 3         # 样本数 < 这个值的段判为「过渡」（窗口横跨切换点）
DROP_FIRST = 1          # bench 一次 run 的开头这个数的样本落在启动切换窗口里

STOP_HZ = 0.0           # ① 停机哨兵：游戏自己把 sim_speed 归零
LOW_HZ = 1e6            # ② 这个值以下多半是电路自带限速（`report.hz_style` 也用这个阈值）

OPT_WAIT = 15.0         # 见过 hook_out 后等 opt.bin 的上限秒数（转发层自己等子进程 8 秒）


def is_stopped(hz: float) -> bool:
    """① 停机哨兵 —— 游戏把统计归零。"""
    return hz == STOP_HZ


def is_low(hz: float) -> bool:
    """② 低非零值 = 电路自带速度上限的候选。是**真信号**，调用方别据此丢弃。"""
    return STOP_HZ < hz < LOW_HZ


@dataclass(frozen=True)
class Sample:
    """一个样本：本地采样时刻 + 游戏自己算的仿真主频（Hz）。"""

    t: float
    hz: float


@dataclass
class Segment:
    """一段连续相近的样本。低值不作噪声处理（可能就是电路自带的上限）。"""

    samples: list[Sample]

    @property
    def values(self) -> list[float]:
        return [s.hz for s in self.samples]

    @property
    def median(self) -> float:
        return statistics.median(self.values)

    @property
    def count(self) -> int:
        return len(self.samples)

    @property
    def stopped(self) -> bool:
        return self.median == STOP_HZ

    @property
    def transient(self) -> bool:
        """窗口横跨切换点的混合段 —— 低样本数，统计时不选它。"""
        return self.count < TRANSIENT_N

    @property
    def spread(self) -> float:
        """段内相对波动（%）；停机段为 0。"""
        m = self.median
        return 0.0 if m == 0 else (max(self.values) - min(self.values)) / m * 100

    @property
    def duration(self) -> float:
        """近似时长（秒）：样本按每秒一个采，末首之差 + 1。"""
        return self.samples[-1].t - self.samples[0].t + 1.0


@dataclass
class Region:
    """一次编译 → 一次运行 的区间（一个电路）。区间只记编号，摘要时再查产物与日志。"""

    label: str
    num: str | None = None
    segments: list[Segment] = field(default_factory=list)

    def add(self, segment: Segment) -> None:
        self.segments.append(segment)


class Segmenter:
    """把连续相近的样本归成段。低值不作噪声处理（可能是电路自带的上限）。"""

    def __init__(self, on_close: Callable[[Segment], None]):
        self.on_close = on_close
        self.samples: list[Sample] = []

    def value(self) -> float:
        """当前段的中位数（判断下一个样本算不算「同一段」）。"""
        return statistics.median(s.hz for s in self.samples)

    def add(self, t: float, hz: float) -> bool:
        """喂一个样本；返回 True 表示由此**开了一段新段**。

        ⚠️ 开新段时必须**先收尾旧段、再把本样本收进新段**。早期版本在 close()
        之后直接 return，把新段的第一个样本悄悄丢了（每段少一个样本），
        见 2026-10-04 的终端输出 `段2·0`。停机（`hz == 0`）与非停机不混段。
        """
        started = False
        if self.samples:
            ref = self.value()
            if hz == 0 or ref == 0:
                same = hz == 0 and ref == 0
            else:
                same = abs(hz - ref) <= ref * SEG_TOL
            if not same:
                self.close()
                started = True
        self.samples.append(Sample(t, hz))
        return started

    def close(self) -> None:
        if not self.samples:
            return
        segment = Segment(self.samples)
        self.samples = []
        self.on_close(segment)

    def count(self) -> int:
        return len(self.samples)


class Sampler:
    """统一采样循环：相位对齐 + 兜底间隔 + 丢首个。

    非阻塞（`poll()` 由调用方的循环驱动），因为 watch 的循环还要在两拍之间处理
    编译点、游戏退出重连、心跳刷新 —— 那些不属于采样本身。
    """

    def __init__(self, h: int, base: int, *, interval: float = 1.0, drop: int = 0,
                 read: Callable[[int, int], tuple[float, float] | None] = probe.read_speed_time,
                 clock: Callable[[], float] = time.time):
        self.h = h
        self.base = base
        self.interval = interval
        self.drop = drop
        self._read = read
        self._clock = clock
        self._last_game_time: float | None = None
        self._sampled_at = 0.0
        self.taken = 0            # 已到相位/间隔的样本数（含被丢的首个）

    def rearm(self) -> None:
        """重连 / 换区间：丢掉上一轮相位，下一个样本重新算。"""
        self._last_game_time = None
        self._sampled_at = 0.0

    def poll(self, now: float | None = None) -> Sample | None:
        """到相位或兜底间隔就返回一个新样本（已应用「丢首个」）；否则 None。

        读数读不到（偏移过时/进程没了）返回 None —— 调用方据此重连或退出。
        """
        now = self._clock() if now is None else now
        cur = self._read(self.h, self.base)
        if cur is None:
            return None
        hz, game_time = cur
        fresh = game_time != self._last_game_time
        if not fresh and now - self._sampled_at < self.interval:
            return None
        self._last_game_time = game_time
        self._sampled_at = now
        self.taken += 1
        if self.taken <= self.drop:
            return None
        return Sample(now, hz)


def collect(h: int, base: int, seconds: float, *, drop: int = DROP_FIRST,
            interval: float = 1.0, sleep: Callable[[float], None] = time.sleep,
            read: Callable[[int, int], tuple[float, float] | None] = probe.read_speed_time,
            clock: Callable[[], float] = time.time,
            ) -> list[Sample]:
    """采 `seconds` 个点（含被丢的首个），返回丢完开头 `drop` 个之后的样本。

    与 watch 走**同一条**循环（`Sampler`），只是有个固定点数上限。旧 bench 的
    `sample_freq` 是固定 `sleep(1.0)` 的私有实现，已废。
    """
    sampler = Sampler(h, base, interval=interval, drop=drop, read=read, clock=clock)
    want = max(0, int(seconds))
    out: list[Sample] = []
    deadline = clock() + want * interval * 4.0 + 10.0
    while sampler.taken < want:
        if clock() >= deadline:        # 读不到 / 相位不动：别死等
            break
        sleep(POLL)
        sample = sampler.poll()
        if sample is not None:
            out.append(sample)
    return out


class Sink:
    """事件日志（TSV，追加写）—— 只记事件与段摘要，不逐样本落盘（不然几百行）。

    终端输出不在这里（那是 `report` 的事）。日志路径由调用方注入。
    """

    HEADER = ("# tools/watch_freq.py 追加写（TSV）\n"
              "# 列：时间\t类型\t值(Hz)\t备注\n")

    def __init__(self, log_path: Path):
        log_path.parent.mkdir(parents=True, exist_ok=True)
        fresh = not log_path.exists()
        self.fh = log_path.open("a", encoding="utf-8")
        if fresh:
            self.fh.write(self.HEADER)
            self.fh.flush()

    def write(self, kind: str, hz: float | None = None, note: str = "") -> None:
        self.fh.write(f"{datetime.now():%Y-%m-%d %H:%M:%S}\t{kind}\t"
                      f"{'' if hz is None else round(hz)}\t{note}\n")
        self.fh.flush()

    def close(self) -> None:
        self.fh.close()
