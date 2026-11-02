#!python3
"""difftrace 的 C SDK 依赖：一条命令备齐 Unicorn 与 Capstone 的头文件与导入库。

为什么要有这个脚本：两个 SDK 只存在于 PyPI wheel 和已装的 site-packages 里，都不进 git；
README 那段手工流程是七条命令加写死的解释器绝对路径，换机器或换 Python 版本就失效。
这里把「下载/解包/gendef/dlltool/打印编译命令」固定下来，并把版本与 dll 的 sha256 记进
`deps.json`，下次跑就能判断「已存在且校验通过」而不是盲目重建。

产物（默认 `<repo>/simplexc/.deps/difftrace-deps/`，`.deps/` 已被 simplexc/.gitignore 挡掉）::

    unicorn/include/unicorn/*.h     wheel 里的 unicorn/include
    unicorn/lib/unicorn.dll         wheel 里的 unicorn/lib/*.dll（wheel 里的 unicorn.lib 不要）
    unicorn/lib/unicorn.def         gendef 生成
    unicorn/lib/libunicorn.a        dlltool 生成，g++ 链接用
    capstone/include/capstone/*.h   site-packages/capstone/include
    capstone/lib/capstone.dll       site-packages/capstone/lib
    capstone/lib/capstone.def       gendef 生成
    capstone/lib/libcapstone.a      dlltool 生成
    wheel/                          下载的 wheel 缓存（`--offline` 从这里取）
    deps.json                       校验用：版本 + dll 的 sha256

用法
----
    python simplexc/tools/difftrace/fetch_deps.py               # 默认目录
    python simplexc/tools/difftrace/fetch_deps.py --offline     # 只用 wheel/ 里已有的 wheel
    python simplexc/tools/difftrace/fetch_deps.py --force       # 已存在的全部重建
    python simplexc/tools/difftrace/fetch_deps.py --dest <dir>

成功跑完会打印「怎么编 difftrace」与运行期需要的 dll。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

REPO = Path(__file__).resolve().parents[3]
DEFAULT_DEST = REPO / "simplexc" / ".deps" / "difftrace-deps"
UNICORN_WHEEL_INSTRUCTIONS = (
    "  有网时先下载（或在浏览器里打开 https://pypi.org/simple/unicorn/ 取 win_amd64 的 wheel）:\n"
    "    python -m pip download unicorn --no-deps -d {wheel_dir}\n"
    "  把 unicorn-*-win_amd64.whl 放进该目录后重跑 `--offline`。"
)


@dataclass(frozen=True)
class Layout:
    """`--dest` 下的固定布局；所有路径的唯一定义处。"""

    dest: Path
    wheel: Path
    unicorn_include: Path
    unicorn_lib: Path
    capstone_include: Path
    capstone_lib: Path
    manifest: Path

    @classmethod
    def at(cls, dest: Path) -> Layout:
        return cls(
            dest=dest,
            wheel=dest / "wheel",
            unicorn_include=dest / "unicorn" / "include",
            unicorn_lib=dest / "unicorn" / "lib",
            capstone_include=dest / "capstone" / "include",
            capstone_lib=dest / "capstone" / "lib",
            manifest=dest / "deps.json",
        )

    def create_dirs(self) -> None:
        for directory in (
            self.wheel,
            self.unicorn_include,
            self.unicorn_lib,
            self.capstone_include,
            self.capstone_lib,
        ):
            directory.mkdir(parents=True, exist_ok=True)


def say(message: str) -> None:
    print(message, flush=True)


def die(message: str) -> NoReturn:
    print(f"\n错误: {message}", file=sys.stderr, flush=True)
    raise SystemExit(1)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def shown_path(path: Path) -> str:
    """相对当前目录的 posix 路径 —— 复制出来的命令要能直接在当前 shell 里跑。"""
    return Path(os.path.relpath(str(Path(path).resolve()), os.getcwd())).as_posix()


def shown_source(path: Path) -> str:
    """只用于给人看的出处：跑到当前目录之外时给绝对路径，不堆一长串 `../`。"""
    relative = shown_path(path)
    return Path(path).resolve().as_posix() if relative.startswith("..") else relative


def tail(text: str, lines: int = 12) -> str:
    rows = [row for row in text.strip().splitlines() if row.strip()]
    return "\n".join(rows[-lines:])


def run_step(cmd: list[str], cwd: Path, what: str) -> None:
    result = subprocess.run(
        cmd, cwd=str(cwd), capture_output=True, text=True, errors="replace"
    )
    if result.returncode != 0:
        detail = "\n".join(x for x in (tail(result.stdout), tail(result.stderr)) if x)
        die(
            f"{what} 失败（退出码 {result.returncode}）\n"
            f"  命令: {' '.join(cmd)}\n"
            f"  目录: {cwd}\n{detail}"
        )


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    die(
        f"找不到 {name} —— 它随 GCC 14+ 的 mingw-w64 一起装（gendef/dlltool 在 bin/ 里）。\n"
        '  把那个 bin 目录加进 PATH 再跑：export PATH="<GCC 14+ 所在目录>/bin:$PATH"'
    )


def read_manifest(layout: Layout) -> dict:
    try:
        data = json.loads(layout.manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def write_manifest(layout: Layout, manifest: dict) -> None:
    layout.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def wheel_version(wheel_name: str) -> str:
    parts = wheel_name.split("-")
    return parts[1] if len(parts) > 1 else "?"


def build_import_lib(lib_dir: Path, dll_name: str, force: bool) -> None:
    """在 `lib_dir` 里为该 dll 生成 `<stem>.def` 与 `lib<stem>.a`（g++ 只认后者）。"""
    stem = Path(dll_name).stem
    def_path = lib_dir / f"{stem}.def"
    archive = lib_dir / f"lib{stem}.a"
    if force:
        for stale in (def_path, archive):
            stale.unlink(missing_ok=True)
    if def_path.is_file() and def_path.stat().st_size > 0:
        say(f"  {def_path.name} 已存在，跳过 gendef")
    else:
        run_step([find_tool("gendef"), dll_name], lib_dir, f"gendef {dll_name}")
        say(f"  gendef {dll_name} → {def_path.name}")
    if archive.is_file() and archive.stat().st_size > 0:
        say(f"  {archive.name} 已存在，跳过 dlltool")
    else:
        run_step(
            [find_tool("dlltool"), "-d", def_path.name, "-l", archive.name],
            lib_dir,
            f"dlltool → {archive.name}",
        )
        say(f"  dlltool -d {def_path.name} -l {archive.name}")


def resolve_unicorn_wheel(layout: Layout, offline: bool, force: bool) -> Path:
    if not force:
        cached = sorted(layout.wheel.glob("unicorn-*.whl"))
        if cached:
            say(f"  wheel 已缓存，跳过下载: {shown_path(cached[-1])}")
            return cached[-1]
    wheel_dir = shown_path(layout.wheel)
    if offline:
        die(
            f"--offline 但 {wheel_dir} 里没有 unicorn-*.whl。\n"
            + UNICORN_WHEEL_INSTRUCTIONS.format(wheel_dir=wheel_dir)
        )
    for stale in layout.wheel.glob("unicorn-*.whl"):
        stale.unlink()
    say(f"  下载 wheel → {wheel_dir}（需要网络）")
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pip",
            "download",
            "unicorn",
            "--no-deps",
            "-d",
            str(layout.wheel),
        ],
        capture_output=True,
        text=True,
        errors="replace",
    )
    wheels = sorted(layout.wheel.glob("unicorn-*.whl"))
    if result.returncode != 0 or not wheels:
        detail = "\n".join(
            x for x in (tail(result.stdout, 8), tail(result.stderr, 12)) if x
        )
        die(
            "pip download unicorn 失败（需要网络）。pip 输出：\n"
            f"{detail}\n"
            "  镜像/代理不通时走离线路线：\n"
            "    1) 在能上网的机器上 `python -m pip download unicorn --no-deps -d wheel`\n"
            f"    2) 把 unicorn-*-win_amd64.whl 复制到 {wheel_dir}\n"
            "    3) 重跑 `python simplexc/tools/difftrace/fetch_deps.py --offline`"
        )
    say(f"  已下载: {wheels[-1].name}")
    return wheels[-1]


def extract_unicorn(wheel: Path, layout: Layout) -> int:
    """只展开 include/ 与 lib/*.dll —— wheel 里的 49 MB MSVC `unicorn.lib` 对 g++ 无用。"""
    written = 0
    with zipfile.ZipFile(wheel) as archive:
        for name in archive.namelist():
            if name.endswith("/"):
                continue
            if not (
                name.startswith("unicorn/include/")
                or (name.startswith("unicorn/lib/") and name.endswith(".dll"))
            ):
                continue
            target = layout.dest / "unicorn" / Path(name).relative_to("unicorn")
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(name) as source, target.open("wb") as sink:
                shutil.copyfileobj(source, sink)
            written += 1
    dll = layout.unicorn_lib / "unicorn.dll"
    header = layout.unicorn_include / "unicorn" / "unicorn.h"
    if not dll.is_file() or not header.is_file():
        die(
            f"{wheel.name} 里没有 unicorn/include/unicorn/unicorn.h 或 unicorn/lib/*.dll —— "
            "换一个 unicorn 版本（2.x 的 win wheel 带 C SDK）。"
        )
    return written


def unicorn_ready(layout: Layout, record: object) -> bool:
    if not isinstance(record, dict) or not record.get("dll_sha256"):
        return False
    dll = layout.unicorn_lib / "unicorn.dll"
    archive = layout.unicorn_lib / "libunicorn.a"
    if not (layout.unicorn_include / "unicorn" / "unicorn.h").is_file():
        return False
    if not dll.is_file() or not archive.is_file() or archive.stat().st_size == 0:
        return False
    return sha256_file(dll) == record["dll_sha256"]


def ensure_unicorn(
    layout: Layout, manifest: dict, offline: bool, force: bool
) -> bool:
    if not force and unicorn_ready(layout, manifest.get("unicorn")):
        say("[1/2] unicorn: 已存在且校验通过，跳过（--force 重建）")
        return False
    say("[1/2] unicorn: 准备 C SDK")
    wheel = resolve_unicorn_wheel(layout, offline=offline, force=force)
    written = extract_unicorn(wheel, layout)
    say(
        f"  解包 {written} 个文件 → {shown_path(layout.unicorn_include)}"
        f" + {shown_path(layout.unicorn_lib)}"
    )
    build_import_lib(layout.unicorn_lib, "unicorn.dll", force)
    manifest["unicorn"] = {
        "version": wheel_version(wheel.name),
        "wheel": wheel.name,
        "dll_sha256": sha256_file(layout.unicorn_lib / "unicorn.dll"),
    }
    write_manifest(layout, manifest)
    return True


def locate_capstone() -> tuple[Path, Path, Path, str]:
    try:
        import capstone
    except ImportError:
        die(
            "capstone 未安装 —— difftrace 用它反汇编，先 `pip install capstone` 再跑本脚本。"
        )
    module_file = getattr(capstone, "__file__", None)
    if not module_file:
        die("capstone 是命名空间包，找不到它的安装目录 —— 换 `pip install capstone`。")
    root = Path(module_file).resolve().parent
    include = root / "include"
    lib = root / "lib"
    if not (include / "capstone" / "capstone.h").is_file() or not (
        lib / "capstone.dll"
    ).is_file():
        die(
            f"capstone 装在 {root}，但里面没有 include/capstone/capstone.h 或 "
            f"lib/capstone.dll。\n"
            "  这个 wheel 不带 C SDK；换带 SDK 的版本（5.x 的 win wheel 带）：\n"
            "    python -m pip install --force-reinstall capstone"
        )
    return root, include, lib, getattr(capstone, "__version__", "?")


def capstone_ready(layout: Layout, record: object, version: str) -> bool:
    if not isinstance(record, dict) or record.get("version") != version:
        return False
    dll = layout.capstone_lib / "capstone.dll"
    archive = layout.capstone_lib / "libcapstone.a"
    if not (layout.capstone_include / "capstone" / "capstone.h").is_file():
        return False
    if not dll.is_file() or not archive.is_file() or archive.stat().st_size == 0:
        return False
    return sha256_file(dll) == record.get("dll_sha256")


def ensure_capstone(layout: Layout, manifest: dict, force: bool) -> bool:
    root, include, lib, version = locate_capstone()
    if not force and capstone_ready(layout, manifest.get("capstone"), version):
        say(f"[2/2] capstone {version}: 已存在且校验通过，跳过（--force 重建）")
        return False
    say(f"[2/2] capstone {version}: 复制 C SDK ← {shown_source(root)}")
    if force:
        shutil.rmtree(layout.capstone_include, ignore_errors=True)
        shutil.rmtree(layout.capstone_lib, ignore_errors=True)
    layout.capstone_include.mkdir(parents=True, exist_ok=True)
    layout.capstone_lib.mkdir(parents=True, exist_ok=True)
    shutil.copytree(include, layout.capstone_include, dirs_exist_ok=True)
    shutil.copy2(lib / "capstone.dll", layout.capstone_lib / "capstone.dll")
    say(
        f"  头文件 → {shown_path(layout.capstone_include)}"
        f"，capstone.dll → {shown_path(layout.capstone_lib)}"
    )
    build_import_lib(layout.capstone_lib, "capstone.dll", force)
    manifest["capstone"] = {
        "version": version,
        "source": str(root),
        "dll_sha256": sha256_file(layout.capstone_lib / "capstone.dll"),
    }
    write_manifest(layout, manifest)
    return True


def print_compile_recipe(layout: Layout) -> None:
    """备齐 SDK 后怎么编 `difftrace` —— 指向 CMake 目标，不再打印一份命令行。"""
    say("")
    say("编译（构建定义只有 CMakeLists.txt 一处，flags 不用手敲）:")
    say("  python simplexc/tools/build.py difftrace")
    say("  # 等价：cmake --build build/cmake --target difftrace（无 cache 先 cd simplexc && cmake --preset mingw）")
    say("")
    say("运行前 difftrace.exe 旁边要有这两个 dll（构建时已拷到 simplexc/build/；或用 PATH）:")
    say(f"  {shown_path(layout.unicorn_lib / 'unicorn.dll')}")
    say(f"  {shown_path(layout.capstone_lib / 'capstone.dll')}")


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="备齐 difftrace 的 Unicorn / Capstone C SDK，并指明怎么编 difftrace。"
    )
    parser.add_argument(
        "--dest",
        default=str(DEFAULT_DEST),
        help="依赖装到这里（默认 <repo>/simplexc/.deps/difftrace-deps）",
    )
    parser.add_argument(
        "--force", action="store_true", help="已存在的产物全部重建（在线时重新下载 wheel）"
    )
    parser.add_argument(
        "--offline",
        action="store_true",
        help="只用 <dest>/wheel 里已有的 wheel，不联网",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    layout = Layout.at(Path(args.dest).expanduser().resolve())
    layout.create_dirs()
    say(f"difftrace 依赖 → {shown_path(layout.dest)}")
    manifest = read_manifest(layout)
    changed = ensure_unicorn(
        layout, manifest, offline=args.offline, force=args.force
    )
    changed = ensure_capstone(layout, manifest, force=args.force) or changed
    say("全部就绪。" if changed else "全部就绪（无改动）。")
    print_compile_recipe(layout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
