/* assemble.cc —— COFF → blob（接口与规则见 assemble.hh / docs/specs/2026-10-15-simplexc-interfaces.md §4）。
 *
 * 布局：`[.text][.rdata][.data][.bss][stubs]` 顺序拼接，全部按输入节的对齐要求对齐。
 * 产物内部只允许 PC 相对引用（REL32 家族）→ blob 被游戏 memcpy 到任意地址执行都成立。
 *
 * ⚠️ 一个刻意的取舍：节的顺序固定，不做「把 .text 放最后」。因为 stub 在 .text 之后，
 *    而条数在链接期才知道；把 stub 并进 .text 段尾比再开一段简单，且 REL32 一定够得着。
 */
#include "assemble.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace tc {
namespace {

/* ---------------------------------------------------------------- COFF 解析 */

enum Rel : uint16_t {
    REL_ABSOLUTE = 0x00,
    REL_ADDR64 = 0x01,
    REL_ADDR32 = 0x02,
    REL_ADDR32NB = 0x03,
    REL_REL32 = 0x04,      /* 到 REL32_5 = 0x09 */
    REL_REL32_5 = 0x09,
};

struct Reloc {
    uint32_t offset = 0;
    uint32_t sym = 0;
    uint16_t type = 0;
};

struct Section {
    std::string name;
    uint32_t chars = 0;
    uint64_t vsize = 0;                 /* .bss 用它当大小 */
    std::vector<uint8_t> data;
    std::vector<Reloc> relocs;
    uint32_t align = 16;
};

struct Sym {
    std::string name;
    int32_t sec = 0;                    /* 1 基；0 = 未定义；-1 = 绝对值；-2 = 调试 */
    uint32_t value = 0;
    uint8_t sclass = 0;
};

struct Obj {
    std::string name;
    std::vector<Section> secs;
    std::vector<Sym> syms;
};

uint32_t r16(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8); }
uint32_t r32(const uint8_t* p) {
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint32_t align_from_chars(uint32_t chars) {
    /* IMAGE_SCN_ALIGN_* 在 bits 20..23：1→1, 2→2, 3→4, 4→8, 5→16, 6→32, ..., 14→8192 */
    uint32_t code = (chars >> 20) & 0xF;
    if (code < 1 || code > 14) return 1;
    return 1u << (code - 1);
}

bool read_cstr(const std::vector<uint8_t>& b, size_t off, std::string& out) {
    if (off >= b.size()) return false;
    out.clear();
    while (off < b.size() && b[off] != 0) out.push_back(char(b[off++]));
    return true;
}

bool parse_coff(const ObjectFile& f, Obj& o, std::string& err) {
    const std::vector<uint8_t>& b = f.bytes;
    if (b.size() < 20) { err = "不足 20 字节，不是 COFF"; return false; }
    uint32_t machine = r16(&b[0]);
    if (machine != 0x8664) { err = "machine 不是 AMD64(0x8664)"; return false; }
    uint32_t nsec = r16(&b[2]);
    uint32_t psym = r32(&b[8]);
    uint32_t nsym = r32(&b[12]);
    if (nsec == 0) { err = "没有节"; return false; }
    if (20 + size_t(nsec) * 40 > b.size()) { err = "节表越界"; return false; }
    size_t strtab = size_t(psym) + size_t(nsym) * 18;   /* 字符串表（长名与长符号名都用它） */

    o.name = f.name;
    o.secs.reserve(nsec);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t* sh = &b[20 + size_t(i) * 40];
        Section s;
        if (sh[0] == '/') {                       /* 长名：/偏移 */
            std::string num(reinterpret_cast<const char*>(sh + 1), 7);
            size_t off = strtab + std::strtoul(num.c_str(), nullptr, 10);
            if (!read_cstr(b, off, s.name)) { err = "长节名越界"; return false; }
        } else {
            std::string raw(reinterpret_cast<const char*>(sh), 8);
            s.name = raw.c_str();
        }
        s.vsize = r32(sh + 8);
        uint32_t raw_size = r32(sh + 16);
        uint32_t raw_ptr = r32(sh + 20);
        uint32_t rel_ptr = r32(sh + 24);
        uint32_t nrel = r16(sh + 32);
        s.chars = r32(sh + 36);
        s.align = align_from_chars(s.chars);
        if (raw_size) {
            if (size_t(raw_ptr) + raw_size > b.size()) { err = "节内容越界：" + s.name; return false; }
            s.data.assign(b.begin() + raw_ptr, b.begin() + raw_ptr + raw_size);
        }
        if (nrel) {
            if (size_t(rel_ptr) + size_t(nrel) * 10 > b.size()) { err = "重定位表越界：" + s.name; return false; }
            for (uint32_t r = 0; r < nrel; r++) {
                const uint8_t* rp = &b[rel_ptr + size_t(r) * 10];
                s.relocs.push_back(Reloc{r32(rp), r32(rp + 4), uint16_t(r16(rp + 8))});
            }
        }
        o.secs.push_back(std::move(s));
    }

    if (psym && nsym) {
        if (size_t(psym) + size_t(nsym) * 18 > b.size()) { err = "符号表越界"; return false; }
        o.syms.reserve(nsym);
        for (uint32_t i = 0; i < nsym; i++) {
            const uint8_t* sp = &b[psym + size_t(i) * 18];
            Sym s;
            if (r32(sp) == 0) {                    /* 名字在字符串表里（偏移在 +4） */
                if (!read_cstr(b, strtab + r32(sp + 4), s.name)) { err = "长符号名越界"; return false; }
            } else {
                std::string raw(reinterpret_cast<const char*>(sp), 8);
                s.name = raw.c_str();
            }
            s.value = r32(sp + 8);
            s.sec = int16_t(r16(sp + 12));
            s.sclass = sp[16];
            o.syms.push_back(std::move(s));
            /* aux 记录占位保留：**重定位用的是原始符号表下标**，跳过它们会让下标整体错位
               （踩过：报「重定位指向的符号下标越界」）。 */
            for (uint8_t a = 0; a < sp[17]; a++) o.syms.push_back(Sym{});
            i += sp[17];                           /* 已通过占位推进，这里只跳循环计数 */
        }
    }
    return true;
}

