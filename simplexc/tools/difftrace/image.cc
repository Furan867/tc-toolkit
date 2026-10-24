#include "image.hh"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spec.hh"

namespace {

// host.py 的同名常量：`PAGE` 是「地址」与「DLL 内的偏移」之间的粒度，`BAND` 是「像游戏地址」的
// 邻域宽度。投票判据 `(v - t) % PAGE == 0` 与「落在游戏段附近的立即数不参与投票」都靠这两个数。
constexpr int64_t kPage = 0x10000;
constexpr uint64_t kBand = 1 << 26;

constexpr uint32_t kScnMemExecute = 0x20000000;

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("读不了 " + path);
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw std::runtime_error("读不了 " + path);
    return out;
}

uint64_t le_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t le_u32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                                          static_cast<uint32_t>(p[2]) << 16 |
                                          static_cast<uint32_t>(p[3]) << 24; }

uint16_t le_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | static_cast<uint16_t>(p[1]) << 8);
}

std::string hex(uint64_t v) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << v;
    return out.str();
}

// 导入名只保留过程名：`KERNEL32.dll!Sleep` → `Sleep`。host.py 的 handler 表也是按过程名索引的
// （`detour_name` 只去 `__imp_` 前缀，DLL 名它根本不带进来）。
std::string proc_name(const std::string& full) {
    const auto bang = full.find('!');
    return bang == std::string::npos ? full : full.substr(bang + 1);
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = line.find('\t', start);
        if (at == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, at - start));
        start = at + 1;
    }
}

// 严过 Python 的 `int(s, 16)` 只严在一点：空串与夹杂非法字符直接判假（不回绕到 0）。
bool parse_hex(const std::string& s, uint64_t& out) {
    size_t i = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    if (i >= s.size()) return false;
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint64_t d = 0;
        if (c >= '0' && c <= '9')
            d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<uint64_t>(c - 'A' + 10);
        else
            return false;
        if (v > (UINT64_MAX - d) / 16) return false;
        v = v * 16 + d;
    }
    out = v;
    return true;
}

// `<blob>.stubs` → `StubRef`。语义照抄 host.py 的 `read_stub_sidecar`（含它“宁可退出”的那几条）：
// 表头必须正好是 `offset/kind/symbol`，每行三列、offset 十六进制、kind 只有 stub / slot，
// 一不对就抛 —— sidecar 写错时静默跳过等于拿一个烤进来的 `kernel32!Sleep` 地址去跑。
// 行尾 `\r` 要剥：Python 是文本模式（通用换行）读的，这些文件实际是 CRLF。
// 缺文件 = 老产物（没有 sidecar），返回空表；文件在但读不了就抛（read_file 同）。
std::vector<StubRef> read_stub_sidecar(const std::string& path, uint64_t code_size) {
    if (!std::filesystem::exists(path)) return {};
    const std::vector<uint8_t> raw = read_file(path);
    const std::string text(raw.begin(), raw.end());
    std::vector<StubRef> out;
    bool header = false;
    size_t from = 0;
    while (from <= text.size()) {
        const size_t nl = text.find('\n', from);
        std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        from = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const std::vector<std::string> row = split_tabs(line);
        if (!header) {
            if (row.size() != 3 || row[0] != "offset" || row[1] != "kind" || row[2] != "symbol")
                throw std::runtime_error(path + ": 表头不是 offset/kind/symbol");
            header = true;
            continue;
        }
        if (row.size() != 3) throw std::runtime_error(path + ": 这一行列数不是 3：" + line);

        uint64_t off = 0;
        if (!parse_hex(row[0], off))
            throw std::runtime_error(path + ": offset 不是十六进制：" + row[0]);
        StubKind kind = StubKind::Slot;
        if (row[1] == "stub")
            kind = StubKind::Code;
        else if (row[1] != "slot")
            throw std::runtime_error(path + ": 未知 kind " + row[1]);

        // `__imp_X` → `X`：与 `Thunk::import` 同一拼法（host.py 的 `detour_name` 同规则）。
        const std::string& sym = row[2];
        const std::string import = sym.compare(0, 6, "__imp_") == 0 ? sym.substr(6) : sym;
        if (import.empty()) throw std::runtime_error(path + ": 符号名为空：" + line);

        // 改写的落点：代码桩是 `movabs rax,imm64` 的 imm64（指令起点 +2），数据槽是它自己。
        const uint64_t at = off + (kind == StubKind::Code ? 2 : 0);
        if (at + 8 > code_size)
            throw std::runtime_error(path + ": " + hex(off) + " 的 8 字节落在产物外（code " +
                                     std::to_string(code_size) + " B）");
        out.push_back({off, kind, import});
    }
    if (!header) throw std::runtime_error(path + ": 表头不是 offset/kind/symbol");
    return out;
}

