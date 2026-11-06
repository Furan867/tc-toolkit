"""仓库内路径常量与 opt.ini 的部署侧读写。"""
from __future__ import annotations

import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOOK = os.path.join(ROOT, "simplexc", "build", "compile_hook.dll")


def ensure_opt_ini() -> None:
    """补一份 `opt.ini` 模板。

    转发层读 `opt.ini` 的 `[opt] compiler=` 等键，读不到就用内置默认值，**不会自动建文件** ——
    所以新 clone 的仓库里这些开关是隐形的。部署时从模板补一份，让它们可见。
    """
    ini = os.path.join(ROOT, "opt.ini")
    example = os.path.join(ROOT, "opt.ini.example")
    if os.path.exists(ini) or not os.path.exists(example):
        return
    try:
        shutil.copy2(example, ini)
    except OSError as exc:
        print(f"⚠️ 没能生成 opt.ini（不影响部署）：{exc}", file=sys.stderr)
        return
    print("ℹ️ 已从模板生成 opt.ini（切换 compiler / stdlib / rewrite 时改它）")
