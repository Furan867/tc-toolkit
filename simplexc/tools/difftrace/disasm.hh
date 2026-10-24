// 反汇编：x86-64 / Intel 语法，按偏移取指令，给分叉点定位用
// （设计 `docs/specs/2026-10-25-difftrace-design.md` §2 L2「分叉点 → 反汇编」的取指令那一步）。
//
// 解码器只有 Capstone 一个来源，不自己判架构：产物 triple 是 `x86_64-pc-windows-gnu`
// （设计 §4 K3），所以 `CS_ARCH_X86` + `CS_MODE_64` 是确定的，不需要看文件头猜。
//
// **坏字节不抛异常、不静默跳过**：分叉点记的 pc 来自 Unicorn 的 code hook，一定是指令边界；
// 但诊断时也会有人手写一个偏移进来，落在指令中间的解码结果就是垃圾。所以解不动时输出
// `db <字节>` 并只吃掉 1 字节 —— 每行后面带字节列，一行覆盖了哪几个字节一眼可见，
// 比抛异常把整段现场藏起来更利于定位。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <capstone/capstone.h>

namespace tc {

// 持有 Capstone 句柄：`cs_close` 必须成对，所以不可拷贝（两个对象关同一个句柄会二次释放）。
class Disasm {
  public:
    // 打开 x86-64 句柄并切到 Intel 语法；`cs_open` / `cs_option` 失败抛 std::runtime_error。
    Disasm();
    ~Disasm();

    Disasm(const Disasm&) = delete;
    Disasm& operator=(const Disasm&) = delete;

    // 从偏移 `off` 起反汇编，至少覆盖 `len` 字节（整条指令收口），再往前带 `before` 条、
    // 往后带 `after` 条。一行一条指令：`0x地址  字节 文本`，行序即地址序。
    //
    // 「向前」从 code 的第一个字节线性扫到 `off`：只有从头连续解出来的起点才和 Unicorn
    // 走过的指令边界一致，反向解码出来的起点对不上。
    //
    // `off` 越界返回空串；`len == 0` 只出 before/after 两条边。
    std::string range(const std::vector<uint8_t>& code, uint64_t base, uint64_t off, size_t len,
                      int before = 0, int after = 0) const;

    // 单条：{文本, 字节数}。`base` 只影响分支目标显示成哪个绝对地址，不影响解码。
    // 解不动返回 {"db <两位十六进制>", 1}；`off` 越界返回 {"", 0}（调用方据此停）。
    std::pair<std::string, size_t> one(const std::vector<uint8_t>& code, uint64_t base,
                                       uint64_t off) const;

  private:
    csh handle_ = 0;
};

}  // namespace tc