// DLL 的导入表 + 可执行节里的 `jmp QWORD PTR [rip+rel32]` 跳板，语义照抄 `abs_scan.Pe`。
//
// 为什么非读真 DLL 不可：产物里的 `movabs` 存的是**捕获那一 boot 的** `baked + RVA`，
// 而 RVA 是不是导入（进而叫什么名字）只有 DLL 的表能回答；baked 本身也要靠「v - thunk_rva
// 是 64 KB 的整数倍」投票才能反推（只靠值域 + 对齐有多个候选，实测 rv32im 是 5 个）。
struct PeImage {
    struct Section {
        uint32_t va, vsize, raw_ptr, raw_size, chars;
    };

    std::vector<uint8_t> data;
    std::vector<Section> sections;
    uint32_t image_size = 0;
    uint64_t opt_ = 0;  // 可选头起点（`DataDirectory` 在它 +112 处）
    std::unordered_map<uint64_t, std::string> slot_name;   // IAT 槽 RVA → `dll!proc`
    std::unordered_map<uint64_t, std::string> thunk_name;  // 跳板 RVA → `dll!proc`
    std::unordered_map<uint64_t, uint64_t> thunk_slot;     // 跳板 RVA → IAT 槽 RVA

    static PeImage load(const std::string& path) {
        PeImage pe;
        pe.data = read_file(path);
        const std::vector<uint8_t>& d = pe.data;
        if (d.size() < 0x40) throw std::runtime_error(path + " 不是 PE 文件");
        const uint64_t e_lfanew = le_u32(&d[0x3C]);
        if (e_lfanew + 24 + 2 > d.size() || d[e_lfanew] != 'P' || d[e_lfanew + 1] != 'E' ||
            d[e_lfanew + 2] != 0 || d[e_lfanew + 3] != 0)
            throw std::runtime_error(path + " 不是 PE 文件");
        const uint16_t n_sections = le_u16(&d[e_lfanew + 6]);
        const uint16_t opt_size = le_u16(&d[e_lfanew + 20]);
        const uint64_t opt = e_lfanew + 24;
        if (opt + opt_size > d.size()) throw std::runtime_error(path + " 的可选头越界");
        if (le_u16(&d[opt]) != 0x20B) throw std::runtime_error(path + " 不是 PE32+");
        pe.image_size = le_u32(&d[opt + 56]);

        pe.opt_ = opt;
        uint64_t off = opt + opt_size;
        for (uint16_t i = 0; i < n_sections; ++i, off += 40) {
            if (off + 40 > d.size()) throw std::runtime_error(path + " 的节表越界");
            pe.sections.push_back({le_u32(&d[off + 12]), le_u32(&d[off + 8]), le_u32(&d[off + 20]),
                                    le_u32(&d[off + 16]), le_u32(&d[off + 36])});
        }

        pe.read_imports();
        pe.scan_thunks();
        return pe;
    }

    // `LoadLibrary` 之后那块内存的字节：头 + 各节按 VA 摆放，节外与 `.bss` 保持 0。
    // 只读 `raw_ptr` 起 `min(raw_size, vsize)` 字节 —— `raw_ptr == 0` 是「这个节没有原始数据」
    // （`.bss` 的标准形态），按 rawptr 读会读到文件开头的 COFF 头。
    std::vector<uint8_t> memory_image() const {
        std::vector<uint8_t> out(image_size, 0);
        const uint64_t headers = std::min<uint64_t>(le_u32(&data[opt_ + 60]), out.size());
        std::copy(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(headers), out.begin());
        for (const Section& s : sections) {
            const uint64_t len = std::min<uint64_t>(s.raw_size, std::max(s.vsize, s.raw_size));
            if (len == 0) continue;
            if (s.va + len > out.size() || s.raw_ptr + len > data.size())
                throw std::runtime_error("DLL 的节 " + std::to_string(s.va) + " 越界");
            std::copy_n(data.begin() + s.raw_ptr, len, out.begin() + s.va);
        }
        return out;
    }

    // 整张导入表：IAT 槽 RVA → 导入名。DLL 自己的代码调的那些导入也在这里面。
    std::vector<Thunk> iat_table(uint64_t base) const {
        std::vector<Thunk> out;
        out.reserve(slot_name.size());
        for (const auto& [rva, full] : slot_name) out.push_back({base + rva, proc_name(full)});
        std::sort(out.begin(), out.end(), [](const Thunk& a, const Thunk& b) {
            return a.slot_va < b.slot_va;
        });
        return out;
    }

private:
    // 与 `abs_scan.Pe.rva_to_off` 同一判定顺序：落在节的虚拟范围内但超出文件字节的那一段
    // **返回空**（不是「查下一节」）—— 那个 RVA 确实没有可读的字节。
    std::optional<uint64_t> rva_to_off(uint32_t rva) const {
        for (const Section& s : sections) {
            const uint32_t span = std::max(s.vsize, s.raw_size);
            if (rva < s.va || rva - s.va >= span) continue;
            const uint32_t delta = rva - s.va;
            if (delta >= s.raw_size) return std::nullopt;
            const uint64_t off = static_cast<uint64_t>(s.raw_ptr) + delta;
            if (off >= data.size()) return std::nullopt;
            return off;
        }
        return std::nullopt;
    }

