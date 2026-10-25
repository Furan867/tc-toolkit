/* codegen/asm_x64_selftest.cc —— `asm_x64` 编码器的判据入口（不在产品路径）。
 *
 * 判什么：每条源指令 → 一串**写死的字节**。期望值的来源分两类，用例名后缀写清楚：
 *   · `ref` = 参考编译器产物里量到的字节。出处是 `work/tmp/asmx64/{a1_copy,a2_clear,
 *     a3_breakpoint,t4_named_params}.*`，偏移写在用例的 `why` 里（定位命令见
 *     `work/tmp/asmx64/notes.md`）。这类期望**不许改** —— 它就是「与参考逐字节相同」的定义。
 *   · `own` = 参考从未发过的形态（`$name` 槽访问），字节按本模块的口径自定，
 *     只在「口径没被无意改掉」这个意义上成立（见 notes.md 的「未对齐」一节）。
 * 反面用例（白名单外的 `nop`、未解析的形参名、缺槽、立即数越界）必须**失败**并且
 * 错误串里出现指定片段 —— 静默通过比编错更难查。
 *
 * 用法：asm_x64_selftest     退出码 0 = 全部 PASS；1 = 有用例 FAIL。
 * 构建：python simplexc/tools/build.py asm_x64_selftest
 */
#include "asm_x64.hh"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ax = simplexc::codegen::asm_x64;

int g_total = 0;
int g_failed = 0;

ax::Inst inst(const std::string& mnemonic, std::vector<std::string> ops) {
    ax::Inst i;
    i.mnemonic = mnemonic;
    i.ops = std::move(ops);
    return i;
}

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0xF]);
    }
    return out;
}

struct OkCase {
    std::string name;
    ax::Inst inst;
    std::vector<ax::Slot> slots;
    std::vector<std::uint8_t> want;
    std::string why;
};

struct BadCase {
    std::string name;
    ax::Inst inst;
    std::vector<ax::Slot> slots;
    std::string want_error; /* 错误串里必须出现的片段 */
};

const std::vector<ax::Slot> kResultSlot = {{"result", -8}};

/* 期望字节按「参考产物里的偏移」排：同一片的相邻用例来自同一条连续指令流，改期望前先回
 * 产物里看那几个字节还在不在。 */
