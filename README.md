# Turing Complete 反编译成果

> 游戏：Turing Complete 0.1059 Beta（Steam）
> 反编译时间：2026-08-17
> 反编译对象：`F:\opt\SteamLibrary\steamapps\common\Turing Complete\`

## 1. 技术架构分析

Turing Complete **不是 GDScript 游戏**。它的真实架构是：

| 二进制 | 大小 | 角色 |
|---|---|---|
| `Turing Complete.exe` | 15 MB | **游戏本体**，Nim 2.x 编译（含完整 COFF 调试符号） |
| `game_engine.dll` | 2 MB | **Godot 3.4.2 stable (custom_build)** 引擎，嵌入式宿主模式 |
| `compile.dll` | 2 MB | **自研 Nim 编译器后端**（玩家电路 → x86-64/AArch64 机器码 + JIT） |
| `soft_oal.dll` | 2 MB | OpenAL 音频 |
| `steam_api64.dll` | 0.3 MB | Steam 集成 |

- 引擎版本来自 [debug.log](recovered/res/../debug.log)：`Godot Engine v3.4.2.stable.custom_build`
- **没有 .pck、没有 .gdc、没有 .tscn**：游戏数据直接散放在 exe 旁（`asset/`、`campaign/`、`translations/`），UI 全部由 Nim 代码运行时构建（无场景文件），游戏逻辑全部编译为原生机器码
- exe 携带 **58428 个 COFF 符号 + 1.6 MB 字符串表**（未剥离的发布构建），这是本次反编译的主要突破口

## 2. 反编译方法

1. 解析 PE 头 → 定位 COFF 符号表（offset `0xC97E00`）与字符串表
2. 用 binutils `nm` 读取全部符号（T/t 函数、D/d/R/r/B/b 数据）
3. 识别符号为 **Nim 2.x mangling**（`procname__moduleZpath_uID`，`95`=`_` 转义），编写 demangler 恢复函数名与模块路径
4. 按模块归组生成工程骨架树（每个 .nim 模块一个 stub 文件，含函数/变量清单与地址）
5. 从二进制字符串提取 UI 类名清单

## 3. 产出目录

```
turing-complete/
├── README.md                  ← 本文件
├── recovered/
│   ├── src/                   ← 游戏本体模块骨架（383 个 stub，8815 函数 / 705 变量）
│   │   ├── main.nim.stub         游戏入口（build_game_scene、main、update_* 主循环）
│   │   ├── globals.nim.stub      全局路径/日志
│   │   ├── presenter/            渲染/UI 层（约 100 模块）
│   │   │   ├── board_ui/         电路板 UI（popup: IDE、文件浏览器、十六进制编辑器…）
│   │   │   ├── renderer/         渲染器（multi_mesh、shader、texture、steam…）
│   │   │   ├── main_menu_ui/     主菜单（hub、options、credits…）
│   │   │   ├── user_input/       输入处理（board_io/action: 布线、选择、快捷键…）
│   │   │   ├── imgui/ markup/ rtree/ audio/ …
│   │   ├── model/                数据/逻辑层
│   │   │   ├── board/            电路板模型（schematics、memory_manager…）
│   │   │   ├── simulation/       仿真器（preorder、code_gen、jit、controller…）
│   │   │   ├── isa_spec/         ISA 规格（assemble、parse、types、expressions…）
│   │   │   ├── save_monger/      存档系统（versions v0–v15、supersnappy 压缩）
│   │   │   ├── exports/verilog/  Verilog 导出
│   │   │   ├── networking/ campaigns/ scores/ translations/ …
│   │   ├── std/ pure/ system/    Nim 标准库模块（引用到的）
│   │   ├── crt/                  MinGW CRT 符号（非游戏代码）
│   │   └── _TREE.txt             完整模块清单
│   ├── compiler-src/          ← compile.dll 编译器骨架（66 模块，1989 函数）
│   │   └── OO/
│   │       ├── passes/
│   │       │   ├── front_end/    词法/语法/作用域/约束（tokens、scope、constraint…）
│   │       │   ├── middle/       中端优化（lower、suffix_tree、dependency DAG…）
│   │       │   └── back_end/     寄存器分配、栈帧、emit
│   │       ├── targets/
│   │       │   ├── x86_64/       x86_64_assembler（指令编码）
│   │       │   └── aarch64/      base_instructions
│   │       └── passes/jit/jit    JIT 执行
│   └── res/                   ← 游戏资源（原样复制，257 MB）
│       ├── asset/                图片、音频、字体、shader（.frag/.vert 为明文 GLSL）
│       ├── campaign/             全部关卡数据（.tc 关卡、.data 电路、.si 测试）
│       └── translations/         25 种语言翻译（明文）
├── work/                      ← 中间产物（原始符号表、字符串提取…）
│   ├── symbols_raw.txt        nm 完整符号输出（54522 行）
│   ├── functions_by_unit.tsv  符号归属
│   ├── compile_units.txt      编译单元清单（364 个，.nim 模块原始名）
│   ├── ui_class_names.txt     101 个 UI 类名
│   └── unmatched_symbols.txt  未 demangle 的符号
└── tools/                     ← 反编译工具与脚本
    ├── pe_overlay.py          PE/COFF 解析
    ├── coff_symbols.py        符号表解析
    ├── nim_demangle.py        Nim mangling demangler + 骨架树生成
    ├── demangle_dll.py        compile.dll 分析
    └── gdre/                  GDRETools v2.6.4（Godot RE Tools，本次未用到数据包）
```

## 4. 关键发现

- **开发栈**：Nim（游戏逻辑 + 自研编译器）+ Godot 3.4 引擎（窗口/渲染/输入），UI 纯代码构建
- **无源码泄露风险的文件**：发布 exe 保留了完整调试符号（未 strip），可直接恢复整个模块结构和函数级骨架
- `main.nim` 主循环结构：`build_game_scene` → `main` → `update_static` / `update_dynamic` / `update_stream` / `render_component_snapshots`
- UI 类名清单（101 个）与模块树交叉印证（如 `presenter/board_ui/popup/ide.nim` ↔ `##Ide`），其中 `##BotomPanel` 是作者源码中的原始拼写错误
- 关卡格式：`.tc`（关卡定义）、`.data`（电路/提示）、`.si`（测试用例），均为二进制；`.assembly` 为玩家存档中的汇编程序

## 5. 局限性

- **Nim 函数体（源码逻辑）无法从机器码恢复**——仅恢复名称、签名骨架与模块结构；如需逻辑级还原需对 exe 做反汇编分析（符号表已就绪，可辅助 IDA/Ghidra）
- 资源 `asset/` 中图片为 Godot 导入后格式，非原始工程素材
- 存档目录 `godot/app_userdata`（用户数据）未包含在输出中

## 6. 复现步骤

```bash
# 1. 提取符号（需 binutils nm）
nm "Turing Complete.exe" > work/symbols_raw.txt

# 2. 重建模块骨架树
python tools/nim_demangle.py

# 3. compile.dll 编译器骨架
python tools/demangle_dll.py
```
