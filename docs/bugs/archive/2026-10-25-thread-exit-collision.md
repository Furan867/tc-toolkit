# `thread_exit()` 撞出 `thread_exit.1`

- 状态：**未修**
- 影响：前缀里的 `def thread_exit()`（`stdlib_prefix.si`）。98 份语料 **0 次调用**，所以目前
  不挡任何验收；但它是**符号碰撞**，一旦有语料调用就会链接到错的那个。
- 现象：编「前缀 + 调用 `thread_exit()` 的片段」时，产物里出现两个符号：
  前缀的 `thread_exit` 与隐式收尾生成的 `thread_exit.1`。
- 出处：`codegen.cc:2466` 一带的**隐式收尾**（程序正常结束时要调的那段）与前缀的
  `def thread_exit` 同名，撞了之后装配器给后者改名成 `.1`。
- 为什么现在才露出来：以前 `kernel32.'ExitThread'` 那一步在 sema 就报「未支持的外部符号
  kernel32」，根本走不到 codegen；`cca6f00` 把 `kernel32` 接通后才暴露。

## 判据

产物里只有一个 `thread_exit`，且它就是前缀里 `def thread_exit` 的那个体（隐式收尾要调它，
不该另造一个同名兄弟）。

## 下一步

先确认参考怎么做：它是「隐式收尾直接 call 前缀的 `thread_exit`」还是「隐式收尾自己是一段
内联代码、与用户的 `thread_exit` 无关」。这决定改法是「隐式收尾改成引用前缀符号」还是
「隐式收尾换个不会碰撞的内部名」。**别先改代码**——两种改法的产物形态不同，得先测。

## 复现

```bash
printf 'def thread_exit() None { }\nrun_sim: while true { thread_exit() break }\n' > /tmp/te.si
build/simplexc_cli.exe --src /tmp/te.si --out /tmp/te.bin   # 前缀要拼在前面
# 看 <blob>.stubs 与符号表里是否同时有 thread_exit 与 thread_exit.1
```
