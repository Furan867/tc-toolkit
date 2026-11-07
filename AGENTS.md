# tc-toolkit

Turing Complete 的逆向工程工具集：把游戏编译出的电路机器码拿下来做性能分析（主频测量、组件级热点归因），并维护一条用自研编译器替代 `compile.dll` 的路线。

## 目录

| 目录 | 依赖谁 | 放什么 |
|---|---|---|
| `libs/` | 标准库 | 所有层都会用的东西：游戏环境发现、Win32 内存原语、终端着色、`hook.log`、`work/` 路径 |
| `services/` | `libs` | 两个以上产品都要的能力，例如 `simctl` |
| `native/` | Windows API；`backend/` 另需 simplexc 的 C 边界 | C 侧：`forward/` 机制、`compile/` compile 服务、`backend/` 编译器实现 |
| `tools/` | 上面三层 | 用户敲的命令，一个文件一个入口；细分见 `tools/README.md` |
| `hotspot/`、`simplexc/` | 同上 | 两个独立产品 |

## 依赖方向

**只能指向更稳定的一层。** 具体条款都是它的推论：

- 命令 → 服务 → 库；产品之间不互相 import。
- 实现被两个入口用到，就不该留在 `tools/`——它会比入口活得久。
- `native/forward/` 不认识 `compile`、`simplexc`、`opt.ini`、`work/`——机制要比业务活得久。

## 找东西

- 目录地图 `docs/map.md`；每层细则看该目录的 `README.md`
- 工具与坑 `TOOLS.md`、`docs/pitfalls.md`
- 当前状态 `docs/status.md`