const std::vector<OkCase>& ok_cases() {
    static const std::vector<OkCase> cases = {
        /* ---- get_cycle_count（work/tmp/asmx64/a1_copy.bin 0x2175..0x2193） ---- */
        {"1  mov rcx,32（ref a1@0x217f）", inst("mov", {"rcx", "32"}), {}, {0xB9, 0x20, 0x00, 0x00, 0x00},
         "参考发 32 位形式，不是 48 c7 c1 20 00 00 00"},
        {"2  rdtsc（ref a1@0x217a）", inst("rdtsc", {}), {}, {0x0F, 0x31}, ""},
        {"3  shl rdx,cl（ref a1@0x2187）", inst("shl", {"rdx", "cl"}), {}, {0x48, 0xD3, 0xE2}, ""},
        {"4  or rax,rdx（ref a1@0x218d）", inst("or", {"rax", "rdx"}), {}, {0x48, 0x09, 0xD0}, ""},
        {"5  mov rdx,rcx（ref a1@0x217c，glue 同表）", inst("mov", {"rdx", "rcx"}), {},
         {0x48, 0x89, 0xCA}, "源里 `mov rcx,32` 前后的 version glue 走的就是这张表"},

        /* ---- memory_copy（a1@0x2196..0x21a3 = t4_named_params 的同一段） ---- */
        {"6  mov rsi,0x3000（ref a1@0x2196）", inst("mov", {"rsi", "0x3000"}), {},
         {0xBE, 0x00, 0x30, 0x00, 0x00}, "形参 a 内联成 Ptr 常量后就是这个字节串"},
        {"7  mov rdi,0x4000（ref t4@0x2175）", inst("mov", {"rdi", "0x4000"}), {},
         {0xBF, 0x00, 0x40, 0x00, 0x00}, ""},
        {"8  rep movsb（ref a1@0x21a3）", inst("rep movsb", {}), {}, {0xF3, 0xA4}, ""},

        /* ---- memory_clear（a2_clear.bin 0x2184..0x2191 + 前缀体 0x6e6） ---- */
        {"9  mov al,0（ref a1@0x6e6）", inst("mov", {"al", "0"}), {}, {0xB8, 0x00, 0x00, 0x00, 0x00},
         "8 位名被参考归一到 eax 的 32 位形式（b0 00 不是参考的字节）"},
        {"10 mov rdi,0x5000（ref a2@0x2189）", inst("mov", {"rdi", "0x5000"}), {},
         {0xBF, 0x00, 0x50, 0x00, 0x00}, ""},
        {"11 rep stosb（ref a2@0x2191）", inst("rep stosb", {}), {}, {0xF3, 0xAA}, ""},

        /* ---- breakpoint（a3_breakpoint.bin 0x215e..0x2168） ---- */
        {"12 mov rcx,0（ref a3@0x215e）", inst("mov", {"rcx", "0"}), {}, {0xB9, 0x00, 0x00, 0x00, 0x00},
         ""},
        {"13 mov rdx,0（ref a3@0x2163）", inst("mov", {"rdx", "0"}), {}, {0xBA, 0x00, 0x00, 0x00, 0x00},
         ""},
        {"14 idiv rcx（ref a3@0x2168）", inst("idiv", {"rcx"}), {}, {0x48, 0xF7, 0xF9}, ""},

        /* ---- REX.R/B 位：高寄存器（ref a1@0x14bc 的 49 89 c0 = mov r8,rax） ---- */
        {"15 mov r8,rax（ref a1@0x14bc）", inst("mov", {"r8", "rax"}), {}, {0x49, 0x89, 0xC0}, ""},
        {"16 mov rax,rcx（ref a2@0x6dc）", inst("mov", {"rax", "rcx"}), {}, {0x48, 0x89, 0xC8}, ""},

        /* ---- `$name` 槽访问（own：参考不发访存，见 notes.md「未对齐」） ---- */
        {"17 mov $result,rax（own，槽 -8）", inst("mov", {"$result", "rax"}), kResultSlot,
         {0x48, 0x89, 0x84, 0x24, 0xF8, 0xFF, 0xFF, 0xFF},
         "参考把它落成 48 89 c3 + 48 89 dd 两条寄存器 mov，从不发 [rsp+off]"},
        {"18 mov rbx,$result（own，槽 -8）", inst("mov", {"rbx", "$result"}), kResultSlot,
         {0x48, 0x8B, 0x9C, 0x24, 0xF8, 0xFF, 0xFF, 0xFF}, ""},
        {"19 mov $result,0（own，槽 16）", inst("mov", {"$result", "0"}), {{"result", 16}},
         {0x48, 0xC7, 0x84, 0x24, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
         "C7 /0 的 imm32 会符号扩展到 64 位"},
    };
    return cases;
}

const std::vector<BadCase>& bad_cases() {
    static const std::vector<BadCase> cases = {
        {"B1 nop 在白名单外（参考：Unsupported instruction nop.）", inst("nop", {}), {},
         "Unsupported instruction nop"},
        {"B2 add 也不做（参考的 dll 表里有 add，但活路径不用）", inst("add", {"rax", "rdx"}), {},
         "Unsupported instruction add"},
        {"B3 形参名没被调用方解析成寄存器", inst("mov", {"rsi", "source"}), {},
         "不认识"},
        {"B4 $槽名没有对应偏移", inst("mov", {"$missing", "rax"}), kResultSlot, "没有对应的栈槽"},
        {"B5 立即数超出 imm32", inst("mov", {"rcx", "0x100000000"}), {}, "超出 imm32"},
        {"B6 shl 只做 cl 计数", inst("shl", {"rdx", "cl2"}), {}, "不认识"},
        {"B7 rdtsc 不带操作数", inst("rdtsc", {"rax"}), {}, "不带操作数"},
        {"B8 mov 寄存器到寄存器只做 64 位", inst("mov", {"eax", "ecx"}), {}, "只做 64 位形式"},
    };
    return cases;
}

void run_ok(const OkCase& c) {
    ++g_total;
    const ax::Result r = ax::encode(c.inst, c.slots);
    const bool good = r.ok && r.bytes == c.want;
    if (good) {
        std::cout << "PASS " << c.name << "  → " << hex(r.bytes) << "\n";
        return;
    }
    ++g_failed;
    std::cout << "FAIL " << c.name << "\n  期望 " << hex(c.want) << "\n  实际 "
              << (r.ok ? hex(r.bytes) : ("<失败> " + r.error)) << "\n";
    if (!c.why.empty()) std::cout << "  口径：" << c.why << "\n";
}

void run_bad(const BadCase& c) {
    ++g_total;
    const ax::Result r = ax::encode(c.inst, c.slots);
    if (!r.ok && r.error.find(c.want_error) != std::string::npos) {
        std::cout << "PASS " << c.name << "  → 失败：" << r.error << "\n";
        return;
    }
    ++g_failed;
    std::cout << "FAIL " << c.name << "\n  期望失败且错误串含「" << c.want_error << "」，实际 "
              << (r.ok ? ("编成了 " + hex(r.bytes)) : ("错误串不含该片段的失败：" + r.error)) << "\n";
}

/* 白名单的边界：7 条都在 `mnemonic_supported` 里，第 8 条（nop）不在。
 * 单列一条是因为「整块能不能过」的粗筛用它，而不是用 `encode` 的副作用。 */
void run_whitelist() {
    const std::vector<std::string> inside = {"mov", "idiv", "shl", "or",
                                             "rdtsc", "rep movsb", "rep stosb"};
    for (const std::string& m : inside) {
        ++g_total;
        if (ax::mnemonic_supported(m)) {
            std::cout << "PASS 白名单包含 " << m << "\n";
        } else {
            ++g_failed;
            std::cout << "FAIL 白名单漏了 " << m << "\n";
        }
    }
    ++g_total;
    if (!ax::mnemonic_supported("nop")) {
        std::cout << "PASS 白名单不含 nop\n";
    } else {
        ++g_failed;
        std::cout << "FAIL 白名单里混进了 nop\n";
    }
}

}  // namespace

int main() {
    std::cout << "asm_x64_selftest: 7 条白名单助记符的编码 / 槽访问 / 白名单外失败\n\n";
    for (const OkCase& c : ok_cases()) run_ok(c);
    for (const BadCase& c : bad_cases()) run_bad(c);
    run_whitelist();

    std::cout << "\n总计 " << g_total << " 条用例：PASS " << (g_total - g_failed) << "，FAIL "
              << g_failed << "\n";
    if (g_failed == 0) {
        std::cout << "结论：全部 PASS\n";
        return 0;
    }
    std::cout << "结论：有 " << g_failed
              << " 条 FAIL —— 标 `ref` 的期望不许改（它就是逐字节对齐的定义），"
                 "先查 asm_x64.cc\n";
    return 1;
}
