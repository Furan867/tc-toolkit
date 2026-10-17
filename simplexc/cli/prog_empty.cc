/* prog_empty.cc —— spike 用的「空程序」：程序根，留一条可观测的痕迹就返回。
 *
 * 它在链路里的位置与将来 LLVM 生成的模块**完全一样**：装配器只认符号名 `tc_main`，
 * 不在乎是 g++ 还是 LLVM 编出来的。所以先用它验证「装配器 + shim + runtime + blob 真能跑」，
 * 再接 LLVM —— 这样出问题时能立刻分清是装配链还是 LLVM 那一侧。
 *
 * `tc_spike_mark` 往固定文件追加一行：**游戏内验证靠它**。没有这一行就只能靠
 * 「游戏卡住了」这种间接证据 —— 那不可靠（分不清是没跑到、还是跑了但代码本身没作用）。
 */
extern "C" unsigned long long tc_spike_mark(unsigned long long tag);

extern "C" void tc_main(void) {
    tc_spike_mark(0x5A17);      /* 0x5A17 = “spike 跑到这里了”，游戏内与进程内共用同一个值 */
}
