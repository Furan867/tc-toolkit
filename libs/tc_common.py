#!python3
"""各工具共用的游戏入口：装在哪、哪次构建、哪份二进制。

装在哪不该写死在每个脚本里（写死一次的代价是换机器时全体失效），所以统一走
`game_dir()`：环境变量 `TC_GAME_DIR` → Steam 库 → `<库>/steamapps/common/Turing Complete`。

「游戏更新过没有」有三个读数，分工不同 —— 写死的地址/偏移在 Steam 更新后一律失效，
判据就是它们变了：

    game_version()      EXE 的 PE 版本资源。**这个构建里没有**，所以正常返回 None，
                        见函数注释（别拿它当判据）。
    game_build_id()     appmanifest 的 buildid —— Steam 每次更新都改它。
    game_fingerprint()  `compile.dll` 的内容指纹 —— 二进制变了地址表就失效。

还放「最近一次编译 hook 落下来的产物对在哪」：`latest_hook_product()` ——
无参跑热点分析的入口。
"""
from __future__ import annotations

import ctypes
import hashlib
import os
import pathlib
import re
import shutil
import struct
from dataclasses import dataclass

from libs import paths

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent   # tools/ 的父目录

GAME_EXE_NAME = "Turing Complete.exe"
GAME_DLL_NAME = "compile.dll"
GAME_REAL_DLL_NAME = "compile_real.dll"
GAME_INSTALL_DIR_NAME = "Turing Complete"

_APP_ID = "1444480"
_ACF_NAME = f"appmanifest_{_APP_ID}.acf"
_STEAM_REG_KEY = r"Software\Valve\Steam"


def _as_game_dir(path: pathlib.Path | None) -> pathlib.Path | None:
    """目录里真有游戏 exe 才算数，否则 None（降一级去找）。"""
    if path is None:
        return None
    return path if (path / GAME_EXE_NAME).is_file() else None


def _env_game_dir() -> pathlib.Path | None:
    raw = os.environ.get("TC_GAME_DIR")
    return _as_game_dir(pathlib.Path(raw)) if raw else None


def _steam_root() -> pathlib.Path | None:
    """HKCU\\Software\\Valve\\Steam 的 SteamPath（Steam 本体的安装目录）。"""
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _STEAM_REG_KEY) as key:
            value: str = winreg.QueryValueEx(key, "SteamPath")[0]
    except OSError:
        return None
    return pathlib.Path(value) if value else None


def _vdf_library_paths(vdf: pathlib.Path) -> list[str]:
    """libraryfolders.vdf 里每个库的 `path` 值（按文件里的出现顺序）。

    只认 `"path"` 行，不做完整 VDF 解析：这个文件里一层 key 下就是一层
    `"键"  "值"` 对，取出 path 再按目录探一遍 appmanifest 就够。VDF 的反斜杠是
    双写的（`"F:\\\\opt\\\\SteamLibrary"`），要去掉一层。
    """
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return [m.group(1).replace("\\\\", "\\")
            for m in re.finditer(r'"path"\s*"([^"]*)"', text)]


def _library_roots() -> list[pathlib.Path]:
    """Steam 本体目录 + libraryfolders.vdf 里登记的库目录（去重，保持顺序）。"""
    root = _steam_root()
    if root is None:
        return []
    roots = [root]
    for raw in _vdf_library_paths(root / "steamapps" / "libraryfolders.vdf"):
        lib = pathlib.Path(raw)
        if lib not in roots:
            roots.append(lib)
    return roots


def game_dir() -> pathlib.Path:
    """游戏安装目录（含 `Turing Complete.exe` 的那一层）。

    每一级失败才降级：`TC_GAME_DIR` → Steam 注册表指向的库里登记了本游戏
    的 → `<库>/steamapps/common/Turing Complete`。全不成立抛 RuntimeError。
    """
    if (found := _env_game_dir()) is not None:
        return found

    roots = _library_roots()
    for root in roots:
        installed = root / "steamapps" / "common" / GAME_INSTALL_DIR_NAME
        if not (root / "steamapps" / _ACF_NAME).is_file():
            continue
        if (found := _as_game_dir(installed)) is not None:
            return found

    tried = "、".join(str(r) for r in roots) or "（读不到 Steam 的 SteamPath / libraryfolders.vdf）"
    raise RuntimeError(
        f"找不到游戏安装目录：这些库里都没有 steamapps/{_ACF_NAME} + "
        f"steamapps/common/{GAME_INSTALL_DIR_NAME}/{GAME_EXE_NAME}：{tried}。"
        f"手动指定：设环境变量 TC_GAME_DIR=<含 {GAME_EXE_NAME} 的目录>。"
    )


