"""Organize raw exe_strings.txt into categorized, deduplicated, sorted files."""
import os
import re
import shutil

WORK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work")
SRC = os.path.join(WORK, "exe_strings.txt")
OUT = os.path.join(WORK, "strings")

NOISE_PREFIXES = (
    ".text", ".rdata", ".pdata", ".xdata", ".bss", ".data", ".CRT",
    ".tls", ".rsrc", ".reloc", ".idata", ".debug", ".eh_frame",
)
NOISE_ANYWHERE = (".refptr.", "$R", "$D", "@@")

def is_noise(s: str) -> bool:
    if not s or len(s) < 4:
        return True
    if not re.search(r"[A-Za-z]", s):   # 不含字母的纯符号/数字串
        return True
    if s.startswith(NOISE_PREFIXES):
        return True
    if any(m in s for m in NOISE_ANYWHERE):
        return True
    if re.fullmatch(r"[0-9a-fA-Fx_:\s]+", s):  # 纯地址/hex 串
        return True
    return False

cats = {
    "nim_symbols": [],        # Nim mangled 符号 (proc__moduleZpath_uID)
    "ui_classes": [],         # ## 前缀 = UI 类名
    "components": [],         # com_* 组件名
    "translation_keys": [],   # 31337_xxx 翻译键
    "paths": [],              # 路径类字符串
    "messages": [],           # 人类可读文本（句子/错误消息）
    "other": [],              # 其他有效字符串
}

with open(SRC, encoding="utf-8", errors="replace") as f:
    for line in f:
        s = line.strip()
        if is_noise(s):
            continue
        if "__" in s and re.search(r"(Z[a-z0-9_]*|_u\d+$)", s):
            cats["nim_symbols"].append(s)
        elif s.startswith("##"):
            cats["ui_classes"].append(s)
        elif s.startswith("com_"):
            cats["components"].append(s)
        elif re.fullmatch(r"\d{10,}_\d+", s):  # 翻译键（关卡文件里是 31337_xxx 形式）
            cats["translation_keys"].append(s)
        elif ("\\" in s or "/" in s) and re.search(r"(\.\w{2,4})|(asset|campaign|translations|presenter|model)", s):
            cats["paths"].append(s)
        elif len(s) >= 12 and re.search(r"[A-Za-z]{3,} [A-Za-z]{2,}", s):
            cats["messages"].append(s)
        elif len(s) >= 9:
            cats["other"].append(s)

os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "all_dedup.txt"), "w", encoding="utf-8") as f:
    all_sorted = sorted(set(sum(cats.values(), [])))
    f.write("\n".join(all_sorted) + "\n")

stats = {}
for name, items in cats.items():
    uniq = sorted(set(items))
    stats[name] = len(uniq)
    with open(os.path.join(OUT, f"{name}.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(uniq) + "\n")

print("分类统计（去重后）:")
for name, cnt in sorted(stats.items(), key=lambda x: -x[1]):
    print(f"  {name:20s} {cnt:6d}")

# 翻译键不在 exe 中，从关卡文件提取（键格式: 数字ID_数字ID）
CAMPAIGN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "recovered", "res", "campaign")
keys = set()
for root, _, files in os.walk(CAMPAIGN):
    for fn in files:
        if fn in ("circuit.data", "hint_solution.data", "hint_0.data") or fn.endswith(".png"):
            continue
        p = os.path.join(root, fn)
        try:
            with open(p, encoding="utf-8", errors="ignore") as f:
                text = f.read()
        except Exception:
            continue
        keys.update(re.findall(r"\d{4,}_\d+", text))
        keys.update(re.findall(r"(?<![\d_])\d{10,}(?!\d)", text))  # 纯数字 ID
with open(os.path.join(OUT, "translation_keys.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(sorted(keys)) + "\n")
print(f"  {'translation_keys (关卡文件)':20s} {len(keys):6d}")

# 原始文件已无保留价值（可从 exe 随时重提），替换为分类目录
os.remove(SRC)
print(f"\n已删除原始 exe_strings.txt，分类结果在 {OUT}")
