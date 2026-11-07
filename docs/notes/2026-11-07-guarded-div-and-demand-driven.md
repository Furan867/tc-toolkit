# guarded div 实验与按需求值讨论（2026-11-07）

状态: 有效

## 背景

- 目标电路：RV32IM。最新几份热点报告都是同一电路，除 `div`/`rem` 外的结论可横向看。
- 基线热点：`com_mod #518` 约 14.5–14.9%，`com_div #517` 约 2.1%。
- 讨论目标：在不改玩家电路的前提下，让“不被使用的纯计算”不执行；`div` 是第一个实验对象。

## 实验

临时在 `simplexc_compile()` 入口做 env-gated 文本替换：

- 只有 `TC_GUARDED_DIV=1` 时生效。
- 只替换 `mode_run` 的 `com_mod`/`com_div` 两段，用 `if (U1 vid704) != 0` 包住；`mode_refresh` 不动。
- 每个 `pattern` 只替换一次；找不到时打印 `[guarded-div] pattern not found`，原样编译。
- 未提交；实验结束后源码改动与临时源/脚本已删除，`simplexc/build/` 已重建为基线。
- 游戏内仍可能留着实验 DLL（部署时 3874154 B，基线 3872047 B）；游戏退出后需 `python tools/perf_opt/cli.py restore` 或重新部署基线。

验证：

- 机器码里 `div` 已位于条件分支之后；LLVM 仍把 `/` 和 `%` 合并成同一条 `div`。
- guarded 源编译产物与游戏内 guarded blob 逐字节一致（sha `eb92e4…`，入口 `0x5310`）；原始源基线为 `c14e23…`（入口 `0x5300`）。
- 注意：`simplexc_cli` 复制了一套自己的 pipeline，不经过 `simplexc_compile`；因此 `TC_GUARDED_DIV` 对 CLI 无效。热点工具用 CLI 编译时，必须喂已经 guard 好的源，否则 sha 不匹配，`locate` 失败。

热点结果（10 s，`2026-11-07_15-01-30_guarded_hook_src_002_t10`）：

- 10000 样本，bucket0 10000；主频 47.154 → 47.247 MHz，均值约 47.20 MHz。
- `com_mod` 177（2.0%），`com_div` 29（0.3%）。
- 当前热点变为 `switch_word` 30.5%、`switch_bit` 11.5%、`mux` 9.5%、`load_port` 9.3%、`custom` 7.4%；根代码 10.8%。
- 旧报告基线约 45.x MHz；当前不是同源基线（旧的 `476e…`/`8d50…` 是不同源码/版本）。因此只能把“约 +4%”当信号，不能当受控 A/B。

## 讨论结论

### 执行与 store

- `mode_run` 是顺序展开的直线求值；大多数元件结果是局部变量，编译到 IR 后成为 SSA 值。所有元件表达式都会进入指令流，除非编译器做 DCE。
- `store` 不是求值的开关。没有 store 的元件照样计算；有 store 的元件也不会因为 store 才计算。`mode_run` 只有 16 次 load、27 次 store，主要是输出缓存/RAM/键盘/设置；`mode_refresh` 的约 1221 个 store 属于另一个函数，不在热点路径。
- SSA 只是“每个值一处定义、使用关系显式”，不是优化，也不会自动跳过。guard 需要 SSA/def-use 来证明“所有使用都被某个条件覆盖”；在此之前，`div` 仍然无条件执行。

### 名称

不是死代码消除。结果仍然被 mux 使用，只是在部分路径上不需要。最接近的分类：

- 条件化纯计算 / 计算下沉（sinking into a guard）
- 部分死代码消除（partial dead code elimination）
- 惰性求值 / demand-driven evaluation
- 运行时语境：guarded specialization；纯计算不需要 deoptimization

### 层级

- `HIR pass`：在 `sema::Hir` 之后、`cg::Program` 之前；看表达式、控制流、def-use、纯/副作用、store/调用、源码 Span。
- `LLVM IR pass`：在 `prog.build()` 之后、`emit_object()` 之前；看 SSA/CFG，但后续 O2 可能重新 if-convert。
- 当前实验是文本替换，只适合验证，不适合产品化；文本替换会移动行号，破坏热点报告的行号映射。
- `HIR pass` **不需要电路拓扑**；它只需要 HIR 和可观察性规则。电路拓扑是图级按需求值才需要的。
- 图级按需求值需要从 `.si` 生成侧拿拓扑/元件/mux/状态信息；这是另一条路线，不是 HIR pass 的前提。

### 热点兼容

- 优化必须在 CLI 和游戏壳共用的同一条 pipeline 里，否则同一份源会编出不同 sha，热点 `locate` 直接失败。
- 优化应保留 HIR Span/行号；sidecar 基于最终产物生成；热点工具只消费产物，不反向依赖编译器。
- guard 是运行时跳过，不删除元件段，热点仍可按机器码归因；如果 DCE 真删元件，报告里缺元件是正常的。

### 按需求值

通用目标不是“删表达式”，而是从可观察根反向传播需求：

- 根：寄存器写、内存写、PC、输出端口、屏幕/探针、halt/错误、下一 tick 还要读的状态。
- 纯组合节点：只到达某个 mux 分支的，用该分支条件作为 guard；不流向任何根的，可以 DCE。
- 有状态、store、内存副作用、控制/错误检测相关节点不能随便跳。
- 跨 tick：当前值可能是下一 tick 的输入，不能只看当前函数内的使用。
- 对 RV32IM：load/store 指令下可跳过不写回、不影响控制/地址/存储的 ALU 结果；地址计算和 store data 通路仍在。是否“快很多”取决于剩余热点和指令组合，不能只看 ALU 类型占比。

需要的信息：

- 电路拓扑、mux/select 与控制关系（图级）；
- 纯/有状态划分、store/调用/错误语义；
- 可观察输出契约、跨 tick 状态；
- 运行时守卫分布/分支可预测性；
- 目标代价模型（计算成本 vs 分支/代码体积）。

## 下一步

- 实验性 guard 代码不保留；先不产品化。
- 若继续：先做 HIR pass 原型，验证局部按需求值的收益和边界；同时把 CLI/壳的 pipeline 收敛为一份。
- 若要做全 CPU 的“按需计算”：再在 `.si` 生成侧拿拓扑/可观察性信息，做图级需求传播；不要指望 HIR pass 从压平源码恢复完整电路。
