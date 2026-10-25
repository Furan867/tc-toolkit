/* codegen/asm_x64.cc —— 7 个白名单助记符 → 机器码。
 *
 * 每张表旁边的注释都指向**参考产物里的一串字节**（片段与定位命令见 `work/tmp/asmx64/notes.md`）。
 * 为什么必须这样：这一层的唯一判据是「与参考逐字节相同」，手册上的等价编码（`mov rcx,32` 写成
 * `48 c7 c1 20 00 00 00` 也对）会让判据失效 —— 等价不等于相同。
 *
 * 已对齐的参考证据（片段 → 产物偏移 → 字节）：
 *   `mov rcx,32`      t1/a1 0x217f  b9 20 00 00 00   （32 位形式，不是 48 c7 c1 …）
 *   `mov rcx,0`       a3   0x215e  b9 00 00 00 00
 *   `mov rdx,0`       a3   0x2163  ba 00 00 00 00
 *   `mov al,0`        a1   0x6e6   b8 00 00 00 00   （8 位名也发 32 位形式 ⇒ 归一到父名）
 *   `mov rsi,source`  a1   0x2196  be 00 30 00 00
 *   `mov rdi,source`  a2   0x2189  bf 00 50 00 00
 *   `mov <reg>,<reg>` t1   0x217c  48 89 ca = mov rdx,rcx（glue 与源里长 64 位形式同一张表）
 *   `shl rdx,cl`      t1   0x2187  48 d3 e2
 *   `or rax,rdx`      t1   0x218d  48 09 d0
 *   `rdtsc`           t1   0x217a  0f 31
 *   `rep movsb`       t4   0x217f  f3 a4
 *   `rep stosb`       a2   0x2191  f3 aa
 *   `idiv rcx`        a3   0x2168  48 f7 f9
 */
#include "asm_x64.hh"

#include <cstddef>

