/* assemble.h —— 装配器：CLANG 目标码 → 游戏可 JIT 的 blob（接口见 docs/specs/2026-10-15-simplexc-interfaces.md §4）。
 *
 * 契约（冻结版 1）
 * ----------------
 *   输入：若干 COFF 目标文件（LLVM `LLVMTargetMachineEmitToMemoryBuffer(..., LLVMObjectFile)` 的字节，
 *         以及 w64devkit g++ 编出来的 runtime / shim 目标文件）
 *   输出：`Blob` —— `bytes` 是**完整产物**（8 字节头 + 机器码），`entry_off` 相对机器码起点（= 相对 blob+8）。
 *         头 word = `out_len | (1<<62)`（bit62 → 游戏不 free，主设计 §2.3）。
 *
 * 规则（§4）
 * ---------
 *   1. 只接受 `IMAGE_REL_AMD64_REL32` 及其 `_1.._5` 变体；任何其它重定位 → `UnsupportedReloc`
 *      （`.pdata/.xdata` 用的是 `ADDR32NB`，靠**整节丢弃**躲开）。
 *   2. 整节丢弃 `.pdata` / `.xdata` / `.debug*` / `.drectve`。
 *   3. 未定义符号 → 在 `.text` 尾部造 `movabs rax, imm64 ; jmp rax` 桩（12 字节），地址由 `ExternResolver` 给；
 *      名字以 `__imp_` 开头的（mingw 对 `__declspec(dllimport)` 的写法）改造 **8 字节数据槽**，
 *      因为调用方是 `call *__imp_X(%rip)`，解引用目标地址 —— 拿代码桩去顶必崩。
 *   4. 内部绝对引用一律不复现（blob 会被拷到任意地址执行），所以产物里**只有 PC 相对引用**。
 *
 * 实现见 `assemble.cc`；CLI 见 `simplexc/cli/tcblob.cc`（`--run` 可不进游戏自测一次）。
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tc {

struct ObjectFile {
    std::string name;                 /* 只用于报错信息 */
    std::vector<uint8_t> bytes;
};

/* 外部符号解析：壳负责（GetProcAddress 之类）。失败返回 false。 */
class ExternResolver {
public:
    virtual ~ExternResolver() = default;
    virtual bool resolve(std::string_view symbol, uint64_t& addr) = 0;
};

struct AsmOptions {
    bool verbose = false;
    uint64_t max_blob = 64ull << 20;   /* 与转发层 dump() 的上限一致 */
};

enum class AsmStatus {
    Ok,
    BadObject,             /* COFF 解析不了 */
    UndefinedSymbol,       /* 外部符号解析失败 */
    UnsupportedReloc,      /* 只有 REL32 家族被接受 */
};

struct Blob {
    std::vector<uint8_t> bytes;        /* 完整产物：bytes[0:8] = 头，机器码从 +8 起 */
    uint64_t entry_off = 0;            /* 相对机器码起点 */
    uint64_t out_len() const { return bytes.size(); }
    const uint8_t* code() const { return bytes.data() + 8; }
    uint64_t code_size() const { return bytes.size() - 8; }
};

struct AsmResult {
    AsmStatus status{};   /* 零值 = Ok（枚举首个值），这样写也躲开「:: 被误当位域」的静态检查 */
    std::string detail;
    Blob blob;
    /* 统计（给人看 / 给判据用） */
    uint64_t text_size = 0, rdata_size = 0, data_size = 0, bss_size = 0;
    uint32_t stub_count = 0, reloc_count = 0;
};

AsmResult assemble(const std::vector<ObjectFile>& objs,
                   std::string_view entry_symbol,
                   ExternResolver& resolver,
                   const AsmOptions& opt = {});

}  // namespace tc
