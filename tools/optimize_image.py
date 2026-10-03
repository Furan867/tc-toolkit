#!python3
"""产物改写器 —— 本阶段是**恒等改写**（读进来、原样写出去）。

它存在的意义不是"优化"，而是证明这条管路通：

    转发层落盘 raw → 起本脚本 → 读回 opt → 就地写回原始缓冲区 → 游戏照常跑

本阶段刻意什么都不做：内容逐字节不变、总长度不变、不碰任何字段。
将来的改写逻辑只换本文件内部，转发层不用动。

用法
----
    py tools/optimize_image.py --in N.raw.bin --out N.opt.bin

诊断
----
每跑一次往 work/opt/optimize.log 追加一行（时间 / 字节数 / sha256）。
**这一行是「子进程真的跑过」的唯一证据** —— 恒等改写时 opt 与 raw 逐字节相同，
只看产物分不出「跑了但没改」和「压根没跑」。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.normpath(os.path.join(HERE, os.pardir, "work", "opt", "optimize.log"))


def run(src: str, dst: str) -> int:
    try:
        with open(src, "rb") as f:
            data = f.read()

        # ↓↓↓ 改写逻辑将来只加在这里；现在是原样透传 ↓↓↓

        with open(dst, "wb") as f:
            f.write(data)

        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  "
                    f"{os.path.basename(src)} → {os.path.basename(dst)}  "
                    f"{len(data)} 字节  sha256 {hashlib.sha256(data).hexdigest()}\n")
    except OSError as exc:
        # 非 0 退出码 = 转发层看到"这趟改写没成功"，于是原样返回原始产物
        print(f"改写失败：{exc}", file=sys.stderr)
        return 2
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="产物改写器（当前：恒等）")
    ap.add_argument("--in", dest="src", required=True, help="转发层落盘的原始产物")
    ap.add_argument("--out", dest="dst", required=True, help="要写回游戏的产物")
    args = ap.parse_args()
    return run(args.src, args.dst)


if __name__ == "__main__":
    sys.exit(main())