/* -------------------------------------------------------------- 节的归并类 */

enum class Cls { Text, RData, Data, Bss, Drop };

Cls classify(const std::string& n) {
    /* GCC 会把 `.ident` 版本串放进 `.rdata$zzz`（64 字节垃圾，没人引用）—— 丢掉，别进产物 */
    if (n.find("$zzz") != std::string::npos) return Cls::Drop;
    if (n.rfind(".comment", 0) == 0 || n.rfind(".note", 0) == 0) return Cls::Drop;
    if (n.rfind(".text", 0) == 0) return Cls::Text;
    if (n.rfind(".rdata", 0) == 0) return Cls::RData;
    if (n.rfind(".data", 0) == 0) return Cls::Data;
    if (n.rfind(".bss", 0) == 0) return Cls::Bss;
    /* .pdata/.xdata 是 unwind（含 ADDR32NB），.debug* / .drectve / .llvm_addrsig 是元数据 */
    return Cls::Drop;
}

struct StubInfo {
    uint64_t addr = 0;
    bool resolved = false;
};

/* 未定义符号在产物里的落点，两种形态（都不是内部绝对地址，只是跳板）：
 *   · 普通函数  → 代码桩 `movabs rax,imm64 ; jmp rax`（12 字节，调用方 `call rel32` 到它）
 *   · `__imp_X` → **数据槽**（8 字节，内容 = X 的绝对地址）
 *     —— mingw 对 `__declspec(dllimport)` 的声明会生成 `call *__imp_X(%rip)`，
 *        它**解引用**目标地址，拿代码桩去顶会被当成指针值，必崩。 */