def game_exe() -> pathlib.Path:
    return game_dir() / GAME_EXE_NAME


def game_dll() -> pathlib.Path:
    return game_dir() / GAME_DLL_NAME


def game_version() -> str | None:
    """EXE 的 PE 版本资源里的 FileVersion；读不到返回 None。绝不抛异常。

    实测这个构建的 `Turing Complete.exe` **没有版本资源**：`GetFileVersionInfoSizeW`
    返回 0、`GetLastError()` = 1813（ERROR_RESOURCE_TYPE_NOT_FOUND），`game_engine.dll`
    与两个 compile.dll 也一样（只有 steam_api64.dll 有）。所以这里正常就是 None ——
    **不要**退化成在 .rdata 里扫 `X.Y.Z` 字面量：全会命中 LLVM 的 11.1.0、Nim 的
    2.36.1 这类无关串。判「Steam 更新过没有」用 game_build_id() / game_fingerprint()。
    """
    try:
        path = game_exe()
        version_api = ctypes.windll.version
    except (OSError, RuntimeError, AttributeError):
        return None

    try:
        size = version_api.GetFileVersionInfoSizeW(str(path), None)
        if not size:
            return None
        block = ctypes.create_string_buffer(size)
        if not version_api.GetFileVersionInfoW(str(path), 0, size, block):
            return None
        fixed = ctypes.c_void_p()
        fixed_len = ctypes.c_uint(0)
        if not version_api.VerQueryValueW(block, "\\", ctypes.byref(fixed),
                                          ctypes.byref(fixed_len)):
            return None
        # VS_FIXEDFILEINFO：dwSignature / dwStrucVersion 之后是 dwFileVersionMS/LS
        # （`.value` 的类型是 int | None：VerQueryValueW 说成功时它必不为 None）
        if fixed.value is None:
            return None
        version_ms, version_ls = struct.unpack("<II", ctypes.string_at(fixed.value + 8, 8))
    except (OSError, TypeError, ValueError, struct.error):
        return None

    parts = [version_ms >> 16, version_ms & 0xFFFF, version_ls >> 16, version_ls & 0xFFFF]
    while len(parts) > 3 and parts[-1] == 0:
        parts.pop()
    return ".".join(str(p) for p in parts)


def game_build_id() -> str | None:
    """Steam 的 buildid（`appmanifest_1444480.acf`）—— Steam 每次更新都改它。

    appmanifest 在 `<库>/steamapps/` 下，即游戏目录的上两级；不是 Steam 装的
    目录（手拷的）读不到，返回 None。
    """
    acf = game_dir().parent.parent / _ACF_NAME
    try:
        text = acf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r'"buildid"\s*"(\d+)"', text)
    return match.group(1) if match else None


def game_fingerprint() -> str:
    """`compile.dll` 的内容指纹：sha256 前 12 位 + ":" + 字节数。

    写死的地址表绑定在某份二进制上；Steam 更新后这个串必变，就是「地址表该重取」的判据。
    ⚠ 这里的 `compile.dll` 也可能是转发层（perf_opt/cli.py 换过），
    所以它变了不等于 Steam 更新过 —— 那是 game_build_id() 的活。

    文件不存在抛 RuntimeError（那时比「变了没变」更该先解决「没有文件」）。
    """
    path = game_dll()
    if not path.is_file():
        raise RuntimeError(f"找不到 {path} —— 游戏没装全，或 TC_GAME_DIR 指错了")
    data = path.read_bytes()
    return f"{hashlib.sha256(data).hexdigest()[:12]}:{len(data)}"


# ---- 工具链落点 -------------------------------------------------------------
def llvm_root() -> pathlib.Path:
    """LLVM 资产目录（`include/llvm-c/Core.h` 所在的那一层）。

    顺序：`TC_LLVM_DIR` → `<仓库>/simplexc/.deps/llvm`（`fetch_llvm.py` 的默认落点）→
    `<仓库>/build/llvm`（旧的落点，只为还没迁过来的工作区留着）。
    判据是 `include/llvm-c/Core.h` 存在 —— 只认目录名会把半截解包当成可用。
    """
    candidates = []
    if (env := os.environ.get("TC_LLVM_DIR")):
        candidates.append(pathlib.Path(env))
    candidates.append(REPO_ROOT / "simplexc" / ".deps" / "llvm")
    candidates.append(REPO_ROOT / "build" / "llvm")

    for root in candidates:
        if (root / "include" / "llvm-c" / "Core.h").is_file():
            return root

    tried = "、".join(str(c) for c in candidates)
    raise RuntimeError(
        f"找不到 LLVM 资产（要 include/llvm-c/Core.h）：$TC_LLVM_DIR、"
        f"simplexc/.deps/llvm、build/llvm 三处都没有：{tried}。"
        f"取一份：python simplexc/tools/fetch_llvm.py"
    )


