#include "diff.hh"

#include <algorithm>
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

void put_u32(std::string& out, uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

void put_u64(std::string& out, uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

uint32_t read_u32(const std::string& in, size_t& off, const std::string& path) {
    uint32_t v = 0;
    if (in.size() < off + 4) throw std::runtime_error("difftrace: 序列文件被截断: " + path);
    for (int shift = 0; shift < 32; shift += 8) {
        v |= static_cast<uint32_t>(static_cast<unsigned char>(in[off + static_cast<size_t>(shift) / 8]))
             << shift;
    }
    off += 4;
    return v;
}

uint64_t read_u64(const std::string& in, size_t& off, const std::string& path) {
    uint64_t v = 0;
    if (in.size() < off + 8) throw std::runtime_error("difftrace: 序列文件被截断: " + path);
    for (int shift = 0; shift < 64; shift += 8) {
        v |= static_cast<uint64_t>(static_cast<unsigned char>(in[off + static_cast<size_t>(shift) / 8]))
             << shift;
    }
    off += 8;
    return v;
}

bool valid_size(uint32_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8 || size == 16;
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
    std::string bytes;
    bytes.reserve(kHeaderBytes + stores.size() * kRecordBytes);
    bytes.append(kMagic, sizeof(kMagic));
    put_u32(bytes, kVersion);
    put_u64(bytes, stores.size());
    for (const Store& s : stores) {
        put_u64(bytes, s.pc);
        put_u64(bytes, s.addr);
        put_u32(bytes, s.size);
        put_u64(bytes, s.value);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("difftrace: 打不开序列文件写: " + path);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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
        // `value` 只有 8 字节：`size == 16` 只能展开前 8 个（语料里没有 16 字节写）。
        const uint32_t bytes = std::min<uint32_t>(s.size, 8);
        for (uint32_t b = 0; b < bytes; ++b)
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
