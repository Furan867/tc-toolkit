#include "diff.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace tc {
namespace {

// 拆成两段是为了让它成为对象里的连续成员，落盘时不必拼中间字符串。
constexpr char kMagic[8] = {'d', 'i', 'f', 'f', 't', 'r', '0', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 8 + 4 + 8;
constexpr size_t kRecordBytes = 8 + 8 + 4 + 8;
// 落盘缓冲一块多少条：65536 × 28 B ≈ 1.8 MB，小到不拖内存，大到不会把 write 叫成万次。
constexpr size_t kSaveChunkRecords = 1u << 16;

void put_u32(std::string& out, uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

void put_u64(std::string& out, uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

// 小端解码走 memcpy：一次一个字节地拼会在 77 M 条的序列上把时间全花在移位上
// （10000 tick 的 088 是 2.17 GB），而 x86-64 本身就是小端，memcpy 出来的就是那个值。
uint64_t load_le64(const char* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t read_u32(const std::string& in, size_t& off, const std::string& path) {
    if (in.size() < off + 4) throw std::runtime_error("difftrace: 序列文件被截断: " + path);
    const uint32_t v = static_cast<uint32_t>(load_le64(in.data() + off));
    off += 4;
    return v;
}

uint64_t read_u64(const std::string& in, size_t& off, const std::string& path) {
    if (in.size() < off + 8) throw std::runtime_error("difftrace: 序列文件被截断: " + path);
    const uint64_t v = load_le64(in.data() + off);
    off += 8;
    return v;
}

bool valid_size(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8 || size == 16;
}

std::string hex_addr(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// 计数表的唯一排序规则：计数降序，同计数按 key 升序 —— 同输入必须逐字节同输出，
// 否则两份「一样」的 trace 统计起来还会互相diff。
std::vector<std::pair<uint64_t, size_t>> ranked(const std::map<uint64_t, size_t>& counts,
                                                size_t top_n) {
    std::vector<std::pair<uint64_t, size_t>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<uint64_t, size_t>& x, const std::pair<uint64_t, size_t>& y) {
                  if (x.second != y.second) return x.second > y.second;
                  return x.first < y.first;
              });
    if (top_n != 0 && top_n < rows.size()) rows.resize(top_n);
    return rows;
}

}  // namespace

std::optional<Divergence> first_divergence(const std::vector<Store>& a, const std::vector<Store>& b) {
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        if (a[i].pc != b[i].pc || a[i].addr != b[i].addr || a[i].size != b[i].size ||
            a[i].value != b[i].value) {
            return Divergence{i, false, a[i], b[i]};
        }
    }
    if (a.size() != b.size()) return Divergence{common, true, Store{}, Store{}};
    return std::nullopt;
}

void save(const std::string& path, const std::vector<Store>& stores) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("difftrace: 打不开序列文件写: " + path);
    // 头部单独写，记录按块写。梯度上限不是为了好看：整份先攒进一个 `std::string` 再一次性
    // `write` 在超过 2 GiB 时不返回 —— 实测 10000 tick 的 088（2.17 GB）文件已经完整落盘，
    // 进程却在那个 write 里烧了 15 分钟 CPU 再也要不回控制权，batch 只能记成超时。
    std::string buf;
    buf.reserve(kHeaderBytes + kSaveChunkRecords * kRecordBytes);
    buf.append(kMagic, sizeof(kMagic));
    put_u32(buf, kVersion);
    put_u64(buf, stores.size());
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    for (size_t begin = 0; begin < stores.size(); begin += kSaveChunkRecords) {
        const size_t stop = std::min(begin + kSaveChunkRecords, stores.size());
        buf.clear();
        for (size_t i = begin; i < stop; ++i) {
            const Store& s = stores[i];
            put_u64(buf, s.pc);
            put_u64(buf, s.addr);
            put_u32(buf, s.size);
            put_u64(buf, s.value);
        }
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (!out) throw std::runtime_error("difftrace: 写序列文件失败: " + path);
    }
    out.close();
    if (!out) throw std::runtime_error("difftrace: 写序列文件失败: " + path);
}

std::vector<Store> load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("difftrace: 打不开序列文件读: " + path);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < kHeaderBytes) throw std::runtime_error("difftrace: 序列文件过短: " + path);
    if (bytes.compare(0, sizeof(kMagic), kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("difftrace: 不是序列文件（magic 不符）: " + path);
    }
    size_t off = sizeof(kMagic);
    const uint32_t version = read_u32(bytes, off, path);
    if (version != kVersion) {
        throw std::runtime_error("difftrace: 序列文件版本不是 " + std::to_string(kVersion) + ": " + path);
    }
    const uint64_t count = read_u64(bytes, off, path);
    if (count > (bytes.size() - off) / kRecordBytes) {
        throw std::runtime_error("difftrace: 序列文件被截断: " + path);
    }
    if (bytes.size() - off != count * kRecordBytes) {
        throw std::runtime_error("difftrace: 序列文件尾部有多余字节: " + path);
    }
    std::vector<Store> stores;
    stores.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        Store s;
        s.pc = read_u64(bytes, off, path);
        s.addr = read_u64(bytes, off, path);
        s.size = read_u32(bytes, off, path);
        s.value = read_u64(bytes, off, path);
        if (!valid_size(s.size)) {
            throw std::runtime_error("difftrace: 第 " + std::to_string(i) + " 条的 size 非法: " +
                                     std::to_string(s.size) + " (" + path + ")");
        }
        stores.push_back(s);
    }
    return stores;
}