def tool(name: str) -> str:
    """PATH 里的可执行文件全路径（objdump / nm 这类不是仓库资产的工具）。

    clang **不走这里**：它必须来自 `llvm_root()` 的资产（不许 g++ / 系统 clang 顶替），
    否则编出来的代码生成质量与记录下来的那份对不上。
    """
    found = shutil.which(name)
    if found is None:
        raise RuntimeError(
            f"PATH 里找不到 {name}：装 binutils（MinGW-w64 / w64devkit 的 bin 目录），"
            f"或把它的目录加进 PATH。"
        )
    return found


# ---- 编译 hook 的产物落点 ----------------------------------------------------

@dataclass
class HookProduct:
    """一次编译 hook 落下的一对产物（`hook_src_NNN.si` + `hook_out_NNN.bin`）。

    `num` 是文件名里的编号段（保留原样，含前导零）；`stdlib` / `impl` 来自
    `manifest.tsv`，`impl != "ours"` 的产物是参考实现编出来的，复现不了自研产物。
    """

    num: str
    src: pathlib.Path
    out: pathlib.Path
    mtime: float
    stdlib: bool
    impl: str


_HOOK_OUT_NAME = re.compile(r"^hook_out_(\d+)\.bin$")
_HOOK_SRC_NAME = re.compile(r"^hook_src_(\d+)\.si$")


def _read_manifest(path: pathlib.Path) -> dict[str, dict[str, str]]:
    """`manifest.tsv` → {编号: {列名: 值}}。按表头映射列，行短了缺的列就没值。"""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}

    lines = [line for line in text.splitlines() if line.strip()]
    if not lines:
        return {}
    header = [name.strip() for name in lines[0].split("\t")]
    if "seq" not in header:
        return {}

    rows: dict[str, dict[str, str]] = {}
    for line in lines[1:]:
        cells = line.split("\t")
        row = {name: cells[i].strip() for i, name in enumerate(header) if i < len(cells)}
        num = row.get("seq")
        if num:
            rows[num] = row
    return rows


def latest_hook_product(hook_dir: pathlib.Path | None = None) -> HookProduct | None:
    """最近一次编译 hook 的产物对；没有可用产物返回 None。

    编号相同的 `hook_src_*.si` 与 `hook_out_*.bin` 才是一对，所以两个都扫（hook 可能
    只落下了一半）；「最近」按 mtime 比，同一秒落盘的按编号整数比大小 —— 编号是
    落盘顺序，mtime 的分辨率不够时它是唯一还带顺序的读数。

    `manifest.tsv` 里的 src/out 可能是改过名的文件，所以优先用 manifest 记的路径；
    缺那一行时退回同名配对，并按「自研 + 无 stdlib 前缀」算。
    """
    root = pathlib.Path(hook_dir) if hook_dir is not None else paths.hook_dir()
    if not root.is_dir():
        return None

    files: dict[str, dict[str, pathlib.Path]] = {}
    for entry in root.iterdir():
        if not entry.is_file():
            continue
        for kind, name in (("out", _HOOK_OUT_NAME), ("src", _HOOK_SRC_NAME)):
            match = name.match(entry.name)
            if match:
                files.setdefault(match.group(1), {})[kind] = entry

    manifest = _read_manifest(root / "manifest.tsv")
    products: list[HookProduct] = []
    for num, pair in files.items():
        if "src" not in pair or "out" not in pair:
            continue
        row = manifest.get(num, {})
        if row.get("impl", "ours") != "ours":
            continue
        src = root / row["src"] if row.get("src") else pair["src"]
        out = root / row["out"] if row.get("out") else pair["out"]
        if not (src.is_file() and out.is_file()):
            continue
        products.append(
            HookProduct(
                num=num,
                src=src,
                out=out,
                mtime=max(out.stat().st_mtime, src.stat().st_mtime),
                stdlib=row.get("stdlib", "").lower() == "on",
                impl=row.get("impl", "ours"),
            )
        )

    if not products:
        return None
    return max(products, key=lambda product: (product.mtime, int(product.num)))


if __name__ == "__main__":
    print(game_dir())
    print(game_exe())
    print(game_build_id())
    print(game_fingerprint())
    print(game_version())
    print(llvm_root())
