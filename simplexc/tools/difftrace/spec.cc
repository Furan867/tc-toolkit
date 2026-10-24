#include "spec.hh"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace {

// 四个正则与 simspec.py 的同名常量一一对应；`multiline` 是 Python 的 `re.M`。
// 各正则都没加行尾 $：Python 版靠「行尾注释 `[^\n]*`」把尾巴吃掉，等价于不约束。
const std::regex kEnumRe(R"(^type\s+(\w+)\s+Enum\[([^\]]*)\])",
                         std::regex::ECMAScript | std::regex::multiline);
const std::regex kPtrRe(R"(^\s*(?:var\s+(\w+)|const\s+#(\w+))\s*=\s*Ptr\s+(\d{10,})([^
]*))",
                        std::regex::ECMAScript | std::regex::multiline);
// 块首注释里的两个数：`c_alloc(N)` 是块字节数，`c_alloc(N) + back` 的 back 是块首比记录地址低
// 多少（生成器给 `#DATA_*` / `#RESET_DATA_*` 都写 `+ 2048`）。见 simspec.CALLOC_RE / CALLOC_BACK_RE。
const std::regex kCAllocRe(R"(c_alloc\((\d+)\))");
const std::regex kCAllocBackRe(R"(c_alloc\(\d+\)\s*\+\s*(\d+))");

// `#RESET_DATA_*` 前缀：生成器按它区分「内存镜像初值」这一组（simspec.RESET_DATA_PREFIX）。
constexpr const char* kResetDataPrefix = "RESET_DATA_";
const std::regex kArrRe(
    R"(^\s*(?:var\s+(\w+)|const\s+#(\w+))\s*=\s*raw_cast\(\s*<Arr\[(\w+)\]>\s*,\s*(\d{10,})\s*\))",
    std::regex::ECMAScript | std::regex::multiline);
const std::regex kMemClearRe(R"(memory_clear\(\s*#(\w+)\s*(?:-\s*(\d+))?\s*,\s*(\d+)\s*\))");
const std::regex kMemCopyRe(
    R"(memory_copy\(\s*#(\w+)\s*(?:-\s*(\d+))?\s*,\s*#(\w+)\s*(?:-\s*(\d+))?\s*,\s*(\d+)\s*\))");

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("读不了 " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

uint64_t parse_u64(const std::string& s) { return std::stoull(s); }

uint64_t optional_u64(const std::ssub_match& m) { return m.matched ? parse_u64(m.str()) : 0; }

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

}  // namespace

Spec Spec::parse(const std::string& path) {
    const std::string text = read_file(path);
    Spec spec;
    spec.path_ = path;

    for (auto it = std::sregex_iterator(text.begin(), text.end(), kEnumRe);
         it != std::sregex_iterator(); ++it) {
        const std::string members = (*it)[2].str();
        std::vector<std::string> names;
        std::string item;
        std::istringstream fields(members);
        while (std::getline(fields, item, ',')) {
            std::string name = trim(item);
            if (!name.empty()) names.push_back(std::move(name));
        }
        spec.enums_[(*it)[1].str()] = std::move(names);
    }

    for (auto it = std::sregex_iterator(text.begin(), text.end(), kPtrRe);
         it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        const uint64_t addr = parse_u64((*it)[3].str());
        spec.scalars_.emplace_back(name, addr);

        const std::string tail = (*it)[4].str();
        std::smatch m;
        if (!std::regex_search(tail, m, kCAllocRe)) continue;
        const uint64_t size = parse_u64(m[1].str());
        // 两个正则分开用两个 smatch：后一次搜不到时 `m` 会被清空。
        std::smatch back_m;
        const uint64_t back = std::regex_search(tail, back_m, kCAllocBackRe) ? parse_u64(back_m[1].str()) : 0;
        spec.blocks_.push_back({name, addr, back, size});
    }

    for (auto it = std::sregex_iterator(text.begin(), text.end(), kArrRe);
         it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        spec.arrays_.push_back({name, parse_u64((*it)[4].str()), (*it)[3].str()});
    }

    std::map<std::string, uint64_t> by_name(spec.scalars_.begin(), spec.scalars_.end());

    for (auto it = std::sregex_iterator(text.begin(), text.end(), kMemClearRe);
         it != std::sregex_iterator(); ++it) {
        const auto a = by_name.find((*it)[1].str());
        if (a == by_name.end()) continue;
        const uint64_t lo = a->second - optional_u64((*it)[2]);
        spec.ranges_.emplace_back(lo, lo + parse_u64((*it)[3].str()));
    }

    for (auto it = std::sregex_iterator(text.begin(), text.end(), kMemCopyRe);
         it != std::sregex_iterator(); ++it) {
        const uint64_t size = parse_u64((*it)[5].str());
        const std::pair<std::string, uint64_t> ends[2] = {
            {(*it)[1].str(), optional_u64((*it)[2])},
            {(*it)[3].str(), optional_u64((*it)[4])},
        };
        for (size_t i = 0; i < 2; ++i) {
            const auto& [name, back] = ends[i];
            const auto a = by_name.find(name);
            if (a == by_name.end()) continue;
            const uint64_t lo = a->second - back;
            spec.ranges_.emplace_back(lo, lo + size);
            // 只认第一个实参（copy 的源）且名字是 `#RESET_DATA_*` 的 —— 与 simspec 的 `idx == 0` 同。
            if (i == 0 && name.rfind(kResetDataPrefix, 0) == 0)
                spec.reset_copies_.push_back({name, lo, size});
        }
    }

    return spec;
}

