/* assemble.cc —— COFF → blob（接口与规则见 assemble.hh / docs/specs/2026-10-15-simplexc-interfaces.md §4）。
 *
 * 布局：`[.text][.rdata][.data][.bss][stubs]` 顺序拼接，全部按输入节的对齐要求对齐。
 * 产物内部只允许 PC 相对引用（REL32 家族）→ blob 被游戏 memcpy 到任意地址执行都成立。
 *
 * ⚠️ 一个刻意的取舍：节的顺序固定，不做「把 .text 放最后」。因为 stub 在 .text 之后，
 *    而条数在链接期才知道；把 stub 并进 .text 段尾比再开一段简单，且 REL32 一定够得着。
 */
#include "assemble.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace tc {
namespace {

/* ---------------------------------------------------------------- COFF 解析 */

enum Rel : uint16_t {
    REL_ABSOLUTE = 0x00,
    REL_ADDR64 = 0x01,     /* 值 = S + A（64 位绝对值，**没有** PC 相对项）*/
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
    uint64_t size = 0;                  /* 节在产物里占的字节数（规则见 parse_coff） */
    std::vector<uint8_t> data;          /* 文件里的原始数据；没有原始数据的节（.bss）为空 */
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

uint64_t r64(const uint8_t* p) { return uint64_t(r32(p)) | (uint64_t(r32(p + 4)) << 32); }

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
        uint32_t vsize = r32(sh + 8);
        uint32_t raw_size = r32(sh + 16);
        uint32_t raw_ptr = r32(sh + 20);
        uint32_t rel_ptr = r32(sh + 24);
        uint32_t nrel = r16(sh + 32);
        s.chars = r32(sh + 36);
        s.align = align_from_chars(s.chars);
        /* 节在产物里占多少字节。COFF **对象文件**里 `VirtualSize` 是 0（实测 clang 与 gcc
           都如此），大小在 `SizeOfRawData`。 */
        s.size = (std::max)(uint64_t(raw_size), uint64_t(vsize));
        /* `PointerToRawData == 0` 的节在文件里**没有原始数据**（`.bss` 的标准形态，内容隐含
           全 0）—— 绝不能读：偏移 0 是 COFF 头（含编译时间戳），照读会把头和编译时刻当数据
           搬进产物，于是同一源码两次编译的产物不再逐字节相同、`.bss` 的初值成了垃圾而不是 0
           （`docs/notes/2026-10-17-assemble-bss.md`）。留空即可：payload 本来就是零初始化的，
           上面 `size` 字节的零就是它该有的内容。 */
        if (raw_size && raw_ptr) {
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

/* 未定义符号在产物里的落点，两种形态（都不是内部绝对地址，只是跳板）：
 *   · 普通函数  → 代码桩 `movabs rax,imm64 ; jmp rax`（12 字节，调用方 `call rel32` 到它）
 *   · `__imp_X` → **数据槽**（8 字节，内容 = X 的绝对地址）
 *     —— mingw 对 `__declspec(dllimport)` 的声明会生成 `call *__imp_X(%rip)`，
 *        它**解引用**目标地址，拿代码桩去顶会被当成指针值，必崩。 */
bool is_imp_slot(const std::string& name) { return name.rfind("__imp_", 0) == 0; }

/* `Cls` → `AsmResult::SectionPlacement::cls` 的拼写（`--offsets-out` 的 JSON 直接用）。
 * `Drop` 不会出现在布局表里（它没有 blob 偏移），所以这里不给它一个名字。 */
const char* cls_name(Cls c) {
    switch (c) {
        case Cls::Text: return "text";
        case Cls::RData: return "rdata";
        case Cls::Data: return "data";
        case Cls::Bss: return "bss";
        case Cls::Drop: break;
    }
    return "";
}

/* 桩/槽的字节形态只有一处定义：造的时候按这里写，返回前的自校验按这里查。 */
constexpr size_t kCodeStubLen = 12;    /* `48 B8 <imm64> FF E0` */
constexpr size_t kImpSlotLen = 8;      /* 8 字节绝对地址 */

void write_le64(uint8_t* p, uint64_t v) {
    for (int k = 0; k < 8; k++) p[k] = uint8_t((v >> (8 * k)) & 0xFF);
}

void encode_code_stub(uint64_t target, uint8_t* out) {
    out[0] = 0x48; out[1] = 0xB8;                  /* movabs rax, imm64 */
    write_le64(out + 2, target);
    out[10] = 0xFF; out[11] = 0xE0;                /* jmp rax */
}

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
                    off += parsed[i].secs[s].size;
                    any = true;
                }
            }
        }
        if (!any) break;
        break;   /* 一轮就够：类内顺序 = 对象顺序 */
    }
    uint64_t body_end = off;
    res.body_end = body_end;
    res.stubs_base = body_end; /* 代码桩块就排在各节之后：两者按定义相等 */

    /* 布局表（`--offsets-out` 的数据源）与「哪些对象带 `.debug_line`」。节表只列**进了产物**
     * 的节；`.debug*` 被归为 Drop、不占 blob 偏移，所以它们的行表只能靠锚点（对象 `.text`
     * 的 blob 偏移）重定位。 */
    for (size_t i = 0; i < parsed.size(); i++) {
        bool has_line = false;
        for (size_t s = 0; s < parsed[i].secs.size(); s++) {
            const Section& sec = parsed[i].secs[s];
            if (sec.name.rfind(".debug_line", 0) == 0) has_line = true;
            if (sec_base[i][s] == UINT64_MAX) continue;
            /* 零字节的节不进表：它不占产物字节，而它的 `blob_off` 会与下一个非空节重合
             * （对齐后再摆、长度 0）—— 留着只会让「按 `blob_off` 升序且不重叠」这条不变量
             * 在零长度条目上不成立。 */
            if (sec.size == 0) continue;
            res.sections.push_back(AsmResult::SectionPlacement{
                static_cast<uint32_t>(i), static_cast<uint32_t>(s + 1), sec.name,
                cls_name(classify(sec.name)), sec_base[i][s], sec.size, sec.align});
        }
        if (has_line) res.debug_line_objs.push_back(static_cast<uint32_t>(i));
    }
    /* 遍历序是 (对象, 节)，而布局先按类分组 → 上面push进来的次序不是 blob 序。消费方
     * （W3）靠这个升序二分，所以在这里排完再出去。 */
    std::sort(res.sections.begin(), res.sections.end(),
              [](const AsmResult::SectionPlacement& a, const AsmResult::SectionPlacement& b) {
                  return a.blob_off < b.blob_off;
              });

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

    /* ---- 3. 未定义符号的落点：**收集与定址分开两趟** ----
     *
     * 一趟里增量算地址是错的：槽排在全部桩之后，而「桩总共多长」要扫完所有重定位才知道。
     * 旧实现用 `body_end + stubs.size() + slots.size()` 当场算槽地址，之后扫到的裸名 extern
     * 再往桩块追加字节，`stubs.size()` 变大 —— 已记下的槽地址整体前移，真身落到别处。后果是
     * `<blob>.stubs` sidecar 与产物字节不一致：宿主/壳照 sidecar 去 patch，写错位置（实测 sidecar
     * 说槽在 ae0/ae8/af0、真身在 afc/b04/b0c，宿主崩在 entry+0x14E）。触发条件 = `__imp_*` 引用
     * 排在裸名 extern 引用之前（`docs/notes/2026-10-17-assemble-slot-addr.md`）。
     */
    struct Pending {
        std::string name;
        uint64_t target = 0;    /* resolver 解析出的绝对地址 */
        uint64_t addr = 0;      /* 桩/槽的落点，造完才知道 */
    };
    /* 顺序 = 首次引用顺序（确定性）：sidecar 的条目顺序与产物字节都由它决定。 */
    std::vector<Pending> pending;
    std::unordered_map<std::string, size_t> pending_idx;
    bool unresolved = false;
    std::string unresolved_name;
    for (size_t i = 0; i < parsed.size(); i++) {
        const Obj& o = parsed[i];
        for (size_t s = 0; s < o.secs.size(); s++) {
            if (sec_base[i][s] == UINT64_MAX) continue;
            for (const Reloc& r : o.secs[s].relocs) {
                if (r.sym >= o.syms.size()) continue;
                const std::string& nm = o.syms[r.sym].name;
                if (sym_addr[i][r.sym] != UINT64_MAX) continue;       /* 本目标文件里定义的 */
                if (!nm.empty() && defined.count(nm)) continue;        /* 别的目标文件里定义的 */
                if (pending_idx.count(nm)) continue;                   /* 去重 */
                Pending p;
                p.name = nm;
                if (!resolver.resolve(nm, p.target) && !unresolved) {
                    unresolved = true;
                    unresolved_name = nm;
                }
                pending_idx.emplace(nm, pending.size());
                pending.push_back(std::move(p));
            }
        }
    }
    if (unresolved) {
        res.status = AsmStatus::UndefinedSymbol;
        res.detail = unresolved_name;
        return res;
    }

    /* 造桩与槽：代码桩在前、`__imp_` 数据槽在后，各自内部保持首次引用顺序（与旧实现一致）；
       地址在这里一次算完，之后就只读 —— 后面没有任何东西能让它再动。*/
    std::vector<uint8_t> stubs;      /* 代码桩 */
    std::vector<uint8_t> slots;      /* __imp_ 数据槽 */
    for (Pending& p : pending) {
        if (is_imp_slot(p.name)) continue;
        size_t pad = (16 - (stubs.size() & 15)) & 15;
        stubs.insert(stubs.end(), pad, 0x90);          /* nop 填充，保持 16 对齐 */
        p.addr = body_end + stubs.size();
        uint8_t code[kCodeStubLen];
        encode_code_stub(p.target, code);
        stubs.insert(stubs.end(), code, code + kCodeStubLen);
    }
    const uint64_t slots_base = body_end + stubs.size();
    res.slots_base = slots_base;
    for (Pending& p : pending) {
        if (!is_imp_slot(p.name)) continue;
        size_t pad = (8 - (slots.size() & 7)) & 7;
        slots.insert(slots.end(), pad, 0);
        p.addr = slots_base + slots.size();
        uint8_t bin[kImpSlotLen];
        write_le64(bin, p.target);
        slots.insert(slots.end(), bin, bin + kImpSlotLen);
    }
    /* 名字 → 落点 = 重定位要用的表；`res.stubs` 是 sidecar 的内容，顺序仍是首次引用顺序。 */
    std::unordered_map<std::string, uint64_t> place_of;
    for (const Pending& p : pending) {
        place_of.emplace(p.name, p.addr);
        res.stub_count++;
        /* `cli/tcblob.cc` 拿它写 `<blob>.stubs` sidecar，宿主靠它认出「这处 movabs 是 extern
           跳板，不是游戏地址」并把地址换成宿主桩。 */
        res.stubs.push_back(StubRef{p.addr, p.name});
    }

    /* ---- 4. 拼 payload（含桩与槽）并应用重定位 ---- */
    const uint64_t stubs_base = body_end;
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
                uint64_t sz = sec.size;
                switch (classify(sec.name)) {
                    case Cls::Text:  res.text_size += sz; break;
                    case Cls::RData: res.rdata_size += sz; break;
                    case Cls::Data:  res.data_size += sz; break;
                    case Cls::Bss:   res.bss_size += sz; break;
                    case Cls::Drop:  break;
                }
            }
            if (base == UINT64_MAX) continue;                 /* 丢弃的节连同它的重定位一起丢 */
            for (const Reloc& r : sec.relocs) {
                res.reloc_count++;
                if (r.type != REL_ADDR64 && (r.type < REL_REL32 || r.type > REL_REL32_5)) {
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
                        target = it->second;
                    }
                }
                const uint64_t site = base + r.offset;
                if (site + (r.type == REL_ADDR64 ? 8 : 4) > payload.size()) {
                    res.status = AsmStatus::BadObject;
                    res.detail = "重定位落在节外";
                    return res;
                }
                if (r.type == REL_ADDR64) {
                    /* 兜底分支：参考产物里没有 ADDR64，正常路径是「switch 查表走**相对表**」——
                       `@switch.table.rel = [N x i32]` + `llvm.load.relative` 降成
                       `lea table(%rip) ; movslq (%rt,%ri,4) ; add %rt`，表在 `.text` 内、零重定位。
                       真出了 ADDR64 就说明有指针常量被摊成了绝对表（LLVM 用「节符号 + 偏置」的
                       8 字节写法，如 `@.rdata + 0x38`）：这里补进去的只是 **blob 内偏移**，运行期
                       没有基址可加，产物装得出但跑不了（142 踩过，见 `work/tmp/rel/notes.md`）。
                       留着它只为「装得出来、偏移填对」，好让症状落到运行期而不是卡在装配。 */
                    const uint64_t value = target + r64(payload.data() + site);
                    write_le64(payload.data() + site, value);
                    continue;
                }
                /* COFF 的 addend **是有符号的**：GCC 会用 -1 表示「字面量地址减一」的偏置
                   （它的复制循环再从 +1 读回），当无符号算会直接溢出 ±2GB。 */
                int32_t addend = int32_t(r32(payload.data() + site));
                /* COFF：值 = S + A - (P + 4 + n)，n 由 REL32_n 变体给出 */
                int64_t delta = int64_t(target) + int64_t(addend) -
                                int64_t(site + 4 + (r.type - REL_REL32));
                if (delta < INT32_MIN || delta > INT32_MAX) {
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                                  "%s+0x%X 的 REL32 超出 ±2GB：符号 %s target=0x%llX "
                                  "site=0x%llX addend=%d",
                                  sec.name.c_str(), r.offset, nm.c_str(),
                                  (unsigned long long)target, (unsigned long long)site, addend);
                    res.status = AsmStatus::UnsupportedReloc;
                    res.detail = buf;
                    return res;
                }
                int32_t d32 = int32_t(delta);
                for (int k = 0; k < 4; k++) payload[site + k] = uint8_t((d32 >> (8 * k)) & 0xFF);
            }
        }
    }

    /* ---- 5. 自校验：`res.stubs`（= `<blob>.stubs` sidecar 的内容）逐条与产物字节核对 ----
     *
     * 这个 bug 之所以致命，正是因为产物与 sidecar 说的不一样却没人查：宿主照 sidecar 去 patch，
     * 写错位置，崩在游戏里。宁可装配失败，也不要发一份对不上的产物 —— 不通就报 BadObject 并写清
     * 是哪个符号、哪个偏移，别让下游去猜。核对的是 `res.stubs` 本身（也就是 sidecar 会写出去
     * 的那份数据），不是内部变量。
     */
    for (const StubRef& s : res.stubs) {
        const Pending& p = pending[pending_idx.at(s.symbol)];
        const bool slot = is_imp_slot(s.symbol);
        const size_t len = slot ? kImpSlotLen : kCodeStubLen;
        uint8_t want[kCodeStubLen];
        if (slot) write_le64(want, p.target);
        else encode_code_stub(p.target, want);
        char buf[256];
        if (s.offset + len > payload.size()) {
            std::snprintf(buf, sizeof buf, "自校验失败：%s 的%s落点 0x%llX 越出产物（产物 %llu 字节）",
                          s.symbol.c_str(), slot ? "槽" : "桩",
                          (unsigned long long)s.offset, (unsigned long long)payload.size());
            res.status = AsmStatus::BadObject;
            res.detail = buf;
            return res;
        }
        if (std::memcmp(payload.data() + s.offset, want, len) != 0) {
            std::snprintf(buf, sizeof buf,
                          "自校验失败：%s 的%s在 0x%llX，产物该处是 %02X %02X …（应为 %02X %02X …）"
                          "—— sidecar 与产物不是一回事",
                          s.symbol.c_str(), slot ? "槽" : "桩", (unsigned long long)s.offset,
                          payload[s.offset], payload[s.offset + 1], want[0], want[1]);
            res.status = AsmStatus::BadObject;
            res.detail = buf;
            return res;
        }
    }

    /* ---- 6. entry ---- */
    auto e = defined.find(std::string(entry_symbol));
    if (e == defined.end()) {
        res.status = AsmStatus::UndefinedSymbol;
        res.detail = "entry 符号未定义：" + std::string(entry_symbol);
        return res;
    }
    res.blob.entry_off = e->second;

    /* ---- 7. 头 + 机器码 ---- */
    uint64_t out_len = 8 + payload.size();
    if (out_len > opt.max_blob) {
        res.status = AsmStatus::TooLarge;
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