    std::string cstr(uint64_t off) const {
        std::string out;
        for (uint64_t i = off; i < data.size() && data[i] != 0; ++i)
            out.push_back(static_cast<char>(data[i]));
        return out;
    }

    void read_imports() {
        const uint32_t imp_rva = le_u32(&data[opt_ + 112 + 8]);
        const uint32_t imp_size = le_u32(&data[opt_ + 112 + 12]);
        if (!imp_rva) return;
        for (uint32_t i = 0; i < imp_size / 20; ++i) {
            const auto base = rva_to_off(imp_rva + i * 20);
            if (!base) break;
            const uint32_t ilt = le_u32(&data[*base]);
            const uint32_t name_rva = le_u32(&data[*base + 12]);
            const uint32_t iat_rva = le_u32(&data[*base + 16]);
            if (!name_rva && !iat_rva) break;
            if (!iat_rva) continue;
            const auto dll_off = rva_to_off(name_rva);
            const std::string dll = dll_off ? cstr(*dll_off) : "?";
            const auto src = rva_to_off(ilt ? ilt : iat_rva);
            if (!src) continue;
            for (uint32_t k = 0; k < 4096; ++k) {
                const uint64_t at = *src + static_cast<uint64_t>(k) * 8;
                if (at + 8 > data.size()) break;
                const uint64_t v = le_u64(&data[at]);
                if (v == 0) break;
                std::string name;
                if (v >> 63) {
                    name = "#" + std::to_string(v & 0xFFFF);
                } else {
                    // 按序号导入之外：ILT 项指向 `[hint u16][name]`，名字从 +2 开始。
                    const auto hn = rva_to_off(static_cast<uint32_t>(v & 0x7FFFFFFF));
                    name = hn ? cstr(*hn + 2) : "?";
                }
                slot_name[iat_rva + static_cast<uint64_t>(k) * 8] = dll + "!" + name;
            }
        }
    }

    // 照抄 `import_thunks`：可执行节里的 `jmp [rip+rel32]`，目标落在 IAT 槽上才算。
    // ⚠ MinGW 发的是 `48 FF 25 rel32`，产物 movabs 引用的是**指令起点**，所以 REX.W 要退回一格。
    void scan_thunks() {
        for (const Section& s : sections) {
            if (!(s.chars & kScnMemExecute) || !s.raw_size) continue;
            const uint64_t begin = s.raw_ptr;
            const uint64_t end = std::min<uint64_t>(begin + s.raw_size, data.size());
            if (end <= begin) continue;
            // 与 Python 的 `range(len(blob) - 6)` 同界：最后一个能放下 `FF 25 rel32` 的 j。
            for (uint64_t j = begin; j + 7 <= end; ++j) {
                if (data[j] != 0xFF || data[j + 1] != 0x25) continue;
                const uint64_t start = (j > begin && data[j - 1] == 0x48) ? j - 1 : j;
                const int32_t rel = static_cast<int32_t>(le_u32(&data[j + 2]));
                const uint64_t thunk_va = s.va + (start - begin);
                const uint32_t slot_rva =
                    static_cast<uint32_t>(s.va + (j - begin) + 6 +
                                          static_cast<uint64_t>(static_cast<int64_t>(rel)));
                const auto it = slot_name.find(slot_rva);
                if (it == slot_name.end()) continue;
                thunk_name[thunk_va] = it->second;
                thunk_slot[thunk_va] = slot_rva;
            }
        }
    }
};

// 照抄 `recover_baked_base`：DLL 址 = 捕获期的 `baked + thunk_rva`，所以 `v - thunk_rva` 落在
// 64 KB 整数倍上就是一张票。投票按首次出现顺序破平（Python 的 `Counter.most_common` 也是这样）。
uint64_t recover_baked_base(const std::vector<std::pair<uint64_t, uint64_t>>& sites,
                            const std::unordered_map<uint64_t, std::string>& thunks,
                            uint64_t game_lo, uint64_t game_hi) {
    std::vector<std::pair<uint64_t, size_t>> votes;
    for (const auto& site : sites) {
        const uint64_t v = site.second;
        if (game_lo - kBand <= v && v <= game_hi + kBand) continue;
        for (const auto& thunk : thunks) {
            const uint64_t thunk_rva = thunk.first;
            const int64_t diff = static_cast<int64_t>(v - thunk_rva);
            if (diff % kPage != 0) continue;
            auto it = std::find_if(votes.begin(), votes.end(),
                                   [&](const auto& e) { return e.first == v - thunk_rva; });
            if (it == votes.end())
                votes.emplace_back(v - thunk_rva, 1);
            else
                ++it->second;
        }
    }
    uint64_t best = 0;
    size_t best_n = 0;
    for (const auto& [value, n] : votes) {
        if (n > best_n) {
            best_n = n;
            best = value;
        }
    }
    return best;
}

}  // namespace

