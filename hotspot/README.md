# hotspot

热点分析产品：把游戏 JIT 电路的时间归因到元件、源码行、函数，产出 `report.md` / `report.html`。

## 入口

```bash
python hotspot/run.py                 # 取 work/hook 最近一次 hook 产物
python hotspot/run.py --src x.si      # 指定源；配合 --stdlib 复现拼接
python hotspot/run.py clean           # 清 sidecar 缓存与 work/tmp/hotspot_run
```

`run.py` 负责：preflight → 编译 `simplexc_cli` → sidecar → 采样 → 报表。采样用
`hotspot/consumer/build/hotspot-consumer.exe`（独立 CMake），必须先构建。

## 目录

| 路径 | 职责 |
|---|---|
| `run.py` | 用户入口与四阶段编排 |
| `analysis/` | 归因、表、`report.md` / `report.html` / `index.html` |
| `tools/` | sidecar 生成、G2 抽查、W3 验收脚本（脚本式，见该目录 README） |
| `consumer/` | C++ 采样器：suspend / ETW 后端、定位、run 目录 |
| `config.example.ini` | 配置模板；`config.ini` 是本机运行配置，不进 git |
| `workspace/runs/` | 每次采样的产物与报告；`runs/html/` 是集中 HTML 入口 |
| `workspace/cache/sidecar/` | 按 blob sha + 行偏移缓存 sidecar |
| `tests/` | `run.py` 的单测；analysis 的单测在 `analysis/test_report.py` |

## 构建

```bash
python simplexc/tools/build.py simplexc_cli          # run.py 需要，默认 all 不含它
cmake -S hotspot/consumer -B hotspot/consumer/build -G Ninja
cmake --build hotspot/consumer/build
```

`run.py` 发现 `simplexc_cli` 缺失时会尝试自动构建；consumer 缺失只报错，不自动建。

## 读报告

- `workspace/index.html`：全部 run 的索引。
- `workspace/runs/html/<run>.html`：集中 HTML；run 目录里保留同份副本。
- `run.ini` 是机器可读元数据；`report.md` 是文本归档。
