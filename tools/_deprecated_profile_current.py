#!python3
"""一键：识别当前电路 → 建映射 → 采样 → 出组件报告。

为什么需要它：切换电路后，源码、JIT 区、映射全部失效，必须重建。而堆里
同时躺着好几份候选源码（当前电路、旧电路残留、抓取片段），用错了映射，
采样点就全被标成电路里根本没有的组件。

识别判据：**采样点处的机器码字节**。
    1. 采样一次 —— 采样点必定落在正在执行的代码上
    2. 把每份候选编译一遍
    3. 用产物的字节去比对采样点位置的字节，要求**偏移完全对齐**
    匹配数最多的那份就是当前电路。

这比早期两种弱判据都可靠：
    ❌「产物开头能在 JIT dump 里找到」—— 所有产物开头都是同一段固定前缀，全命中
    ❌「候选的 #SIMULATION_STATE 内存在变」—— 那是游戏全局状态区，不是电路特有
    ❌ 而且切换电路后旧产物不释放，几份都躺在 JIT 区里，「在不在内存里」分不出谁在跑

编译一律放在子进程：compile.dll 遇到语法错误会直接 abort 进程，不是返回错误码。

用法：
    python tools/profile_current.py                 # 全流程（约 40 秒）
    python tools/profile_current.py --dump-only     # 只抓源码与 JIT
    python tools/profile_current.py --duration 20   # 采样 20 秒
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "work")
PY = sys.executable


def run(args, **kw):
    return subprocess.run(args, cwd=ROOT, **kw)


def scan():
    print("[1/4] 抓取源码与 JIT 代码 ...")
    # dump 全部可执行区（不只最大的若干个）—— 正在跑的电路可能住在很小的区里，
    # 漏掉它，后面的字节比对就一个都对不上（实测踩过：24KB 的小 MAPPED 区被挤掉）。
    run([PY, os.path.join(HERE, "scan_game_mem.py"),
         "--dump-exec", "--max-dump-mb", "1", "--max-dump-regions", "64"])


def candidates():
    """语法完整的候选源码。

    判据：括号配平，且**组件注释的唯一 ID 数**与源码自己报的 ★顶层组件数★
    （`get_component_count()`）吻合。

    ⚠️ 不要用「ID 不重复」判断 —— 实测**完整源码里每个组件 ID 也会出现两次**
    （组件代码 + 绑定），拿它当判据会把所有正常候选误杀。踩过。
    """
    out = []
    for p in sorted(glob.glob(os.path.join(WORK, "generated_code_*.txt"))):
        s = open(p, encoding="utf-8", errors="replace").read()
        if s.count("{") != s.count("}"):
            continue
        m = re.search(r"get_component_count\(\) Int \{ return (\d+)", s)
        if not m:
            continue
        ids = re.findall(r"//\s*(\d+)\s+com_", s)
        if not ids:
            continue
        want = int(m.group(1))
        if abs(len(set(ids)) - want) > max(2, want * 0.05):
            continue
        out.append(p)
    return out


def compile_quiet(src_path):
    """子进程里编译，返回产物 bytes；失败返回 None。"""
    out_path = os.path.join(WORK, "_probe.bin")
    if os.path.exists(out_path):
        os.remove(out_path)
    code = (
        "import sys\n"
        "sys.path.insert(0, sys.argv[3])\n"
        "from try_compile_dll import load_dll\n"
        "from build_component_map import compile_to_bytes\n"
        "dll = load_dll(call_nimmain=False)\n"
        "open(sys.argv[2], 'wb').write(compile_to_bytes(dll, open(sys.argv[1], 'rb').read()))\n"
    )
    r = run([PY, "-c", code, src_path, out_path, HERE], capture_output=True)
    if r.returncode != 0 or not os.path.exists(out_path):
        return None
    data = open(out_path, "rb").read()
    os.remove(out_path)
    return data


def sample(duration):
    hist = os.path.join(WORK, "rips.tsv")
    run([PY, os.path.join(HERE, "sample_threads.py"), "--duration", str(duration), "--hist", hist])
    rips = []
    for line in open(hist, encoding="utf-8").read().splitlines()[1:]:
        p = line.split("\t")
        if len(p) == 2:
            rips.append((int(p[0]), int(p[1])))
    return rips


def load_dumps():
    out = []
    for f in sorted(glob.glob(os.path.join(WORK, "jit_dump_*.bin"))):
        m = re.search(r"jit_dump_([0-9A-Fa-f]+)\.bin", os.path.basename(f))
        if m:
            out.append((int(m.group(1), 16), open(f, "rb").read()))
    return out


def identify(cands, rips, dumps):
    print(f"[3/4] 用采样点字节识别当前电路（{len(cands)} 份候选 / {len(rips)} 个采样地址）...")
    if not rips or not dumps:
        print("  缺少采样或 JIT dump")
        return None
    best, best_hit = None, 0
    for src in cands:
        prod = compile_quiet(src)
        if not prod or len(prod) <= 8:
            print(f"  {os.path.basename(src)}: 编译失败")
            continue
        body = prod[8:]                       # 去掉 NimStringDesc 长度头，JIT 区里没有它
        hit = 0
        for base, d in dumps:
            for rip, cnt in rips:
                off = rip - base
                if 0 <= off < len(d) - 16 and off < len(body) - 16:
                    if d[off:off + 16] == body[off:off + 16]:
                        hit += cnt
        print(f"  {os.path.basename(src)}: 产物 {len(body)}B → 字节一致 {hit} 次")
        if hit > best_hit:
            best, best_hit = src, hit
    if best_hit == 0:
        return None
    return best


def main():
    ap = argparse.ArgumentParser(description="一键性能剖析当前电路")
    ap.add_argument("--dump-only", action="store_true", help="只抓源码与 JIT，不分析")
    ap.add_argument("--duration", type=float, default=10.0, help="采样时长秒")
    args = ap.parse_args()

    scan()
    if args.dump_only:
        return 0

    cands = candidates()
    if not cands:
        print("没有语法完整的候选源码")
        return 1

    print("[2/4] 采样 ...")
    rips = sample(args.duration)
    if not rips:
        print("❌ 采样为空 —— 电路没在运行？")
        return 1

    src = identify(cands, rips, load_dumps())
    if not src:
        print("❌ 没有任何候选的字节能对上采样点 —— 电路可能没在运行，或源码没抓全")
        return 1
    print(f"  ✅ 当前电路 = {os.path.basename(src)}")

    print("[4/4] 建映射 + 出报告 ...")
    run([PY, os.path.join(HERE, "build_component_map.py"), "--src", src, "--keep"])
    run([PY, os.path.join(HERE, "analyze_sim_hotspot.py"),
         "--hist", os.path.join(WORK, "rips.tsv")])
    return 0


if __name__ == "__main__":
    sys.exit(main())