std::vector<std::pair<uint64_t, uint64_t>> movabs_sites(const std::vector<uint8_t>& code) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    // `48..4F` 是 REX（W + 可选 B/X/R），`B8..BF` 是 `mov r64, imm64`；立即数紧跟其后 8 字节。
    for (uint64_t i = 2; i + 9 <= code.size(); ++i) {
        if (code[i - 2] < 0x48 || code[i - 2] > 0x4F) continue;
        if (code[i - 1] < 0xB8 || code[i - 1] > 0xBF) continue;
        out.emplace_back(i, le_u64(&code[i]));
    }
    return out;
}

Image Image::load(const std::string& bin, const Spec& spec, uint64_t entry_off,
                  const std::string& dll_path) {
    const std::vector<uint8_t> blob = read_file(bin);
    if (blob.size() < 8) throw std::runtime_error(bin + " 连 8 字节头都不够");
    // 头有两种写法：参考产物是纯 `out_len`，`simplexc/assemble` 的产物是 `out_len | bit62`
    // （bit62 = 「游戏不 free 这块」）。反过来，头是 u64，bit62 之外唯一可能的高位就是 bit63，
    // 它一旦为 1 就是假头（不许靠「截断后恰好等于文件长度」把它认下来）。
    constexpr uint64_t kNoFree = 1ull << 62;
    const uint64_t header = le_u64(blob.data());
    if (header & (1ull << 63))
        throw std::runtime_error(bin + " 的头 " + hex(header) + " 带了 bit63（只允许 out_len | bit62）");
    const uint64_t total = header & ~kNoFree;
    if (total != blob.size())
        throw std::runtime_error(bin + " 头里的总长 " + std::to_string(total) + " 与文件长度 " +
                                 std::to_string(blob.size()) + " 不符（头与产物不是一对）");

    Image img;
    img.code.assign(blob.begin() + 8, blob.end());
    img.code_base = kCodeBase;
    img.entry = entry_off;
    if (entry_off >= img.code.size())
        throw std::runtime_error(bin + " 的 entry " + hex(entry_off) + " 落在 code 外");

    // sidecar 先读：它与 DLL 无关（我们的产物根本没走 thunk、`baked` 会是 0，下面会早退），
    // 而它的语法错误要不要抛不该取决于那台机器上有没有 `compile_real.dll`。
    img.stub_refs = read_stub_sidecar(bin + ".stubs", img.code.size());

    // 读不了 DLL 就抛：静默退化成空表会让「认不出任何导入」看起来像「这份产物没有导入」。
    const PeImage pe = PeImage::load(dll_path);
    img.dll_size = pe.image_size;

    const auto sites = movabs_sites(img.code);
    img.baked = recover_baked_base(sites, pe.thunk_name, spec.game_lo(), spec.game_hi());
    if (!img.baked) return img;

    // 整块 DLL：摆在 baked 上的字节 + 整张导入表（driver 拿它 map 与装桩）。
    img.dll.base = img.baked;
    img.dll.image = pe.memory_image();
    img.dll.iat = pe.iat_table(img.baked);

    std::vector<Thunk> thunks;
    for (const auto& [off, v] : sites) {
        if (v < img.baked || v >= img.baked + img.dll_size) continue;
        const uint64_t rva = v - img.baked;
        const auto it = pe.thunk_name.find(rva);
        const bool named = it != pe.thunk_name.end();
        img.dll_refs.push_back({off, v, named ? proc_name(it->second) : std::string()});
        if (!named) continue;
        const auto slot_va = img.baked + pe.thunk_slot.at(rva);
        const auto dup = std::find_if(thunks.begin(), thunks.end(),
                                      [&](const Thunk& t) { return t.slot_va == slot_va; });
        if (dup == thunks.end())
            thunks.push_back({slot_va, proc_name(it->second)});
        else if (dup->import != proc_name(it->second))  // 同一槽两个名字 = DLL 表与产物不是一对
            throw std::runtime_error(dll_path + " 的 IAT 槽 " + hex(slot_va) +
                                     " 有两个名字：" + dup->import + " / " + proc_name(it->second));
    }
    std::sort(thunks.begin(), thunks.end(),
              [](const Thunk& a, const Thunk& b) { return a.slot_va < b.slot_va; });
    img.thunks = std::move(thunks);
    return img;
}