std::vector<Store> filter_addr(const std::vector<Store>& stores, uint64_t addr) {
    std::vector<Store> hits;
    for (const Store& s : stores) {
        if (s.addr == addr) hits.push_back(s);
    }
    return hits;
}

std::vector<std::pair<uint64_t, size_t>> histogram_pc(const std::vector<Store>& stores, size_t top_n) {
    std::map<uint64_t, size_t> counts;
    for (const Store& s : stores) ++counts[s.pc];
    return ranked(counts, top_n);
}

std::vector<std::pair<uint64_t, size_t>> histogram_addr(const std::vector<Store>& stores,
                                                       size_t top_n) {
    std::map<uint64_t, size_t> counts;
    for (const Store& s : stores) ++counts[s.addr];
    return ranked(counts, top_n);
}

Memory fold(const std::vector<Store>& seq, size_t begin, size_t end) {
    Memory mem;
    const size_t stop = std::min(end, seq.size());
    for (size_t i = std::min(begin, seq.size()); i < stop; ++i) {
        const Store& s = seq[i];
        // 只有 1/2/4/8 能按字节展开：`Store::value` 是 uint64_t，`size > 8` 时高半截没有真值来源
        // （宽写由 Unicorn 写钩子记成 0，见 driver.cc 的 Trace::write_hook）。按 8 截断等于让判据
        // 看不见高半截的差异、却照旧报 equal；展开 16 字节则是凭空造数据。两条都比报错难查。
        // `size == 0` 也不接受：落到这里只说明 Store 造错了。
        if (s.size != 1 && s.size != 2 && s.size != 4 && s.size != 8) {
            throw std::runtime_error("difftrace: fold 只能展开 1/2/4/8 字节的写，遇到 size=" +
                                     std::to_string(s.size) + " addr=" + hex_addr(s.addr));
        }
        for (uint32_t b = 0; b < s.size; ++b)
            mem[s.addr + b] = static_cast<uint8_t>((s.value >> (8 * b)) & 0xffu);
    }
    return mem;
}