namespace simplexc {
namespace codegen {
namespace asm_x64 {

namespace {

enum class Width { k8, k32, k64 };

struct RegInfo {
    const char* name;
    unsigned code; /* 0..15，REX.B / REX.R 取高位 */
    Width width;
};

/* 三个位宽的名字都列：8 位的要能查（`mov al,0`），r8–r15 也要能查（参考把 asm 寄存器名
 * 分配成的 vreg 落在它们上面，`49 89 c0` = `mov r8,rax`，a1 0x14bc）。 */
constexpr RegInfo kRegs[] = {
    {"rax", 0, Width::k64},   {"rcx", 1, Width::k64},   {"rdx", 2, Width::k64},
    {"rbx", 3, Width::k64},   {"rsp", 4, Width::k64},   {"rbp", 5, Width::k64},
    {"rsi", 6, Width::k64},   {"rdi", 7, Width::k64},   {"r8", 8, Width::k64},
    {"r9", 9, Width::k64},    {"r10", 10, Width::k64},  {"r11", 11, Width::k64},
    {"r12", 12, Width::k64},  {"r13", 13, Width::k64},  {"r14", 14, Width::k64},
    {"r15", 15, Width::k64},

    {"eax", 0, Width::k32},   {"ecx", 1, Width::k32},   {"edx", 2, Width::k32},
    {"ebx", 3, Width::k32},   {"esp", 4, Width::k32},   {"ebp", 5, Width::k32},
    {"esi", 6, Width::k32},   {"edi", 7, Width::k32},   {"r8d", 8, Width::k32},
    {"r9d", 9, Width::k32},   {"r10d", 10, Width::k32}, {"r11d", 11, Width::k32},
    {"r12d", 12, Width::k32}, {"r13d", 13, Width::k32}, {"r14d", 14, Width::k32},
    {"r15d", 15, Width::k32},

    {"al", 0, Width::k8},     {"cl", 1, Width::k8},     {"dl", 2, Width::k8},
    {"bl", 3, Width::k8},     {"spl", 4, Width::k8},    {"bpl", 5, Width::k8},
    {"sil", 6, Width::k8},    {"dil", 7, Width::k8},    {"r8b", 8, Width::k8},
    {"r9b", 9, Width::k8},    {"r10b", 10, Width::k8},  {"r11b", 11, Width::k8},
    {"r12b", 12, Width::k8},  {"r13b", 13, Width::k8},  {"r14b", 14, Width::k8},
    {"r15b", 15, Width::k8},
};

const RegInfo* find_reg(const std::string& name) {
    for (const RegInfo& r : kRegs) {
        if (name == r.name) return &r;
    }
    return nullptr;
}

/* 白名单就是 report §6 数出来的活代码那 7 条。`nop` 不在这里 —— 参考对它的回话是
 * `Unsupported instruction nop.`（t7），我们同样失败，只是错误串里多写一句本模块的范围。 */
constexpr const char* kMnemonics[] = {
    "mov", "idiv", "shl", "or", "rdtsc", "rep movsb", "rep stosb",
};

enum class Kind { kReg, kSlot, kImm };

struct Operand {
    Kind kind;
    const RegInfo* reg;
    const Slot* slot;
    std::int64_t imm;
};

/* 十进制与 `0x`/`0X` 十六进制两种写法（不认 `0` 开头的八进制：源里 `10` 就是十进制 10，
 * `010` 当 8 没有依据）。超出 imm32 一律报错而不是截断 —— 参考在活路径上只发过 ≤0xFFFFFFFF
 * 的立即数，超范围的形态没有字节证据。 */
bool parse_imm(const std::string& text, std::int64_t* out, std::string* error) {
    std::size_t i = 0;
    bool neg = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        neg = text[i] == '-';
        ++i;
    }
    bool hex = i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X');
    if (hex) i += 2;
    if (i == text.size()) {
        *error = "没有数字的操作数 '" + text + "'";
        return false;
    }
    const int base = hex ? 16 : 10;
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0 || digit >= base) {
            *error = "立即数 '" + text + "' 里有非数字字符";
            return false;
        }
        value = value * base + digit;
        if (value > 0x7FFFFFFF) {
            *error = "立即数 '" + text + "' 超出 imm32（参考在活路径上只发过 ≤0x7FFFFFFF 的）";
            return false;
        }
    }
    *out = neg ? -value : value;
    return true;
}

bool classify(const std::string& text, const std::vector<Slot>& slots, Operand* out,
              std::string* error) {
    if (const RegInfo* reg = find_reg(text)) {
        out->kind = Kind::kReg;
        out->reg = reg;
        out->slot = nullptr;
        out->imm = 0;
        return true;
    }
    if (!text.empty() && text[0] == '$') {
        const std::string name = text.substr(1);
        for (const Slot& slot : slots) {
            if (slot.name == name) {
                out->kind = Kind::kSlot;
                out->reg = nullptr;
                out->slot = &slot;
                out->imm = 0;
                return true;
            }
        }
        *error = "'" + text + "' 没有对应的栈槽（槽偏移由调用方给，见 asm_x64.hh §Slot）";
        return false;
    }
    if (!text.empty() && (text[0] == '-' || text[0] == '+' ||
                          (text[0] >= '0' && text[0] <= '9'))) {
        out->kind = Kind::kImm;
        out->reg = nullptr;
        out->slot = nullptr;
        return parse_imm(text, &out->imm, error);
    }
    *error = "操作数 '" + text + "' 不认识：不是寄存器、不是立即数、也不是 $槽名"
             "（形参名要先由调用方解析成寄存器）";
    return false;
}

class Bytes {
  public:
    void put(unsigned b) { out_.push_back(static_cast<std::uint8_t>(b)); }

    /* REX：W 置位 + R/B 由寄存器号高位与 REX 是否需要决定。目标都是 64 位形式，
     * 所以 W 恒为 1；真正的「要不要发 REX」判据是 R/B。 */
    void rex(bool w, bool r, bool b) {
        const unsigned rex = 0x40 | (w ? 8u : 0u) | (r ? 4u : 0u) | (b ? 1u : 0u);
        if (rex != 0x40) put(rex);
    }

    void imm32(std::int64_t v) {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        for (int shift = 0; shift < 32; shift += 8) put((u >> shift) & 0xFFu);
    }

    void modrm(unsigned mod, unsigned reg, unsigned rm) {
        put(((mod & 3u) << 6) | ((reg & 7u) << 3) | (rm & 7u));
    }

    /* rsp 相对 disp32：mod=10 / rm=100(SIB) / sib=0x24（base=rsp、index=none、scale=1）。
     * 固定 disp32 而不按偏移大小收成 disp8：偏移来自调用方，编码器看不见范围，
     * 两种取值会让同一源指令编出不同长度。 */
    void rsp_disp32(unsigned reg, std::int32_t offset) {
        modrm(2, reg, 4);
        put(0x24);
        imm32(offset);
    }

    std::vector<std::uint8_t> take() { return std::move(out_); }

