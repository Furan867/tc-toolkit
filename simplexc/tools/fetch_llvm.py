#!python3
"""取官方 LLVM Windows 发行包里的 `LLVM-C.dll` + `llvm-c` 头文件（M1 spike 的输入）。

为什么这么做
------------
官方 win64 包（`clang+llvm-*-x86_64-pc-windows-msvc.tar.zst`）里带的是 **MSVC 导入库**
（`lib/LLVM-C.lib`），MinGW 的 ld 未必吃得下；所以解出来之后另用 `gendef` + `dlltool`
造一份 MinGW 导入库（`libLLVM-C.a`）。`LLVM-C.dll` 本身是 C ABI，MinGW 直接链没有问题
（依赖的 MSVC 运行时 `vcruntime140(_1).dll` / `msvcp140.dll` 系统里已有）。

不整包解压：全解要 1.5 GB 上下，而我们只要 DLL + 头 + 导入库，用 `zstd | tar --wildcards` 挑。

用法
----
    py simplexc/tools/fetch_llvm.py                               # 最新稳定版 → simplexc/.deps/downloads + simplexc/.deps/llvm
    py simplexc/tools/fetch_llvm.py --tag llvmorg-23.1.3
    py simplexc/tools/fetch_llvm.py --list                        # 只列资产（含 sha256），不下载
    py simplexc/tools/fetch_llvm.py --extra bin/clang.exe          # 额外多解一个成员（可重复）
    py simplexc/tools/fetch_llvm.py --offline --tag llvmorg-23.1.3 --extra bin/clang.exe   # 不联网重解一遍

产出
----
    <dest>/<asset 名>              压缩包（默认保留，`--no-keep` 删）
    <root>/bin/LLVM-C.dll
    <root>/include/llvm-c/*.h
    <root>/lib/LLVM-C.lib
    <root>/<--extra 给的路径>      例 `bin/clang.exe`（M1.5 用它编译 C 等价物）
    <root>/llvm.json               版本 / 资产名 / sha256 / 大小 / 每个文件的 sha256

两份资产都落在 `simplexc/.deps/`（不进 git）：仓库里不放几百 MB 二进制，工作区里也只有
一个位置。换位置就 `--root` / `--dest`，CMake 那边用 `-DLLVM_C_ROOT` 或 `TC_LLVM_DIR` 跟上。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile

# 这一个文件在 simplexc/tools/ 下：往上是 simplexc/ —— 两份资产的默认落点都在 simplexc/.deps/ 下。
HERE = os.path.dirname(os.path.abspath(__file__))
SIMPLEXC = os.path.dirname(HERE)

API = "https://api.github.com/repos/llvm/llvm-project/releases/{ref}"
ASSET_KEY = "x86_64-pc-windows-msvc.tar.zst"
# 默认成员（LLVM-C 链接那一套）；`--extra` 的成员追加在后面，默认行为不变
MEMBERS = ("bin/LLVM-C.dll", "include", "lib/LLVM-C.lib")
# 解压这 1 GB 窗口的 zst 要 `--long=30`。zstd 不写死路径：装在哪是机器的事 ——
# PATH 里有就用，没有就看 ZSTD 环境变量，两者都没有直接报错（见 require_zstd）。
DEFAULT_DEST = os.path.join(SIMPLEXC, ".deps", "downloads")
DEFAULT_ROOT = os.path.join(SIMPLEXC, ".deps", "llvm")


def require_zstd() -> str:
    """zstd 的可执行文件：`PATH` → 环境变量 `ZSTD` → 报错。"""
    path = shutil.which("zstd") or os.environ.get("ZSTD")
    if not path:
        raise SystemExit("找不到 zstd：装一个（如 `scoop install zstd`）或设 ZSTD 环境变量。")
    return path


def api(refl: str) -> dict:
    """查 GitHub 发行信息。走 curl 而不是 urllib —— 同一个工具里只留一条取网络的路径。

    ⚠ `releases/{ref}` 只在 ref=latest 时合法；按 tag 查必须走 `releases/tags/<tag>`
    （写错会 404，而 curl 的报错里看不出是路径问题）。
    """
    ref = refl if refl == "latest" else f"tags/{refl}"
    url = API.format(ref=ref)
    if not url.startswith("https://"):
        raise SystemExit(f"拒绝非 https 地址：{url}")
    r = subprocess.run(["curl", "-sS", "-L", "--fail", "--retry", "3",
                        "-H", "User-Agent: tc-fetch_llvm", url],
                       capture_output=True, check=False)
    if r.returncode != 0:
        raise SystemExit(f"取发行信息失败（curl {r.returncode}）："
                         f"{r.stderr.decode('utf-8', 'replace').strip()}")
    try:
        return json.loads(r.stdout.decode("utf-8"))
    except ValueError as exc:
        raise SystemExit(f"发行信息不是合法 JSON：{exc}") from exc


def pick_asset(rel: dict) -> dict:
    for a in rel.get("assets", []):
        if ASSET_KEY in a["name"]:
            return a
    raise SystemExit(f"{rel.get('tag_name')} 里没有 {ASSET_KEY} 资产")


def local_asset(dest: str, tag: str, out_root: str) -> dict:
    """不查 API 时的资产描述：名字从 tag 推，期望 sha256 从上次的 llvm.json 拿。

    为什么要这条路：GitHub 的匿名 API 有额度（60/h，用完返回 403），而“重解一遍 clang.exe”
    这种日常操作不该依赖网络，更不该依赖“今天的 latest 还是 23.1.3”。
      · 资产名 = `clang+llvm-<tag 去掉 llvmorg->-x86_64-pc-windows-msvc.tar.zst`；
      · 期望 sha256 = llvm.json 里记的（tag 对得上就用），没有就只报实际值、不假装核过。
    """
    if tag == "latest":
        raise SystemExit("--offline 必须配 `--tag llvmorg-<版本>`（latest 得问 API 才知道是哪个）")
    ver = tag.split("llvmorg-", 1)[-1]
    name = f"clang+llvm-{ver}-{ASSET_KEY}"
    path = os.path.join(dest, name)
    if not os.path.isfile(path):
        raise SystemExit(f"--offline 但本地没有 {path}")
    sha, size = "", os.path.getsize(path)
    meta_path = os.path.join(out_root, "llvm.json")
    try:
        with open(meta_path, encoding="utf-8") as f:
            old = json.load(f)
        if old.get("tag") == tag and old.get("asset") == name:
            sha = str(old.get("sha256") or "")
    except (OSError, ValueError):
        pass
    return {"name": name, "size": size, "digest": f"sha256:{sha}" if sha else "",
            "browser_download_url": ""}


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    except OSError as exc:
        raise SystemExit(f"读不到 {path}：{exc}") from exc
    return h.hexdigest()


def mkdirs(path: str) -> None:
    try:
        os.makedirs(path, exist_ok=True)
    except OSError as exc:
        raise SystemExit(f"建不出目录 {path}：{exc}") from exc


def download(url: str, dest: str) -> None:
    if not url.startswith("https://"):
        raise SystemExit(f"拒绝非 https 地址：{url}")
    mkdirs(os.path.dirname(dest) or ".")
    # -C - 断点续传（GitHub 大文件常被掐断）；--retry-all-errors 才能覆盖 "server closed abruptly"(56)
    cmd = ["curl", "-L", "--fail", "--retry", "10", "--retry-all-errors",
           "--retry-delay", "3", "--speed-limit", "4096", "--speed-time", "60",
           "-C", "-", "-o", dest, url]
    print("$ " + " ".join(cmd))
    r = subprocess.run(cmd, check=False)
    if r.returncode != 0:
        raise SystemExit(f"下载失败（curl 返回 {r.returncode}）")


def wanted(rel: str, extras: tuple[str, ...]) -> bool:
    """tar 内相对路径要不要解：默认三个成员 + 调用者显式要的（`--extra`）。"""
    if rel in extras:
        return True
    return rel == "bin/LLVM-C.dll" or rel == "lib/LLVM-C.lib" or rel.startswith("include/")


def extract(archive: str, top: str, out_root: str, extras: tuple[str, ...] = ()) -> list[str]:
    """只解 MEMBERS（+ extras）里的文件，落到 out_root（保留 bin/ include/ lib/ 结构）。

    用 Python 的 `tarfile` 流式解，**不走外部的 tar**：Windows PATH 里的 tar 未必是 GNU tar
    （bsdtar 没有 `--wildcards`），而我们只需要挑几个文件。
    这份 zst 用 1 GB 窗口压缩，zstd 必须加 `--long=30` 才肯解。

    幂等：重复跑就是重写同样的文件。`--extra` 漏解不出来会**报错**（不静默少给）。
    """
    zstd = require_zstd()
    mkdirs(out_root)
    pz = subprocess.Popen([zstd, "-dc", "--long=30", archive], stdout=subprocess.PIPE)
    if pz.stdout is None:
        raise SystemExit("zstd 没有输出管道")
    got: list[str] = []
    try:
        with tarfile.open(fileobj=pz.stdout, mode="r|") as tf:
            for member in tf:
                if not member.isfile():
                    continue
                rel = member.name
                if rel.startswith(top + "/"):
                    rel = rel[len(top) + 1:]
                if not wanted(rel, extras):
                    continue
                src = tf.extractfile(member)
                if src is None:
                    continue
                dst = os.path.join(out_root, *rel.split("/"))
                mkdirs(os.path.dirname(dst))
                try:
                    with open(dst, "wb") as out, src:
                        shutil.copyfileobj(src, out)
                except OSError as exc:
                    raise SystemExit(f"写不出 {dst}：{exc}") from exc
                got.append(rel)
    except tarfile.TarError as exc:
        raise SystemExit(f"解 tar 失败：{exc}") from exc
    finally:
        pz.stdout.close()
    if pz.wait() != 0:
        raise SystemExit("zstd 非零退出（压缩包可能损坏）")
    missing = [e for e in extras if e not in got]
    if missing:
        raise SystemExit(f"压缩包里没有这些成员（路径写错了？）：{missing}")
    return got


def write_meta(path: str, meta: dict) -> None:
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=2, ensure_ascii=False)
    except OSError as exc:
        raise SystemExit(f"写不出 {path}：{exc}") from exc


def main() -> int:
    ap = argparse.ArgumentParser(description="取 LLVM Windows 发行包里的 LLVM-C.dll 与头文件")
    ap.add_argument("--tag", default="latest", help="如 llvmorg-23.1.3（默认 latest）")
    ap.add_argument("--dest", default=DEFAULT_DEST, help="压缩包落盘目录（默认 simplexc/.deps/downloads）")
    ap.add_argument("--root", default=None, help="解出目录（默认 simplexc/.deps/llvm）")
    ap.add_argument("--list", action="store_true", help="只列资产")
    ap.add_argument("--no-keep", action="store_true", help="解完删压缩包")
    ap.add_argument("--extra", action="append", default=[], metavar="PATH",
                    help="额外解出的成员，tar 内相对路径（可重复；例 bin/clang.exe）")
    ap.add_argument("--offline", action="store_true",
                    help="不查 GitHub API（用本地压缩包 + 上次 llvm.json 里记的 sha256；需配 --tag）")
    a = ap.parse_args()
    extras = tuple(a.extra)

    out_root = a.root or DEFAULT_ROOT

    if a.offline:
        asset = local_asset(a.dest, a.tag, out_root)
        tag = a.tag
    else:
        rel = api(a.tag)
        asset = pick_asset(rel)
        tag = rel["tag_name"]
    digest = ""
    if asset.get("digest"):
        digest = str(asset["digest"]).split(":")[-1]
    print(f"tag      : {tag}")
    print(f"asset    : {asset['name']}  ({asset['size'] / 1048576:.1f} MB)")
    print(f"sha256   : {digest or '（无期望值，只看实际）'}")
    if a.list:
        return 0

    archive = os.path.join(a.dest, asset["name"])
    if not (os.path.isfile(archive) and os.path.getsize(archive) == asset["size"]):
        if a.offline:
            raise SystemExit(f"--offline 但压缩包大小对不上：{archive}")
        download(asset["browser_download_url"], archive)
    else:
        print(f"已有完整压缩包：{archive}")

    got_sha = sha256_of(archive)
    if digest and got_sha != digest:
        raise SystemExit(f"sha256 不符！\n  期望 {digest}\n  实际 {got_sha}")
    print(f"下载校验通过：sha256 {got_sha}")

    top = asset["name"].replace(".tar.zst", "")
    got = extract(archive, top, out_root, extras)
    print(f"解出 {len(got)} 个文件 → {out_root}")
    for g in sorted(got):
        print(f"  {g}")
    # 每个文件的 sha256 也记下来：换机器/重解之后能直接比对（事实账本里记的就是这份数字）
    file_sha = {g: sha256_of(os.path.join(out_root, *g.split("/"))) for g in sorted(got)}
    for g in sorted(got):
        print(f"  sha256 {file_sha[g]}  {g}")

    write_meta(os.path.join(out_root, "llvm.json"),
               {"tag": tag, "asset": asset["name"], "size": asset["size"],
                "sha256": got_sha, "files": sorted(got), "file_sha256": file_sha})
    print(f"元数据 → {os.path.join(out_root, 'llvm.json')}")
    if a.no_keep:
        try:
            os.remove(archive)
            print(f"已删压缩包 {archive}")
        except OSError as exc:
            print(f"删压缩包失败（不影响结果）：{exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
