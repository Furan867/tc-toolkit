/* prog_empty.cc —— spike 用的「空程序」：程序根，什么都不做就返回。
 *
 * 它在链路里的位置与将来 LLVM 生成的模块**完全一样**：装配器只认符号名 `tc_main`，
 * 不在乎是 g++ 还是 LLVM 编出来的。所以先用它验证「装配器 + shim + runtime + blob 在进程里真能跑」，
 * 再接 LLVM —— 这样出问题时能立刻分清是装配链还是 LLVM 那一侧。
 */
extern "C" void tc_main(void) {
}