  private:
    std::vector<std::uint8_t> out_;
};

Result fail(std::string message) {
    Result r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

Result done(Bytes* bytes) {
    Result r;
    r.ok = true;
    r.bytes = bytes->take();
    return r;
}

Result encode_mov(const std::vector<std::string>& ops, const std::vector<Slot>& slots,
                  std::string* error) {
    if (ops.size() != 2) return fail("mov 要正好 2 个操作数（源里是 `mov 目标, 源`）");
    Operand dst;
    Operand src;
    if (!classify(ops[0], slots, &dst, error) || !classify(ops[1], slots, &src, error)) {
        return fail(*error);
    }

    Bytes b;
    /* 寄存器 ← 寄存器：89 /r（MOV r/m64, r64），REX.W。证据见文件头 `mov rdx,rcx`。 */
    if (dst.kind == Kind::kReg && src.kind == Kind::kReg) {
        if (dst.reg->width != Width::k64 || src.reg->width != Width::k64) {
            return fail("mov 的寄存器到寄存器只做 64 位形式（参考只发过 48 89 /r）：'" +
                        ops[0] + "' / '" + ops[1] + "'");
        }
        b.rex(true, src.reg->code >= 8, dst.reg->code >= 8);
        b.put(0x89);
        b.modrm(3, src.reg->code, dst.reg->code);
        return done(&b);
    }

    /* 寄存器 ← 立即数：B8+rd id，**32 位形式**、不带 REX.W。这是参考的实测口径：
     * `mov rcx,32` → b9 20 00 00 00；连 8 位的 `mov al,0` 也发 b8 00 00 00 00
     * （即 8 位名归一到它的 32 位父名）。照抄它才逐字节对齐；语义上等于零扩展到 64 位。 */
    if (dst.kind == Kind::kReg && src.kind == Kind::kImm) {
        const unsigned code = dst.reg->code;
        b.rex(false, false, code >= 8);
        b.put(0xB8 + (code & 7u));
        b.imm32(src.imm);
        return done(&b);
    }

    /* `$name` 是源里的局部变量 → 一次 rsp 相对访存（存/取各一条 mov）。
     * 这一形态**没有参考字节**：参考把局部变量提升成 vreg，`mov $result,rax` 落成
     * `48 89 c3` + `48 89 dd`（a1 0x2190），从不发访存。见 work/tmp/asmx64/notes.md 的「未对齐」。 */
    if (dst.kind == Kind::kSlot && src.kind == Kind::kReg) {
        if (src.reg->width != Width::k64) {
            return fail("写 $槽只做 64 位寄存器（'" + ops[1] + "'）");
        }
        b.rex(true, src.reg->code >= 8, false);
        b.put(0x89);
        b.rsp_disp32(src.reg->code, dst.slot->offset);
        return done(&b);
    }
    if (dst.kind == Kind::kSlot && src.kind == Kind::kImm) {
        if (src.imm < -0x80000000LL || src.imm > 0x7FFFFFFFLL) {
            return fail("写 $槽的立即数 '" + ops[1] + "' 超出 imm32（C7 /0 会符号扩展）");
        }
        b.rex(true, false, false);
        b.put(0xC7);
        b.rsp_disp32(0, dst.slot->offset); /* /0 = MOV r/m64, imm32 */
        b.imm32(src.imm);
        return done(&b);
    }
    if (dst.kind == Kind::kReg && src.kind == Kind::kSlot) {
        if (dst.reg->width != Width::k64) {
            return fail("读 $槽只做 64 位寄存器（'" + ops[0] + "'）");
        }
        b.rex(true, dst.reg->code >= 8, false);
        b.put(0x8B);
        b.rsp_disp32(dst.reg->code, src.slot->offset);
        return done(&b);
    }
    return fail("mov 的这对操作数不在白名单内（'" + ops[0] + "', '" + ops[1] +
                "'）：只做 寄存器←寄存器 / 寄存器←立即数 / $槽←寄存器 / $槽←立即数 / 寄存器←$槽");
}

Result encode_idiv(const std::vector<std::string>& ops, const std::vector<Slot>& slots,
                   std::string* error) {
    if (ops.size() != 1) return fail("idiv 要正好 1 个操作数");
    Operand src;
    if (!classify(ops[0], slots, &src, error)) return fail(*error);
    if (src.kind != Kind::kReg || src.reg->width != Width::k64) {
        return fail("idiv 只做 64 位寄存器（'" + ops[0] + "'）");
    }
    Bytes b;
    b.rex(true, false, src.reg->code >= 8);
    b.put(0xF7);
    b.modrm(3, 7, src.reg->code); /* /7 = IDIV */
    return done(&b);
}

Result encode_shl(const std::vector<std::string>& ops, const std::vector<Slot>& slots,
                  std::string* error) {
    if (ops.size() != 2) return fail("shl 要正好 2 个操作数（源里是 `shl 目标, cl`）");
    Operand dst;
    Operand count;
    if (!classify(ops[0], slots, &dst, error) || !classify(ops[1], slots, &count, error)) {
        return fail(*error);
    }
    if (dst.kind != Kind::kReg || dst.reg->width != Width::k64) {
        return fail("shl 只做 64 位目标寄存器（'" + ops[0] + "'）");
    }
    if (count.kind != Kind::kReg || count.reg->name != std::string("cl")) {
        return fail("shl 只做 cl 计数（活路径只有 `shl rdx, cl`，其它形态没有参考字节）");
    }
    Bytes b;
    b.rex(true, false, dst.reg->code >= 8);
    b.put(0xD3); /* D3 /4 = SHL r/m64, CL */
    b.modrm(3, 4, dst.reg->code);
    return done(&b);
}

Result encode_or(const std::vector<std::string>& ops, const std::vector<Slot>& slots,
                 std::string* error) {
    if (ops.size() != 2) return fail("or 要正好 2 个操作数（源里是 `or 目标, 源`）");
    Operand dst;
    Operand src;
    if (!classify(ops[0], slots, &dst, error) || !classify(ops[1], slots, &src, error)) {
        return fail(*error);
    }
    if (dst.kind != Kind::kReg || dst.reg->width != Width::k64 || src.kind != Kind::kReg ||
        src.reg->width != Width::k64) {
        return fail("or 只做 64 位寄存器到寄存器（'" + ops[0] + "', '" + ops[1] + "'）");
    }
    Bytes b;
    b.rex(true, src.reg->code >= 8, dst.reg->code >= 8);
    b.put(0x09); /* 09 /r = OR r/m64, r64：目标是 modrm.rm，源是 modrm.reg */
    b.modrm(3, src.reg->code, dst.reg->code);
    return done(&b);
}

}  // namespace

bool mnemonic_supported(const std::string& mnemonic) {
    for (const char* name : kMnemonics) {
        if (mnemonic == name) return true;
    }
    return false;
}

bool is_register(const std::string& name) { return find_reg(name) != nullptr; }

/* 同一个硬件寄存器的三种拼写共用 `code`（0..15），三种拼写各占一行，所以「按 code 找同一位宽
 * 的名字」就是查表。位宽不匹配 = 这个寄存器没有该位宽的拼写（不存在）。 */
std::string reg_parent(const std::string& name) {
    const RegInfo* reg = find_reg(name);
    if (reg == nullptr) return std::string();
    if (reg->width == Width::k64) return name;
    for (const RegInfo& r : kRegs) {
        if (r.code == reg->code && r.width == Width::k64) return r.name;
    }
    return std::string();
}

std::string reg32_name(const std::string& name) {
    const RegInfo* reg = find_reg(name);
    if (reg == nullptr) return std::string();
    if (reg->width == Width::k32) return name;
    for (const RegInfo& r : kRegs) {
        if (r.code == reg->code && r.width == Width::k32) return r.name;
    }
    return std::string();
}

Result encode(const Inst& inst, const std::vector<Slot>& slots) {
    if (!mnemonic_supported(inst.mnemonic)) {
        return fail("Unsupported instruction " + inst.mnemonic +
                    ".（本模块只做活代码白名单那 7 条：mov / idiv / shl / or / rdtsc / "
                    "rep movsb / rep stosb）");
    }
    std::string error;
    if (inst.mnemonic == "mov") return encode_mov(inst.ops, slots, &error);
    if (inst.mnemonic == "idiv") return encode_idiv(inst.ops, slots, &error);
    if (inst.mnemonic == "shl") return encode_shl(inst.ops, slots, &error);
    if (inst.mnemonic == "or") return encode_or(inst.ops, slots, &error);
    if (!inst.ops.empty()) {
        return fail(inst.mnemonic + " 不带操作数（源里是裸助记符）");
    }
    Bytes b;
    if (inst.mnemonic == "rdtsc") {
        b.put(0x0F);
        b.put(0x31);
    } else if (inst.mnemonic == "rep movsb") {
        b.put(0xF3);
        b.put(0xA4);
    } else {
        b.put(0xF3); /* rep stosb */
        b.put(0xAA);
    }
    return done(&b);
}

}  // namespace asm_x64
}  // namespace codegen
}  // namespace simplexc