size_t memory_diff(const Memory& a, const Memory& b, size_t max, std::vector<ByteDiff>& out) {
    // 两份 map 键升序合并；缺的键按 0 算 —— 「一边从没写过」与「另一边写了 0」是同一件事。
    size_t count = 0;
    auto ia = a.begin();
    auto ib = b.begin();
    while (ia != a.end() || ib != b.end()) {
        std::optional<uint8_t> left;
        std::optional<uint8_t> right;
        uint64_t addr = 0;
        if (ia != a.end() && ib != b.end() && ia->first == ib->first) {
            addr = ia->first;
            left = ia->second;
            right = ib->second;
            ++ia;
            ++ib;
        } else if (ib == b.end() || (ia != a.end() && ia->first < ib->first)) {
            addr = ia->first;
            left = ia->second;
            ++ia;
        } else {
            addr = ib->first;
            right = ib->second;
            ++ib;
        }
        if (left.value_or(0) == right.value_or(0)) continue;
        if (max == 0 || out.size() < max) out.push_back(ByteDiff{addr, {left, right}});
        ++count;
    }
    return count;
}

SegmentTally tally_by_segment(const Memory& a, const Memory& b, const SegBounds& bounds,
                              size_t top_prefixes) {
    SegmentTally tally;
    std::map<uint64_t, size_t> prefixes;
    // 键升序合并、缺的键按 0 算 —— 与 `memory_diff` 同口径（这里要的是分账，不是差异清单）。
    auto ia = a.begin();
    auto ib = b.begin();
    while (ia != a.end() || ib != b.end()) {
        uint64_t addr = 0;
        std::optional<uint8_t> left;
        std::optional<uint8_t> right;
        if (ia != a.end() && ib != b.end() && ia->first == ib->first) {
            addr = ia->first;
            left = ia->second;
            right = ib->second;
            ++ia;
            ++ib;
        } else if (ib == b.end() || (ia != a.end() && ia->first < ib->first)) {
            addr = ia->first;
            left = ia->second;
            ++ia;
        } else {
            addr = ib->first;
            right = ib->second;
            ++ib;
        }
        if (left.value_or(0) == right.value_or(0)) continue;
        if (!tally.first_addr) tally.first_addr = addr;
        if (addr >= bounds.stack_lo && addr < bounds.stack_hi) {
            ++tally.stack;
        } else if (addr >= bounds.code_lo && addr < bounds.code_hi) {
            ++tally.code;
        } else {
            ++tally.game;
            ++prefixes[addr >> 32];
        }
    }
    tally.game_prefixes = ranked(prefixes, top_prefixes);
    return tally;
}

std::vector<TickSpan> split_ticks(const std::vector<Store>& seq, uint64_t tick_addr) {
    std::vector<TickSpan> spans;
    size_t begin = 0;
    for (size_t i = 0; i < seq.size(); ++i) {
        if (seq[i].addr != tick_addr) continue;
        spans.push_back(TickSpan{begin, i + 1});
        begin = i + 1;
    }
    // 尾巴不满一个 tick 也要自成一段，否则 run 结束时的状态整个被静默扔掉。
    if (begin < seq.size()) spans.push_back(TickSpan{begin, seq.size()});
    return spans;
}

std::optional<StateDiff> first_state_diff(const std::vector<Store>& a, const std::vector<Store>& b,
                                          uint64_t tick_addr, size_t max_mismatches) {
    const std::vector<TickSpan> ta = split_ticks(a, tick_addr);
    const std::vector<TickSpan> tb = split_ticks(b, tick_addr);
    if (ta.size() != tb.size()) {
        StateDiff diff;
        diff.tick = std::min(ta.size(), tb.size());
        diff.tick_count = std::make_pair(ta.size(), tb.size());
        return diff;
    }
    for (size_t t = 0; t < ta.size(); ++t) {
        const Memory ma = fold(a, ta[t].begin, ta[t].end);
        const Memory mb = fold(b, tb[t].begin, tb[t].end);
        StateDiff diff;
        diff.tick = t;
        diff.mismatch_count = memory_diff(ma, mb, max_mismatches, diff.mismatches);
        if (diff.mismatch_count != 0) return diff;
    }
    return std::nullopt;
}

}  // namespace tc
