# sema 的 golden

`hir_sample.dump.txt` 是 `sema/hir_selftest.cc` 里 `sample()` 那棵树的 dump，`hir_selftest.exe`
拿它**逐字节**比。那棵树要表达的源码（行号就是 dump 里 `span=` 的来历）：

```text
  1  main(n: U64) U64 {
  2    var x = 7
  3    if x < 10 {
  4      var y = 42
  5    }
  6    return x + n
  7  }
  8
  9  f[T](k: T) T { return k }
```

核对结论：节点顺序 = `VarDecl(x)` → `If(Binary lt, Block[VarDecl(y)])` → `Return(Binary add)`；
`ty` 只有比较是 `Bool`、其余 `U64`、语句行无 `ty`；`Const` 三个位模式 `0x7` / `0xa` / `0x2a`；
`op=lt` / `op=add`；`span` 行号与上面清单逐行相符；`name` 是产物符号名（`main`、实例 `f$u64`），
`funcs[0]` 的 key 兼符号名按决策 7 写成 `<函数名>$<实参规范化串>`。

重新生成（仓库根，**别手抄**；改 dump 字段形状或 `sample()` 之后必须重跑这一行）：

```bash
build/hir_selftest.exe --dump-only > simplexc/sema/testdata/hir_sample.dump.txt
```

文件里是 CRLF（Windows 下 stdout 是文本模式），而 dump 只出 LF；`hir_selftest.cc` 读进来时
把 CRLF 折成 LF，所以 `core.autocrlf=true` 检出成什么样都不影响判据。
