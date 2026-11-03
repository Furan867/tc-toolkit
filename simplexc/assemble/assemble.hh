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
 *   1. 接受 `IMAGE_REL_AMD64_REL32` 及其 `_1.._5` 变体，以及 `ADDR64`(0x01)；任何其它重定位 →
 *      `UnsupportedReloc`（`.pdata/.xdata` 用的是 `ADDR32NB`，靠**整节丢弃**躲开）。
 *      `ADDR64` 装出来的是 `S + A`，即**相对产物起点**的地址：142 号关卡踩到的就是它
 *      （LLVM 把「各 case 各存一个字符串指针」降成 `[3 x ptr]`，值指向 `.rdata` 里的本地常量）。
 *      位置无关要求终端在运行期给它加基址，而产物内部还没有这个机制 → 含 `ADDR64` 的产物
 *      **装得出但跑不了**（实测见 `work/tmp/coff/notes.md`）。
 *   2. 整节丢弃 `.pdata` / `.xdata` / `.debug*` / `.drectve`。
 *   3. 未定义符号 → 在 `.text` 尾部造 `movabs rax, imm64 ; jmp rax` 桩（12 字节），地址由 `ExternResolver` 给；
 *      名字以 `__imp_` 开头的（mingw 对 `__declspec(dllimport)` 的写法）改造 **8 字节数据槽**，
 *      因为调用方是 `call *__imp_X(%rip)`，解引用目标地址 —— 拿代码桩去顶必崩。
 *   4. 内部绝对引用一律不复现（blob 会被拷到任意地址执行），所以产物里**只有 PC 相对引用**。
 *   5. `PointerToRawData == 0` 的节在文件里**没有原始数据**（`.bss` 的标准形态，内容隐含全 0）：
 *      不读文件（偏移 0 是 COFF 头，含编译时间戳 —— 读了产物就不可复现），但在产物里按
 *      `SizeOfRawData` 留出这么多字节的零。对象文件里 `VirtualSize` 是 0（clang 与 gcc 都是），
 *      所以「节有多大」要看 `SizeOfRawData`。
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
    UnsupportedReloc,      /* 只有 REL32 家族与 ADDR64 被接受 */
    TooLarge,              /* out_len 超过 AsmOptions::max_blob */
};

struct Blob {
    std::vector<uint8_t> bytes;        /* 完整产物：bytes[0:8] = 头，机器码从 +8 起 */
    uint64_t entry_off = 0;            /* 相对机器码起点 */
    uint64_t out_len() const { return bytes.size(); }
    const uint8_t* code() const { return bytes.data() + 8; }
    uint64_t code_size() const { return bytes.size() - 8; }
};

/* 一个未定义符号在产物里的落点（接口冻结版 1.1 增量）。给宿主用：它得知道哪此 movabs 是
 * 「extern 跳板」而不是「游戏地址/常量」—— 否则会把 kernel32 的绝对地址当成未知地址拒绝掉。
 *
 *   offset —— 相对**机器码起点**（= 相对 `blob+8`），就是 `Blob::code()` 的坐标系；
 *   symbol —— COFF 里的**原始**名字（`__imp_X` 前缀保留）：调用方靠前缀分形态，
 *             跟 `Assembler` 里造桩/槽的分支规则一致。
 */
struct StubRef {
    uint64_t offset = 0;
    std::string symbol;
};

struct AsmResult {
    AsmStatus status{};   /* 零值 = Ok（枚举首个值），这样写也躲开「:: 被误当位域」的静态检查 */
    std::string detail;
    Blob blob;
    /* 统计（给人看 / 给判据用） */
    uint64_t text_size = 0, rdata_size = 0, data_size = 0, bss_size = 0;
    uint32_t stub_count = 0, reloc_count = 0;
    /* 每个**已解析**的未定义符号的落点，顺序 = 造桩顺序（`stub_count` 是它的条数）*/
    std::vector<StubRef> stubs;

    /* 布局的另外两个基点：`body_end` = 各节拼完的末尾（= 代码桩块起点 `stubs_base`，
     * 两者相等是布局定义、不是巧合），`slots_base` = `__imp_` 数据槽块起点。
     * `--offsets-out` 要用它们把「对象址」换算成 blob 偏移（hotspot spec §6）。 */
    uint64_t body_end = 0, stubs_base = 0, slots_base = 0;

    /* 每个进产物的输入节落在哪：`obj` = 装配入参下标（0 基）、`sec` = COFF 节下标（1 基）——
     * 与 `.debug_line` 里锚点的口径一致。按 `blob_off` 升序（不变量由 `assemble` 保证）。 */
    struct SectionPlacement {
        uint32_t obj = 0;
        uint32_t sec = 0;
        std::string name;
        std::string cls;   /* "text" / "rdata" / "data" / "bss" */
        uint64_t blob_off = 0; /* 相对机器码起点（= blob 文件下标 - 8） */
        uint64_t size = 0;
        uint64_t align = 1;
    };
    std::vector<SectionPlacement> sections;
    /* 带 `.debug_line` 的装配入参下标（0 基）。这些对象才有行表可展开，是 `--offsets-out`
     * 里 `debug_lines[].anchors` 的成员集合。 */
    std::vector<uint32_t> debug_line_objs;
};

AsmResult assemble(const std::vector<ObjectFile>& objs,
                   std::string_view entry_symbol,
                   ExternResolver& resolver,
                   const AsmOptions& opt = {});

}  // namespace tc
