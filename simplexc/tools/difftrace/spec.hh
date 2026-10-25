// `.si` 的静态解析结果：名字→地址、Enum 字段序、程序真会碰的绝对区间。
//
// 语义逐条对照 `simplexc/tools/host/simspec.py`（宿主版是 golden）：同一份 `.si`，
// 这里算出的区间集合必须与 Python 版**逐字节相同**，否则差分两边映射的内存不同，
// 比的就不是同一件事了。改这里的正则前先改那边，并把两边数字重新对一遍。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Spec {
    // 读不了文件时抛 std::runtime_error。
    static Spec parse(const std::string& path);

    struct ArrayDecl {
        std::string name;
        uint64_t addr;
        std::string elem_type;  // `Arr[X]` 的 X 原样保留；元素宽度本模块用不到，不复制 ELEM_SIZE 表
    };

    // TODO: 映射与判据共用一个返回值是语义重载，长期应拆成 `map_regions()` 与判据范围的独立接口。
    // 宿主该映射的绝对区间 `[lo, hi)`：未页对齐、可能重叠、可能为空区间（`lo == hi`
    // = 「只知道有这么个地址、不知道多大」，由调用方给默认大小）。合并与页对齐是调用方的事。
    //
    // 空区间是映射语义，**不是**「宽度为零的区间」：拿 `lo <= addr < hi` 在它上面判归属时
    // 标量恒不命中，整片标量区（游戏状态）会被当成区间外丢掉（001 就这么假绿过）。
    // 判归属用 `game_lo()`/`game_hi()` 的区间。
    std::vector<std::pair<uint64_t, uint64_t>> regions() const;

    // `var X = Ptr n` / `const #X = Ptr n` 与数组声明；找不到返回空。
    std::optional<uint64_t> addr(const std::string& name) const;

    // `type X Enum[a, b]` 的成员取值（按声明顺序 0,1,2…，语料里没有显式值）。
    std::optional<std::vector<uint64_t>> enum_values(const std::string& type) const;

    // 枚举类型名 → 成员名（声明顺序）。
    std::map<std::string, std::vector<std::string>> names() const;

    // 已解析的声明原样（对账用）：`regions()` 把标量退化成单点 `(a,a)`、数组退化成 `(a-8,a)`，
    // 两者混在同一张表里就分不出来了 —— 要数「几个标量、几个数组」只能从这里数。
    const std::vector<std::pair<std::string, uint64_t>>& scalars() const;
    const std::vector<ArrayDecl>& arrays() const;

    // `#RESET_DATA_*` 那几块「内存镜像初值」的实际区间。宿主灌图案（`--seed-reset-data`）灌的就是
    // 这几块；语义逐行对 `simspec.SimSpec.reset_data_blocks`：`memory_copy` 的**源**实参优先
    // （`lo = 记录地址 - back`、`size = n`），没被任何 `memory_copy` 引用的块退回声明块
    // `(addr - back, c_alloc(N))`。边界差一点，两边的初始内存就不是同一件事。
    struct ResetBlock {
        std::string name;
        uint64_t lo;
        uint64_t size;
    };
    std::vector<ResetBlock> reset_data_blocks() const;

    uint64_t game_lo() const;
    uint64_t game_hi() const;

private:
    std::map<std::string, std::vector<std::string>> enums_;
    std::vector<std::pair<std::string, uint64_t>> scalars_;
    std::vector<ArrayDecl> arrays_;
    // `memory_copy` / `memory_clear` 实参算出的绝对区间：程序实际碰的范围，不是 `c_alloc(N)`
    std::vector<std::pair<uint64_t, uint64_t>> ranges_;

    // `memory_copy` 第一个实参是 `#RESET_DATA_*` 的那些（名字、块首、字节数）。
    std::vector<ResetBlock> reset_copies_;
    // `const #X = Ptr n` 行尾注释里的 `c_alloc(N)`（块字节数）与 `+ back`（块首比记录地址低多少）。
    struct BlockDecl {
        std::string name;
        uint64_t addr;
        uint64_t back;
        uint64_t size;
    };
    std::vector<BlockDecl> blocks_;
    std::string path_;
};