bool is_imp_slot(const std::string& name) { return name.rfind("__imp_", 0) == 0; }

}  // namespace

AsmResult assemble(const std::vector<ObjectFile>& objs,
                   std::string_view entry_symbol,
                   ExternResolver& resolver,
                   const AsmOptions& opt) {
    AsmResult res;
    std::vector<Obj> parsed;
    parsed.reserve(objs.size());
    for (const ObjectFile& f : objs) {
        Obj o;
        std::string err;
        if (!parse_coff(f, o, err)) {
            res.status = AsmStatus::BadObject;
            res.detail = f.name + "：" + err;
            return res;
        }
        parsed.push_back(std::move(o));
    }

    /* ---- 1. 定布局：每个保留下来的输入节拿到一个最终偏移 ---- */
    std::vector<std::vector<uint64_t>> sec_base(parsed.size());
    uint64_t off = 0;
    while (true) {
        bool any = false;
        for (const Cls want : {Cls::Text, Cls::RData, Cls::Data, Cls::Bss}) {
            for (size_t i = 0; i < parsed.size(); i++) {
                if (sec_base[i].empty()) sec_base[i].assign(parsed[i].secs.size(), UINT64_MAX);
                for (size_t s = 0; s < parsed[i].secs.size(); s++) {
                    if (classify(parsed[i].secs[s].name) != want) continue;
                    uint64_t a = parsed[i].secs[s].align ? parsed[i].secs[s].align : 1;
                    off = (off + a - 1) / a * a;
                    sec_base[i][s] = off;
                    uint64_t sz = parsed[i].secs[s].data.size();
                    if (classify(parsed[i].secs[s].name) == Cls::Bss) {
                        sz = parsed[i].secs[s].vsize ? parsed[i].secs[s].vsize
                                                     : parsed[i].secs[s].data.size();
                    }
                    off += sz;
                    any = true;
                }
            }
        }
        if (!any) break;
        break;   /* 一轮就够：类内顺序 = 对象顺序 */
    }
    uint64_t body_end = off;

    /* ---- 2. 全局符号表（已定义的名字 → 最终偏移）---- */
    std::unordered_map<std::string, uint64_t> defined;
    std::vector<std::vector<uint64_t>> sym_addr(parsed.size());
    for (size_t i = 0; i < parsed.size(); i++) {
        sym_addr[i].assign(parsed[i].syms.size(), UINT64_MAX);
        for (size_t k = 0; k < parsed[i].syms.size(); k++) {
            const Sym& s = parsed[i].syms[k];
            if (s.sec <= 0) continue;                       /* 未定义 / 绝对值 / 调试 */
            size_t idx = size_t(s.sec - 1);
            if (idx >= parsed[i].secs.size()) continue;
            if (sec_base[i][idx] == UINT64_MAX) continue;   /* 所在节被丢弃 */
            uint64_t a = sec_base[i][idx] + s.value;
            sym_addr[i][k] = a;
            /* 节符号（`.text`/`.data`/`.bss`…）与 `@feat.00` 每个目标文件都有一份，
               但它们不是真正的链接符号—— 放进全局表会被当成重复定义。 */
            if (!s.name.empty() && s.name[0] != '.' && s.name[0] != '@') {
                auto it = defined.find(s.name);
                if (it != defined.end() && it->second != a) {
                    res.status = AsmStatus::BadObject;
                    res.detail = "符号重复定义：" + s.name;
                    return res;
                }
                defined[s.name] = a;
            }
        }
    }

    /* ---- 3. 未定义符号的落点：先扫一遗重定位决定要造哪些桩/槽 ---- */
    std::vector<uint8_t> stubs;      /* 代码桩 */
    std::vector<uint8_t> slots;      /* __imp_ 数据槽 */
    std::unordered_map<std::string, StubInfo> place_of;
    bool unresolved = false;
    std::string unresolved_name;
    auto place_for = [&](const std::string& name, uint64_t& addr) -> bool {
        auto it = place_of.find(name);
        if (it != place_of.end()) { addr = it->second.addr; return it->second.resolved; }
        uint64_t target = 0;
        bool ok = resolver.resolve(name, target);
        StubInfo info{};
        if (is_imp_slot(name)) {
            size_t pad = (8 - (slots.size() & 7)) & 7;
            slots.insert(slots.end(), pad, 0);
            info.addr = body_end + stubs.size() + slots.size();
            for (int k = 0; k < 8; k++) slots.push_back(uint8_t((target >> (8 * k)) & 0xFF));
        } else {
            size_t pad = (16 - (stubs.size() & 15)) & 15;
            stubs.insert(stubs.end(), pad, 0x90);          /* nop 填充，保持 16 对齐 */
            info.addr = body_end + stubs.size();
            if (ok) {
                stubs.push_back(0x48); stubs.push_back(0xB8);   /* movabs rax, imm64 */
                for (int k = 0; k < 8; k++) stubs.push_back(uint8_t((target >> (8 * k)) & 0xFF));
                stubs.push_back(0xFF); stubs.push_back(0xE0);   /* jmp rax */
            }
        }
        info.resolved = ok;
        if (ok) res.stub_count++;
        place_of.emplace(name, info);
        addr = info.addr;
        return ok;
    };

    for (size_t i = 0; i < parsed.size(); i++) {
        const Obj& o = parsed[i];
        for (size_t s = 0; s < o.secs.size(); s++) {
            if (sec_base[i][s] == UINT64_MAX) continue;
            for (const Reloc& r : o.secs[s].relocs) {
                if (r.sym >= o.syms.size()) continue;
                const std::string& nm = o.syms[r.sym].name;
                if (sym_addr[i][r.sym] != UINT64_MAX) continue;       /* 本目标文件里定义的 */
                if (!nm.empty() && defined.count(nm)) continue;        /* 别的目标文件里定义的 */
                uint64_t dummy = 0;
                if (!place_for(nm, dummy) && !unresolved) {
                    unresolved = true;
                    unresolved_name = nm;
                }
            }
        }
    }
    if (unresolved) {
        res.status = AsmStatus::UndefinedSymbol;
        res.detail = unresolved_name;
        return res;
    }

    /* ---- 4. 拼 payload（含桩与槽）并应用重定位 ---- */
    const uint64_t stubs_base = body_end;
    const uint64_t slots_base = body_end + stubs.size();
    std::vector<uint8_t> payload(body_end + stubs.size() + slots.size(), 0);
    std::memcpy(payload.data() + stubs_base, stubs.data(), stubs.size());
    std::memcpy(payload.data() + slots_base, slots.data(), slots.size());
    for (size_t i = 0; i < parsed.size(); i++) {
        for (size_t s = 0; s < parsed[i].secs.size(); s++) {
            if (sec_base[i][s] == UINT64_MAX || parsed[i].secs[s].data.empty()) continue;
            std::memcpy(payload.data() + sec_base[i][s], parsed[i].secs[s].data.data(),
                        parsed[i].secs[s].data.size());
        }
    }
    /* 统计 + 重定位。注意：先拷贝全部内容，再打补丁 —— 否则跨节的补丁会互相踩。 */
    for (size_t i = 0; i < parsed.size(); i++) {
        const Obj& o = parsed[i];
        for (size_t s = 0; s < o.secs.size(); s++) {
            const Section& sec = o.secs[s];
            uint64_t base = sec_base[i][s];
            if (base != UINT64_MAX) {
                uint64_t sz = sec.data.size();
                switch (classify(sec.name)) {
                    case Cls::Text:  res.text_size += sz; break;
                    case Cls::RData: res.rdata_size += sz; break;
                    case Cls::Data:  res.data_size += sz; break;
                    case Cls::Bss:   res.bss_size += (sec.vsize ? sec.vsize : sz); break;
                    case Cls::Drop:  break;
                }
            }
            if (base == UINT64_MAX) continue;                 /* 丢弃的节连同它的重定位一起丢 */
            for (const Reloc& r : sec.relocs) {
                res.reloc_count++;
                if (r.type < REL_REL32 || r.type > REL_REL32_5) {
                    char buf[128];
                    std::snprintf(buf, sizeof buf, "%s+0x%X 的重定位类型 0x%X 不支持",
                                  sec.name.c_str(), r.offset, unsigned(r.type));
                    res.status = AsmStatus::UnsupportedReloc;
                    res.detail = buf;
                    return res;
                }
                if (r.sym >= o.syms.size()) {
                    res.status = AsmStatus::BadObject;
                    res.detail = "重定位指向的符号下标越界";
                    return res;
                }
                uint64_t target = UINT64_MAX;
                const std::string& nm = o.syms[r.sym].name;
                if (sym_addr[i][r.sym] != UINT64_MAX) {
                    target = sym_addr[i][r.sym];
                } else {
                    auto d = defined.find(nm);          /* 跨目标文件：先查全局表 */
                    if (d != defined.end()) {
                        target = d->second;
                    } else {
                        auto it = place_of.find(nm);   /* 再退到 extern 桩/槽 */
                        if (it == place_of.end()) {
                            res.status = AsmStatus::UndefinedSymbol;
                            res.detail = nm;
                            return res;
                        }
                        target = it->second.addr;
                    }
                }
                const uint64_t site = base + r.offset;
                if (site + 4 > payload.size()) {
                    res.status = AsmStatus::BadObject;
                    res.detail = "重定位落在节外";
                    return res;
                }
                uint32_t addend = r32(payload.data() + site);
                /* COFF：值 = S + A - (P + 4 + n)，n 由 REL32_n 变体给出 */
                int64_t delta = int64_t(target) + int64_t(addend) -
                                int64_t(site + 4 + (r.type - REL_REL32));
                if (delta < INT32_MIN || delta > INT32_MAX) {
                    res.status = AsmStatus::UnsupportedReloc;
                    res.detail = "REL32 超出 ±2GB（blob 太大？）";
                    return res;
                }
                int32_t d32 = int32_t(delta);
                for (int k = 0; k < 4; k++) payload[site + k] = uint8_t((d32 >> (8 * k)) & 0xFF);
            }
        }
    }

    /* ---- 5. entry ---- */
    auto e = defined.find(std::string(entry_symbol));
    if (e == defined.end()) {
        res.status = AsmStatus::UndefinedSymbol;
        res.detail = "entry 符号未定义：" + std::string(entry_symbol);
        return res;
    }
    res.blob.entry_off = e->second;

    /* ---- 6. 头 + 机器码 ---- */
    uint64_t out_len = 8 + payload.size();
    if (out_len > opt.max_blob) {
        res.status = AsmStatus::BadObject;
        res.detail = "产物超过上限";
        return res;
    }
    res.blob.bytes.resize(out_len);
    uint64_t header = out_len | (1ull << 62);              /* bit62 → 游戏不 free（主设计 §2.3） */
    for (int k = 0; k < 8; k++) res.blob.bytes[k] = uint8_t((header >> (8 * k)) & 0xFF);
    std::memcpy(res.blob.bytes.data() + 8, payload.data(), payload.size());

    if (opt.verbose) {
        std::fprintf(stderr,
                     "[assemble] text=%llu rdata=%llu data=%llu bss=%llu stubs=%u relocs=%u\n"
                     "           out_len=%llu entry=0x%llX\n",
                     (unsigned long long)res.text_size, (unsigned long long)res.rdata_size,
                     (unsigned long long)res.data_size, (unsigned long long)res.bss_size,
                     res.stub_count, res.reloc_count,
                     (unsigned long long)out_len, (unsigned long long)res.blob.entry_off);
    }
    res.status = AsmStatus::Ok;
    return res;
}

}  // namespace tc