std::vector<std::pair<uint64_t, uint64_t>> Spec::regions() const {
    // 先放 memop 区间，再退化的单点，顺序与 simspec.regions() 一致（合并前不许去重）。
    std::vector<std::pair<uint64_t, uint64_t>> out = ranges_;
    out.reserve(ranges_.size() + scalars_.size() + arrays_.size());
    for (const auto& [name, addr] : scalars_) out.emplace_back(addr, addr);
    for (const auto& arr : arrays_) {
        // 数组的 seq 头在 p-8（`.len()` = `load i64, p-8`）。地址落在页边界上时 p-8 掉进
        // 上一页，不扩这 8 字节宿主就没映射它，参考产物读 len 时 AV。
        out.emplace_back(arr.addr - 8, arr.addr);
    }
    return out;
}

std::vector<Spec::ResetBlock> Spec::reset_data_blocks() const {
    std::vector<ResetBlock> out;
    std::vector<std::pair<std::string, uint64_t>> covered;
    for (const ResetBlock& b : reset_copies_) {
        const std::pair<std::string, uint64_t> key{b.name, b.lo};
        if (std::find(covered.begin(), covered.end(), key) != covered.end()) continue;
        covered.push_back(key);
        out.push_back(b);
    }
    for (const BlockDecl& d : blocks_) {
        if (d.name.rfind(kResetDataPrefix, 0) != 0) continue;
        const bool referenced = std::any_of(out.begin(), out.end(), [&](const ResetBlock& b) {
            return b.name == d.name;
        });
        if (referenced) continue;
        out.push_back({d.name, d.addr - d.back, d.size});
    }
    return out;
}

std::optional<uint64_t> Spec::addr(const std::string& name) const {
    for (const auto& [n, a] : scalars_) {
        if (n == name) return a;
    }
    for (const auto& arr : arrays_) {
        if (arr.name == name) return arr.addr;
    }
    return std::nullopt;
}

std::optional<std::vector<uint64_t>> Spec::enum_values(const std::string& type) const {
    const auto it = enums_.find(type);
    if (it == enums_.end()) return std::nullopt;
    std::vector<uint64_t> values(it->second.size());
    for (size_t i = 0; i < values.size(); ++i) values[i] = i;
    return values;
}

std::map<std::string, std::vector<std::string>> Spec::names() const { return enums_; }

const std::vector<std::pair<std::string, uint64_t>>& Spec::scalars() const { return scalars_; }

const std::vector<Spec::ArrayDecl>& Spec::arrays() const { return arrays_; }

uint64_t Spec::game_lo() const {
    std::optional<uint64_t> lo;
    for (const auto& [n, a] : scalars_) lo = lo ? std::min(*lo, a) : a;
    for (const auto& arr : arrays_) lo = lo ? std::min(*lo, arr.addr) : arr.addr;
    if (!lo) throw std::runtime_error(path_ + ": 没有任何地址声明");
    return *lo;
}

uint64_t Spec::game_hi() const {
    std::optional<uint64_t> hi;
    for (const auto& [n, a] : scalars_) hi = hi ? std::max(*hi, a) : a;
    for (const auto& arr : arrays_) hi = hi ? std::max(*hi, arr.addr) : arr.addr;
    if (!hi) throw std::runtime_error(path_ + ": 没有任何地址声明");
    return *hi;
}
