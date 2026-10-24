// 实现（接口与理由见 disasm.hh）。
#include "disasm.hh"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace tc {
namespace {

// 字节列的对齐宽度：x86 单条最长 15 字节（= 45 字符），全按最长留白会让文本列离得太远。
// 8 字节以内（绝大多数指令）对齐，更长的就自然推后 —— 诊断输出里可读性优先于绝对对齐。
constexpr size_t kBytesColumn = 24;

std::string addr_hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string byte_hex(uint8_t b) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x", b);
    return buf;
}

// 指令的原始字节，空格分隔。字节列是「这一行到底对应哪几个字节」的唯一判据：
// 坏字节（db）与正常解码在文本上能混，在字节数上混不了。
std::string bytes_text(const std::vector<uint8_t>& code, size_t off, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        if (i != 0) out += ' ';
        out += byte_hex(code[off + i]);
    }
    return out;
}

}  // namespace

Disasm::Disasm() {
    const cs_err err = cs_open(CS_ARCH_X86, CS_MODE_64, &handle_);
    if (err != CS_ERR_OK) throw std::runtime_error(std::string("cs_open 失败: ") + cs_strerror(err));

    const cs_err opt = cs_option(handle_, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
    if (opt != CS_ERR_OK) {
        cs_close(&handle_);  // 构造失败也要把句柄还回去，否则每次重试漏一份引擎
        throw std::runtime_error(std::string("cs_option(Intel 语法) 失败: ") + cs_strerror(opt));
    }
}

Disasm::~Disasm() {
    if (handle_ != 0) cs_close(&handle_);
}

std::pair<std::string, size_t> Disasm::one(const std::vector<uint8_t>& code, uint64_t base,
                                           uint64_t off) const {
    if (off >= code.size()) return {"", 0};

    cs_insn* insn = nullptr;
    const size_t n = cs_disasm(handle_, code.data() + off, code.size() - off, base + off, 1, &insn);
    if (n == 0) {
        // cs_disasm 返回 0 有两种原因：字节解不成指令（常态），或内存不足。不区分的话 OOM
        // 会被打成一串 db 而看不出来 —— 这行数据是假的，必须抛。
        if (cs_errno(handle_) == CS_ERR_MEM)
            throw std::runtime_error("cs_disasm 内存不足（off=" + addr_hex(base + off) + "）");
        return {"db " + byte_hex(code[off]), 1};
    }

    std::string text = insn->mnemonic;
    if (insn->op_str[0] != '\0') {
        text += ' ';
        text += insn->op_str;
    }
    const size_t size = insn->size;
    cs_free(insn, n);
    return {text, size};
}

std::string Disasm::range(const std::vector<uint8_t>& code, uint64_t base, uint64_t off, size_t len,
                          int before, int after) const {
    if (off >= code.size()) return "";

    std::vector<size_t> starts;  // 要输出的指令起点，按地址升序

    if (before > 0) {
        std::vector<size_t> seen;
        size_t pos = 0;
        while (pos < off) {
            const size_t size = one(code, base, pos).second;
            if (size == 0) break;
            seen.push_back(pos);
            pos += size;
        }
        // pos 跨过 off ⇒ off 落在最后一条指令中间；那条指令照旧算「前一条」（它确实起在 off 之前），
        // 但下面从 off 起的解码是另一条路 —— 两行重叠正是要看见的现象。
        const size_t keep = std::min(static_cast<size_t>(before), seen.size());
        for (size_t i = seen.size() - keep; i < seen.size(); ++i) starts.push_back(seen[i]);
    }

    size_t pos = off;
    while (pos < code.size() && pos - off < len) {
        const size_t size = one(code, base, pos).second;
        if (size == 0) break;
        starts.push_back(pos);
        pos += size;
    }

    for (int i = 0; i < after && pos < code.size(); ++i) {
        const size_t size = one(code, base, pos).second;
        if (size == 0) break;
        starts.push_back(pos);
        pos += size;
    }

    std::string out;
    for (const size_t start : starts) {
        const std::pair<std::string, size_t> insn = one(code, base, start);
        std::string bytes = bytes_text(code, start, insn.second);
        if (bytes.size() < kBytesColumn) bytes.append(kBytesColumn - bytes.size(), ' ');
        out += addr_hex(base + start) + "  " + bytes + "  " + insn.first + "\n";
    }
    return out;
}

}  // namespace tc
