# hotspot/tools

热点分析的离线辅助脚本。全部按**脚本式**运行：`python hotspot/tools/<x>.py ...`，不提供包入口。

| 脚本 | 作用 |
|---|---|
| `sidecar.py` | 读取/校验 sidecar、offsets，公共结构与异常 |
| `functions.py` | 从 `.debug_info` / `.debug_ranges` 抽函数区间 |
| `make_sidecar.py` | 对象行表 + `el.tsv` + `offsets.json` + blob → `sidecar.json` |
| `g2_check.py` | sidecar / objdump 行表 / 反汇编行注三方对齐抽查 |
| `acceptance_sidecar.py` | W3 自造 DWARF 样本，跑 sidecar 与 G2 的正负例 |

`run.py` 会调 `make_sidecar.py`；其余脚本用于验收和排查，不进入采样热路径。

TOML 迁移脚本已经删除：配置与 run 记录统一为 INI，不再保留 TOML 兼容。
