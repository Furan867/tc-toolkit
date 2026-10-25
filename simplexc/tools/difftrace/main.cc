/* difftrace —— Unicorn 上的访存序列差分（设计 docs/specs/2026-10-25-difftrace-design.md）。
 *
 * 三个子命令，一个子命令一件事：
 *   run   跑一份产物、采一份 store 序列（唯一会碰 Unicorn 的子命令）；
 *   diff  两份落盘序列比状态（`--state`，主判据）或比轨迹（`--trace`，同编译器两版才成立）；
 *   where 查一份序列里写过某地址的那些写（连带反汇编）。
 * 判据是内存状态不是轨迹，所以 run 只管采集、比对归 diff —— 一个进程里同时干两件事时，
 * 「哪边跑出了偏差」与「哪边状态不同」会混在同一段输出里。
 */
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include <unicorn/unicorn.h>

#include "diff.hh"
#include "disasm.hh"
#include "driver.hh"
#include "image.hh"
#include "spec.hh"

namespace {

using u64 = uint64_t;

constexpr const char* kState2663Offset = "2663";  // 相对 `#SIMULATION_STATE` 的诊断偏移

// `--where` 的窗口宽度。判据是「覆盖」，不是「相等」：一次写可能比地址宽（8 字节写盖住窗口里的
// 一个字），也可能比地址窄（1 字节写落在窗口里的第二个字节上）；只比 `s.addr == addr` 会把
// 同一个块里除首条之外的所有写全漏掉，而那正是要看的东西。
constexpr u64 kWhereWindow = 8;

constexpr size_t kMaxMismatchPrint = 20;

// 退出码。三档互斥，一个概念一个码：
//   `kExitOk`   判据成立 / 成功；
//   `kExitDiff` 判据不成立（`diff` 不等 / `where` 未命中）；
//   `kExitError` 出错（参数错、文件打不开、序列解析失败、`--tick-mark` 无效、模拟错误）。
// 出错一律 `kExitError`，`run` 不返回 `kExitDiff` —— `1` 只表示「跑完了，结论是不等」。
constexpr int kExitOk = 0;
constexpr int kExitDiff = 1;
constexpr int kExitError = 2;

// ---- 输出 -------------------------------------------------------------------
//
// `--json` 时 stdout 只留一个 JSON 对象，人读的行全部转去 stderr：两种格式混在同一条流里，
// `difftrace … --json | python -c "json.load(sys.stdin)"` 就不可能成立。
struct Io {
    bool json = false;
    std::FILE* text = stdout;
};

void say(const Io& io, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    const size_t len = std::min(static_cast<size_t>(n), sizeof(buf) - 1);
    std::fwrite(buf, 1, len, io.text);
}

std::string hex(u64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// 路径与反汇编文本都可能带 `"`、`\`、换行，直接塞进 JSON 就不再是 JSON。
std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// 字段顺序固定（按插入顺序），值由调用方给成已经编码好的 JSON 文本。
class Json {
  public:
    void put(const std::string& key, const std::string& value) {
        if (!body_.empty()) body_ += ',';
        body_ += json_quote(key) + ':' + value;
    }
    void num(const std::string& key, u64 v) { put(key, std::to_string(v)); }
    void size(const std::string& key, size_t v) { put(key, std::to_string(v)); }
    void opt_num(const std::string& key, const std::optional<u64>& v) {
        put(key, v ? std::to_string(*v) : std::string("null"));
    }
    void opt_size(const std::string& key, const std::optional<size_t>& v) {
        put(key, v ? std::to_string(*v) : std::string("null"));
    }
    void text(const std::string& key, const std::string& v) { put(key, json_quote(v)); }
    void flag(const std::string& key, bool v) { put(key, v ? "true" : "false"); }
    std::string str() const { return "{" + body_ + "}"; }

  private:
    std::string body_;
};

std::string store_json(const tc::Store& s) {
    Json o;
    o.num("pc", s.pc);
    o.num("addr", s.addr);
    o.num("size", s.size);
    o.num("value", s.value);
    o.text("pc_hex", hex(s.pc));
    o.text("addr_hex", hex(s.addr));
    o.text("value_hex", hex(s.value));
    return o.str();
}

// ---- SHA-256（FIPS 180-4）---------------------------------------------------
//
// 只为一件事：拿 blob 的 sha256 去 manifest 的 `out_sha256` 列反查 entry。为它引一条 OpenSSL
// 依赖（或 shell out 到 sha256sum）不值得 —— 判据设施不该把平台差异吃进来。

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

uint32_t rotr(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }

void sha256_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) |
               (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t i = 0;
    for (; i + 64 <= data.size(); i += 64) sha256_block(h, data.data() + i);

    const size_t rem = data.size() - i;
    const size_t pad = rem + 1 + 8 > 64 ? 128 : 64;
    uint8_t tail[128] = {0};
    if (rem) std::memcpy(tail, data.data() + i, rem);
    tail[rem] = 0x80;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int k = 0; k < 8; ++k) tail[pad - 1 - k] = static_cast<uint8_t>(bits >> (8 * k));
    sha256_block(h, tail);
    if (pad == 128) sha256_block(h, tail + 64);

    char out[65];
    for (int k = 0; k < 8; ++k) std::snprintf(out + 8 * k, 9, "%08x", h[k]);
    return std::string(out, 64);
}

std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("读不了 " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

bool file_exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<bool>(f);
}

std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("读不了 " + path);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// ---- 参数 -------------------------------------------------------------------

u64 parse_u64(const std::string& text, const char* what) {
    std::size_t used = 0;
    u64 v = 0;
    try {
        v = std::stoull(text, &used, 0);
    } catch (const std::exception&) {
        used = text.size() + 1;  // 走下面同一句报错，不区分「格式错」与「越界」
    }
    if (used != text.size()) throw std::runtime_error(std::string(what) + " 不是整数: " + text);
    return v;
}

// `<blob>.entry` 的内容是不带前缀的十六进制（`simplexc/cli/tcblob.cc` 写 `%llX`），
// 按 base 0 解会把 `64A0` 读成 64 而不报错 —— 这种错只能靠调用方按 base 16 解来避免。
u64 parse_hex_text(const std::string& text, const char* what) {
    std::string s = text;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (s.empty()) throw std::runtime_error(std::string(what) + " 是空文件");
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    std::size_t used = 0;
    u64 v = 0;
    try {
        v = std::stoull(s, &used, 16);
    } catch (const std::exception&) {
        used = s.size() + 1;
    }
    if (used != s.size()) throw std::runtime_error(std::string(what) + " 不是十六进制: " + text);
    return v;
}

tc::Seed parse_seed(const std::string& s) {
    if (s == "none") return tc::Seed::kNone;
    if (s == "seq") return tc::Seed::kSeq;
    if (s == "xorshift") return tc::Seed::kXorshift;
    throw std::runtime_error("--seed 只有 none / seq / xorshift，给了: " + s);
}

const char* seed_name(tc::Seed s) {
    switch (s) {
        case tc::Seed::kNone:
            return "none";
        case tc::Seed::kSeq:
            return "seq";
        case tc::Seed::kXorshift:
            return "xorshift";
    }
    return "?";
}

bool is_help(const std::string& a) { return a == "--help" || a == "-h"; }

// ---- entry 自动解析 ---------------------------------------------------------
//
// 三处按序（都不中才报错，报错里把试过的每一处列出来）：
//   1. `<blob>.entry` —— 我们产物旁边那一份（`tcblob` 写的裸十六进制）；
//   2. blob 上级目录里的 `manifest.tsv` / `manifest-capture.tsv` —— `out_sha256` 与 blob 相同的
//      那一行，取 `entry`（corpus 表）或 `r2`（游戏内捕获表，列名如 `simplexc/tools/check.py`
//      的 RV32IM_REF_ENTRY 所述）列。
// 参考产物 `simplexc/corpus/rv32im/rv32im.bin` 的 entry 只在同目录的 `manifest-capture.tsv`
// 里（`corpus/manifest.tsv` 是另一批语料，没有这一行），所以第 2 处必须沿上级目录找。

std::string dir_name(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return path.substr(0, 1);  // `\foo` / `/foo`
    const std::string head = path.substr(0, slash);
    // `D:` 这种没有尾斜杠的盘符根：再往上没有目录了。
    if (head.size() == 2 && head[1] == ':') return head + "/";
    return head;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

std::vector<std::string> split_tab(const std::string& line) {
    std::vector<std::string> out;
    size_t begin = 0;
    while (true) {
        const size_t tab = line.find('\t', begin);
        if (tab == std::string::npos) {
            out.push_back(line.substr(begin));
            return out;
        }
        out.push_back(line.substr(begin, tab - begin));
        begin = tab + 1;
    }
}

std::optional<size_t> column_index(const std::vector<std::string>& head, const char* name) {
    for (size_t i = 0; i < head.size(); ++i)
        if (head[i] == name) return i;
    return std::nullopt;
}

struct EntryHit {
    u64 entry = 0;
    std::string column;
    std::string row_id;
    std::string manifest;
};

// 一张 manifest 里按 `out_sha256` 反查。同一张表里多行同 sha 是常态（捕获表 seq 002…007 是
// 同一份源码被连续编译），所以「多个命中值不同」才算歧义，由调用方判。
// `note` 非空 = 这张表存在但没给出 entry（原因），给报错用；文件不存在时保持空。
std::vector<EntryHit> lookup_manifest(const std::string& path, const std::string& sha,
                                     std::string& note) {
    std::vector<EntryHit> hits;
    note.clear();
    std::ifstream f(path);
    if (!f) return hits;
    std::string line;
    if (!std::getline(f, line)) {
        note = path + "（空文件）";
        return hits;
    }
    const std::vector<std::string> head = split_tab(line);
    const std::optional<size_t> sha_col = column_index(head, "out_sha256");
    if (!sha_col) {
        note = path + "（没有 out_sha256 列）";
        return hits;
    }
    std::optional<size_t> entry_col = column_index(head, "entry");
    std::string column = "entry";
    if (!entry_col) {
        entry_col = column_index(head, "r2");
        column = "r2";
    }
    if (!entry_col) {
        note = path + "（没有 entry 列也没有 r2 列）";
        return hits;
    }
    std::optional<size_t> id_col = column_index(head, "id");
    if (!id_col) id_col = column_index(head, "seq");
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cells = split_tab(line);
        if (cells.size() <= std::max(*sha_col, *entry_col)) continue;
        if (cells[*sha_col] != sha) continue;
        hits.push_back({parse_u64(cells[*entry_col], column.c_str()), column,
                        id_col && *id_col < cells.size() ? cells[*id_col] : std::string("?"), path});
    }
    if (hits.empty()) note = path + "（没有 out_sha256 命中的行）";
    return hits;
}

struct EntryChoice {
    u64 value = 0;
    std::string source;
};

EntryChoice resolve_entry(const std::string& bin, const std::optional<u64>& explicit_entry) {
    if (explicit_entry) return {*explicit_entry, "--entry"};

    std::string side_note;
    std::vector<std::string> notes;  // 存在但没给出 entry 的表（各带原因）
    const std::string side = bin + ".entry";
    if (file_exists(side)) return {parse_hex_text(read_text(side), side.c_str()), side};

    const std::string sha = sha256_hex(read_all(bin));
    std::vector<EntryHit> hits;
    size_t probed = 0;
    const std::string first_dir = dir_name(bin);
    std::string last_dir = first_dir;
    for (std::string dir = first_dir;; dir = dir_name(dir)) {
        for (const char* name : {"manifest.tsv", "manifest-capture.tsv"}) {
            std::string note;
            const std::vector<EntryHit> got = lookup_manifest(join_path(dir, name), sha, note);
            ++probed;
            if (!note.empty()) notes.push_back(note);
            hits.insert(hits.end(), got.begin(), got.end());
        }
        last_dir = dir;
        const std::string up = dir_name(dir);
        if (dir.empty() || up == dir) break;
    }
    if (hits.empty()) {
        std::string msg = bin + " 的 entry 自动解析失败（blob 的 sha256 " + sha + "）。试过：\n  - " +
                          side + "（文件不存在）";
        if (notes.empty()) {
            msg += "\n  - " + (first_dir.empty() ? std::string(".") : first_dir) + " 逐级向上到 " +
                   (last_dir.empty() ? std::string(".") : last_dir) + " 的 " + std::to_string(probed) +
                   " 个 manifest.tsv / manifest-capture.tsv 位置：都不存在";
        } else {
            for (const std::string& note : notes) msg += "\n  - " + note;
        }
        msg += "\n入口偏移请用 --entry <hex> 显式给。";
        throw std::runtime_error(msg);
    }
    // 歧义不猜：同一个 sha 在不同行给出不同 entry，只能由人指定是哪一个。
    const EntryHit& first = hits.front();
    for (const EntryHit& h : hits)
        if (h.entry != first.entry) {
            std::string msg = bin + " 的 entry 在 manifest 里有多个候选（同一个 out_sha256）:";
            for (const EntryHit& c : hits)
                msg += "\n  - " + hex(c.entry) + "（" + c.manifest + " 的 " + c.column + " 列，id " +
                       c.row_id + "）";
            msg += "\n请用 --entry <hex> 显式指定。";
            throw std::runtime_error(msg);
        }
    return {first.entry, first.manifest + " 的 " + first.column + " 列（id " + first.row_id + "）"};
}

// ---- DLL 定位 ---------------------------------------------------------------
//
// 优先级：`--dll-dir` > 环境变量 `DIFFTRACE_DLL_DIR` > `Image::load` 自己的默认（`kDllReal`）。
//
// 前两者给的是目录：拼上文件名并**当场确认存在**。显式指定了却找不到必须报出来 —— 悄悄回退到别的
// 路径会让人以为在用自己指的那份，而这正是要分清的事。
//
// 两者都没给时返回空串，由 `load_image` 不传第四个参数、原样交给 `Image::load` 的默认：`kDllReal`
// 是开发机上真实存在的那一份，把它改写成「exe 旁」等于把真的那份挡在门外（exe 旁没有 DLL 是常态）。
std::string resolve_dll_path(const std::optional<std::string>& cli_dir) {
    std::string dir;
    const char* how = nullptr;
    if (cli_dir && !cli_dir->empty()) {
        dir = *cli_dir;
        how = "--dll-dir";
    } else if (const char* env = std::getenv("DIFFTRACE_DLL_DIR"); env && *env) {
        dir = env;
        how = "环境变量 DIFFTRACE_DLL_DIR";
    } else {
        return std::string();  // 空 = 不覆盖，用 `Image::load` 的默认
    }
    const std::string path = join_path(dir, "compile_real.dll");
    if (!file_exists(path))
        throw std::runtime_error(std::string(how) + " 指的目录里没有 compile_real.dll: " + path);
    return path;
}

// `dll_arg` 为空 = 没指定：不传第四个参数，`Image::load` 用它自己的默认（`kDllReal`）。
Image load_image(const std::string& bin, const Spec& spec, u64 entry, const std::string& dll_arg) {
    if (dll_arg.empty()) return Image::load(bin, spec, entry);
    return Image::load(bin, spec, entry, dll_arg);
}

// 打印 / JSON 要报「实际读的是哪一份」：空的那条分支读的就是 `kDllReal`。
std::string dll_display_path(const std::string& dll_arg) {
    return dll_arg.empty() ? std::string(kDllReal) : dll_arg;
}

// ---- 人读的打印 -------------------------------------------------------------

void print_store(const Io& io, const char* tag, size_t index, const tc::Store& s) {
    say(io, "  %s[%zu] pc=%s addr=%s size=%u value=%s\n", tag, index, hex(s.pc).c_str(),
        hex(s.addr).c_str(), s.size, hex(s.value).c_str());
}

// 「哪些地址在分歧」比「哪一条先分叉」更早能看出原因：分叉点常常只是一个地址上的值先裂开。
void print_addr_histogram(const Io& io, const char* tag, const std::vector<tc::Store>& stores) {
    say(io, "histogram_addr top 5（%s）：\n", tag);
    for (const auto& [addr, n] : tc::histogram_addr(stores, 5))
        say(io, "  %s  %zu\n", hex(addr).c_str(), n);
}

std::string byte_text(const std::optional<uint8_t>& v) {
    if (!v) return "-(没写)";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", static_cast<unsigned>(*v));
    return buf;
}

// `rows` 已经是截断到 `kMaxMismatchPrint` 条的那一份（`count` 是总数，在 JSON 的 `diff_bytes`
// / `tick_diff_bytes` 里另给）。
std::string byte_diff_json(const std::vector<tc::ByteDiff>& rows) {
    const size_t shown = std::min(rows.size(), kMaxMismatchPrint);
    std::string out = "[";
    for (size_t i = 0; i < shown; ++i) {
        const auto& [addr, vals] = rows[i];
        if (i) out += ',';
        Json o;
        o.num("addr", addr);
        o.text("addr_hex", hex(addr));
        o.text("a", byte_text(vals.first));
        o.text("b", byte_text(vals.second));
        out += o.str();
    }
    out += ']';
    return out;
}

void print_byte_diffs(const Io& io, const std::vector<tc::ByteDiff>& rows, size_t count) {
    const size_t shown = std::min(rows.size(), kMaxMismatchPrint);
    say(io, "  差异字节数     : %zu（列前 %zu 条）\n", count, shown);
    for (size_t i = 0; i < shown; ++i) {
        const auto& [addr, vals] = rows[i];
        say(io, "    addr=%s A=%s B=%s\n", hex(addr).c_str(), byte_text(vals.first).c_str(),
            byte_text(vals.second).c_str());
    }
}

// 只留落在 `spec.regions()` 里的写。被排除的两类固定下来：
//   - 栈区（0x4000_0000 起那 8 MB，与 driver.cc 的 kStackBase/kStackSize 同）：两边栈帧布局
//     本来就不会一样；
//   - 我们代码区里被改写成 magic 的导入槽：它们本来就应该不同，永远比不等。
// 两类都不在 regions 里，所以一条规则就够了。
std::vector<tc::Store> keep_regions(const std::vector<tc::Store>& seq,
                                   const std::vector<std::pair<u64, u64>>& regions,
                                   size_t& dropped) {
    std::vector<tc::Store> out;
    out.reserve(seq.size());
    for (const tc::Store& s : seq) {
        bool inside = false;
        for (const auto& [lo, hi] : regions) {
            if (s.addr >= lo && s.addr < hi) {
                inside = true;
                break;
            }
        }
        if (inside) out.push_back(s);
        else ++dropped;
    }
    return out;
}

size_t count_writes_at(const std::vector<tc::Store>& seq, u64 addr) {
    size_t n = 0;
    for (const tc::Store& s : seq)
        if (s.addr == addr) ++n;
    return n;
}

// ---- run --------------------------------------------------------------------

void usage_all(const char* argv0) {
    std::printf(
        "用法: %s <子命令> [选项]\n"
        "\n"
        "子命令:\n"
        "  run    --src <si> --bin <blob> [--entry <hex>] [--target <cycles>]\n"
        "         [--seed none|seq|xorshift] [--save <seq>] [--json]\n"
        "         跑一份产物，采一份 store 序列（只采集，不比对）\n"
        "  diff   --state <a.seq> <b.seq> [--src <si>] [--tick-mark <hex>] [--json]\n"
        "  diff   --trace <a.seq> <b.seq> [--json]\n"
        "         比两份落盘序列：--state 按内存状态（主判据），--trace 逐条\n"
        "  where  --seq <seq> --addr <hex> [--src <si> --bin <blob>] [--json]\n"
        "         查序列里写过该地址（或它所在 8 字节窗口）的每一条写\n"
        "\n"
        "公共选项:\n"
        "  --json           结果输出成一个 JSON 对象到 stdout（人读的行转去 stderr）\n"
        "  --dll-dir <dir>  compile_real.dll 所在目录（默认用 image.hh 的 kDllReal；也可用 DIFFTRACE_DLL_DIR）\n"
        "  --help, -h       这份说明；`%s <子命令> --help` 是子命令的说明\n"
        "\n"
        "退出码:\n"
        "  run    0=成功 / 2=参数或模拟错误\n"
        "  diff   0=equal / 1=不等 / 2=错误\n"
        "  where  0=命中 / 1=未命中 / 2=错误\n",
        argv0, argv0);
}

void usage_run(const char* argv0) {
    std::printf(
        "用法: %s run --src <si> --bin <blob> [--entry <hex>] [--target <cycles>]\n"
        "           [--seed none|seq|xorshift] [--save <seq>] [--json] [--dll-dir <dir>]\n"
        "  --src     .si（协议区地址、数组声明、region 集合都由它定）\n"
        "  --bin     产物 blob（8 字节头 + 机器码）\n"
        "  --entry   入口偏移（相对机器码起点）。省略时自动解析：\n"
        "              1) <blob>.entry（我们产物旁边那份，裸十六进制）\n"
        "              2) blob 上级目录的 manifest.tsv / manifest-capture.tsv 里 out_sha256 相同\n"
        "                 的那一行的 entry（corpus 表）或 r2（游戏内捕获表）列\n"
        "  --target  sim_target_cycle，默认 100\n"
        "  --seed    `#RESET_DATA_*` 灌什么：seq（默认）/ xorshift / none（全零）\n"
        "  --save    跑完后把 store 序列落盘（`tc::save` 的格式，给 diff / where 用）\n"
        "  --json    结果输出成一个 JSON 对象到 stdout\n"
        "  --dll-dir compile_real.dll 所在目录（默认用 image.hh 的 kDllReal；也可用 DIFFTRACE_DLL_DIR）\n"
        "退出码: 0=成功 / 2=参数或模拟错误\n",
        argv0);
}

void usage_diff(const char* argv0) {
    std::printf(
        "用法: %s diff --state <a.seq> <b.seq> [--src <si>] [--tick-mark <hex>] [--json]\n"
        "      %s diff --trace <a.seq> <b.seq> [--json]\n"
        "  --state      按内存状态比：整段序列折叠成终态、逐字节比（主判据）。两边产物结构不同\n"
        "               也成立，是「与参考等价」的判据\n"
        "  --trace      逐条比 (pc, addr, size, value)：只对「同一编译器前后两版」成立\n"
        "  --src        比哪些地址由 spec.regions() 定（栈区、导入槽不算）。不给就不过滤，\n"
        "               结论只会更严\n"
        "  --tick-mark  每 tick 写一次的地址，只用来切逐 tick 线索。不给就不做逐 tick；\n"
        "               给了但该地址在过滤后的序列里没出现过 → 报错（不许静默退化成终态比）\n"
        "  --json       结果输出成一个 JSON 对象到 stdout\n"
        "退出码: 0=equal / 1=不等 / 2=错误\n",
        argv0, argv0);
}

void usage_where(const char* argv0) {
    std::printf(
        "用法: %s where --seq <seq> --addr <hex> [--src <si> --bin <blob>] [--json]\n"
        "           [--dll-dir <dir>]\n"
        "  --seq   要查的落盘序列\n"
        "  --addr  查这个地址（连同它所在的 8 字节窗口：宽写与窄写都算命中）\n"
        "  --src   给出它就把每条命中的写按 pc 反汇编（--src 与 --bin 要一起给；\n"
        "          entry 自动解析，规则同 run）\n"
        "  --bin   机器码来源\n"
        "  --json  结果输出成一个 JSON 对象到 stdout\n"
        "退出码: 0=命中 / 1=未命中 / 2=错误\n",
        argv0);
}

int cmd_run(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string src, bin, save_path;
    std::optional<u64> entry_arg;
    std::optional<std::string> dll_dir;
    u64 target = 100;
    tc::Seed seed = tc::Seed::kSeq;  // 默认灌 seq：全零等于「reset_sim 的拷贝等于没拷」
    Io io;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (is_help(a)) {
            usage_run(argv[0]);
            return kExitOk;
        } else if (a == "--src" && more) {
            src = argv[++i];
        } else if (a == "--bin" && more) {
            bin = argv[++i];
        } else if (a == "--entry" && more) {
            entry_arg = parse_u64(argv[++i], "--entry");
        } else if (a == "--target" && more) {
            target = parse_u64(argv[++i], "--target");
        } else if (a == "--seed" && more) {
            seed = parse_seed(argv[++i]);
        } else if (a == "--save" && more) {
            save_path = argv[++i];
        } else if (a == "--dll-dir" && more) {
            dll_dir = argv[++i];
        } else if (a == "--json") {
            io.json = true;
        } else {
            usage_run(argv[0]);
            throw std::runtime_error("run 认不出的参数: " + a);
        }
    }
    if (io.json) io.text = stderr;
    if (src.empty() || bin.empty()) {
        usage_run(argv[0]);
        throw std::runtime_error("run 要 --src 与 --bin");
    }

    const EntryChoice entry = resolve_entry(bin, entry_arg);
    const std::string dll_arg = resolve_dll_path(dll_dir);
    const std::string dll_path = dll_display_path(dll_arg);

    const Spec spec = Spec::parse(src);
    const Image image = load_image(bin, spec, entry.value, dll_arg);

    uc_engine* uc = nullptr;
    const uc_err err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) throw std::runtime_error(std::string("uc_open 失败: ") + uc_strerror(err));

    tc::RunOutcome out;
    try {
        out = tc::run_to_cycle(uc, spec, image, target, seed);
    } catch (...) {
        uc_close(uc);
        throw;
    }
    uc_close(uc);

    if (!save_path.empty()) {
        tc::save(save_path, out.stores);
        say(io, "序列已落盘       : %s（%zu 条）\n", save_path.c_str(), out.stores.size());
    }

    // 正常收工只有这两种停法（`running_zero` = 精确、`sampled` = 兜底）。
    const bool ok = out.result.reason == "running_zero" || out.result.reason == "sampled";

    const u64 state = *spec.addr("SIMULATION_STATE");
    const u64 watched = state + std::stoull(kState2663Offset);
    std::vector<u64> watched_values;
    for (const tc::Store& s : out.stores)
        if (s.addr == watched) watched_values.push_back(s.value);

    say(io, "entry            : %s（%s）\n", hex(entry.value).c_str(), entry.source.c_str());
    say(io, "段 / 字节        : %zu 段、%llu B（uc_mem_map_ptr %zu 段）\n", out.layout.regions,
        static_cast<unsigned long long>(out.layout.mapped_bytes), out.host_regions.size());
    say(io, "DLL 镜像         : %s base %s、%zu B、导入槽 %zu\n", dll_path.c_str(),
        hex(image.dll.base).c_str(), image.dll.image.size(), image.dll.iat.size());
    say(io, "RESET_DATA 图案  : %s（%zu 块、%llu B）\n", seed_name(seed), out.seeded_blocks,
        static_cast<unsigned long long>(out.seeded_bytes));
    // 启动期 `rep movsb` 特判：命中几次、每次的现场。它把一条按字节执行的指令换成了宿主
    // memmove，所以 pc / count / src / dst 必须报出来（不报就成了看不见的魔数）。
    say(io, "rep movsb 特判   : 命中 %zu 次、共 %llu B\n", out.rep_copies.size(),
        static_cast<unsigned long long>(out.rep_bytes));
    for (size_t i = 0; i < out.rep_copies.size(); ++i) {
        const tc::RepCopy& r = out.rep_copies[i];
        say(io, "  [%2zu] pc=%s count=%llu src=%s dst=%s\n", i, hex(r.pc).c_str(),
            static_cast<unsigned long long>(r.count), hex(r.src).c_str(), hex(r.dst).c_str());
    }
    for (const std::string& w : out.warnings) say(io, "warning          : %s\n", w.c_str());
    say(io, "dll_refs 站点    : 改写 %zu、原样 %zu（共 %zu）\n", out.refs.patched,
        out.refs.untouched, image.dll_refs.size());
    size_t stub_code = 0;
    for (const StubRef& ref : image.stub_refs)
        if (ref.kind == StubKind::Code) ++stub_code;
    say(io, "stub_refs 站点   : 改写 %zu（code 桩 %zu / slot %zu）\n", out.refs.stubs, stub_code,
        image.stub_refs.size() - stub_code);
    say(io, "执行指令数       : %llu（启动 %llu + run %llu）\n",
        static_cast<unsigned long long>(out.result.instrs),
        static_cast<unsigned long long>(out.instrs_boot),
        static_cast<unsigned long long>(out.result.instrs - out.instrs_boot));
    say(io, "store 条数       : %llu\n", static_cast<unsigned long long>(out.result.stores));
    say(io, "settings 地址    : %s（StateIndex[sim_running] = %llu → 监视 %s）\n",
        hex(out.settings_addr).c_str(), static_cast<unsigned long long>(out.sim_running_index),
        hex(out.sim_running_addr).c_str());
    say(io, "sim_running 写   : %zu 次（只在发命令之后监视）", out.running_writes.size());
    if (!out.running_writes.empty()) {
        say(io, "，值：");
        for (size_t i = 0; i < out.running_writes.size(); ++i) {
            const tc::RunningWrite& w = out.running_writes[i];
            say(io, "%s%s(size=%u @pc=%s)", i ? " " : "", hex(w.value).c_str(), w.size,
                hex(w.pc).c_str());
        }
    }
    say(io, "\n");
    say(io, "sim_cycle        : %llu\n", static_cast<unsigned long long>(out.result.sim_cycle));
    say(io, "停止原因         : %s\n", out.result.reason.c_str());
    say(io, "error_buffer len : %u\n", static_cast<unsigned>(out.result.error_len));
    say(io, "停下的 pc        : %s\n", hex(out.stop_pc).c_str());

    say(io, "\n前 20 条 store（pc / addr / size / value）：\n");
    const size_t n = std::min<size_t>(out.stores.size(), 20);
    for (size_t i = 0; i < n; ++i) {
        const tc::Store& s = out.stores[i];
        say(io, "  [%2zu] pc=%s addr=%s size=%u value=%s\n", i, hex(s.pc).c_str(),
            hex(s.addr).c_str(), s.size, hex(s.value).c_str());
    }

    say(io, "\n#SIMULATION_STATE + %s 被写 %zu 次", kState2663Offset, watched_values.size());
    if (!watched_values.empty()) {
        say(io, "，值：");
        for (size_t i = 0; i < watched_values.size(); ++i)
            say(io, "%s%s", i ? " " : "", hex(watched_values[i]).c_str());
    }
    say(io, "\n");
    say(io, "import 为空的站点 : 踩到 %llu 次", static_cast<unsigned long long>(out.unnamed_hits));
    if (out.unnamed_first_pc != 0) say(io, "，首次 pc=%s", hex(out.unnamed_first_pc).c_str());
    say(io, "\n");

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    if (io.json) {
        Json j;
        j.flag("ok", ok);
        j.text("subcommand", "run");
        j.num("entry", entry.value);
        j.text("entry_hex", hex(entry.value));
        j.text("entry_source", entry.source);
        j.num("target", target);
        j.text("seed", seed_name(seed));
        j.num("sim_cycle", out.result.sim_cycle);
        j.num("store_count", out.stores.size());
        // `stub_hits` = 被改写成 magic 的 sidecar 桩站点数（我们的产物走 `.stubs`，参考产物走
        // DLL thunk，后者记在 `dll_refs_patched`）；`unnamed_hits` 是 import 为空的站点被踩到的次数。
        j.num("stub_hits", out.refs.stubs);
        j.num("stub_sites", image.stub_refs.size());
        j.num("dll_refs_patched", out.refs.patched);
        j.num("unnamed_hits", out.unnamed_hits);
        j.num("instrs", out.result.instrs);
        j.num("instrs_boot", out.instrs_boot);
        j.num("code_bytes", image.code.size());
        j.text("reason", out.result.reason);
        j.num("error_len", out.result.error_len);
        j.num("stop_pc", out.stop_pc);
        j.text("src", src);
        j.text("bin", bin);
        j.text("save", save_path);
        j.text("dll", dll_path);
        j.num("elapsed_ms", static_cast<u64>(elapsed_ms));
        std::printf("%s\n", j.str().c_str());
    }
    return ok ? kExitOk : kExitError;
}

// ---- diff -------------------------------------------------------------------

int cmd_diff(int argc, char** argv) {
    std::string path_a, path_b, src;
    std::optional<u64> tick_mark;
    bool trace = false;
    bool have_state = false;
    Io io;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (is_help(a)) {
            usage_diff(argv[0]);
            return kExitOk;
        } else if (a == "--state") {
            if (i + 2 >= argc) throw std::runtime_error("--state 要两个路径：<a.seq> <b.seq>");
            path_a = argv[i + 1];
            path_b = argv[i + 2];
            have_state = true;
            i += 2;
        } else if (a == "--trace") {
            if (i + 2 >= argc) throw std::runtime_error("--trace 要两个路径：<a.seq> <b.seq>");
            path_a = argv[i + 1];
            path_b = argv[i + 2];
            trace = true;
            i += 2;
        } else if (a == "--src" && i + 1 < argc) {
            src = argv[++i];
        } else if (a == "--tick-mark" && i + 1 < argc) {
            tick_mark = parse_u64(argv[++i], "--tick-mark");
        } else if (a == "--json") {
            io.json = true;
        } else {
            usage_diff(argv[0]);
            throw std::runtime_error("diff 认不出的参数: " + a);
        }
    }
    if (io.json) io.text = stderr;
    if (have_state == trace) {
        usage_diff(argv[0]);
        throw std::runtime_error("diff 要给且只给一个 --state 或 --trace");
    }

    if (trace) {
        if (tick_mark) throw std::runtime_error("--tick-mark 只跟 --state 一起用（--trace 是逐条比，没有 tick）");
        if (!src.empty()) throw std::runtime_error("--trace 不读 --src（逐条比不看 region 集合）");
        const std::vector<tc::Store> a = tc::load(path_a);
        const std::vector<tc::Store> b = tc::load(path_b);
        const std::optional<tc::Divergence> div = tc::first_divergence(a, b);
        const bool equal = !div;

        say(io, "A                : %s\n", path_a.c_str());
        say(io, "B                : %s\n", path_b.c_str());
        say(io, "条数             : A %zu / B %zu\n", a.size(), b.size());
        if (equal) {
            say(io, "比对             : equal\n");
        } else if (div->length_only) {
            say(io, "比对             : 只是长度差（index %zu = 短的一边条数，短的是长的前缀）\n",
                div->index);
        } else {
            say(io, "比对             : 分叉 @ index %zu\n", div->index);
            print_store(io, "A", div->index, div->a);
            print_store(io, "B", div->index, div->b);
        }
        say(io, "\n");
        print_addr_histogram(io, "A", a);
        print_addr_histogram(io, "B", b);

        if (io.json) {
            Json j;
            j.flag("ok", true);
            j.text("subcommand", "diff");
            j.text("mode", "trace");
            j.flag("equal", equal);
            j.put("ticks", "null");
            j.put("diff_bytes", "null");
            j.put("first_diff_tick", "null");
            j.size("stores_a", a.size());
            j.size("stores_b", b.size());
            j.opt_size("first_diff_index", div ? std::optional<size_t>(div->index) : std::nullopt);
            if (div) j.flag("length_only", div->length_only);
            else j.put("length_only", "null");
            j.put("divergence", div && !div->length_only
                                    ? "{\"a\":" + store_json(div->a) + ",\"b\":" + store_json(div->b) + "}"
                                    : std::string("null"));
            std::printf("%s\n", j.str().c_str());
        }
        return equal ? kExitOk : kExitDiff;
    }

    const std::vector<tc::Store> raw_a = tc::load(path_a);
    const std::vector<tc::Store> raw_b = tc::load(path_b);
    std::vector<tc::Store> a;
    std::vector<tc::Store> b;
    size_t dropped_a = 0;
    size_t dropped_b = 0;
    if (src.empty()) {
        a = raw_a;
        b = raw_b;
    } else {
        const std::vector<std::pair<u64, u64>> regions = Spec::parse(src).regions();
        a = keep_regions(raw_a, regions, dropped_a);
        b = keep_regions(raw_b, regions, dropped_b);
    }

    // 界标先验：界标不在过滤后的序列里 = `split_ticks` 只会给出一段，逐 tick 会静默退化成终态比。
    // 这正是「传了标量地址（region 是 (a,a)，恒不过滤）」踩过的坑，所以它在任何结论打印之前就报错。
    size_t mark_a = 0;
    size_t mark_b = 0;
    size_t mark_raw_a = 0;
    size_t mark_raw_b = 0;
    if (tick_mark) {
        mark_a = count_writes_at(a, *tick_mark);
        mark_b = count_writes_at(b, *tick_mark);
        mark_raw_a = count_writes_at(raw_a, *tick_mark);
        mark_raw_b = count_writes_at(raw_b, *tick_mark);
        if (mark_a == 0 || mark_b == 0) {
            char msg[1024];
            std::snprintf(msg, sizeof(msg),
                          "--tick-mark %s 在过滤后的序列里出现 A %zu 次 / B %zu 次"
                          "（原始序列 A %zu / B %zu）；该地址出现 0 次。"
                          "如果是标量地址，它不在 regions 里（`s.addr < hi` 判归属，标量 region 是 "
                          "(addr,addr) 恒不匹配），逐 tick 需要的是序列里真被写过的地址。",
                          hex(*tick_mark).c_str(), mark_a, mark_b, mark_raw_a, mark_raw_b);
            throw std::runtime_error(msg);
        }
    }

    // 判据：折完整序列的终态。不靠界标，所以界标选错也不会把结论带偏，所以它是第一行。
    const tc::Memory final_a = tc::fold(a, 0, a.size());
    const tc::Memory final_b = tc::fold(b, 0, b.size());
    std::vector<tc::ByteDiff> final_rows;
    const size_t final_count = tc::memory_diff(final_a, final_b, kMaxMismatchPrint, final_rows);
    const bool equal = final_count == 0;

    say(io, "最终状态         : %s\n", equal ? "equal（判据通过）" : "不等（判据不过）");
    if (final_count != 0) print_byte_diffs(io, final_rows, final_count);
    say(io, "A                : %s（%zu 条写，留下 %zu）\n", path_a.c_str(), raw_a.size(), a.size());
    say(io, "B                : %s（%zu 条写，留下 %zu）\n", path_b.c_str(), raw_b.size(), b.size());
    if (src.empty()) {
        say(io, "范围             : 没给 --src → 不做 spec.regions() 过滤（栈区与导入槽也进比对）\n");
    } else {
        say(io, "排除项           : spec.regions() 之外的写不比（栈区 / 我们代码区的导入槽）；"
                   "丢掉 A %zu / B %zu 条\n",
            dropped_a, dropped_b);
    }

    // 逐 tick：不给界标就不做（宁可不给线索，也不给一条看着像判据的假线索）。
    std::optional<size_t> ticks_a;
    std::optional<size_t> ticks_b;
    std::optional<size_t> first_diff_tick;
    size_t tick_diff_bytes = 0;
    std::vector<tc::ByteDiff> tick_rows;
    bool tick_count_mismatch = false;
    if (tick_mark) {
        const std::vector<tc::TickSpan> spans_a = tc::split_ticks(a, *tick_mark);
        const std::vector<tc::TickSpan> spans_b = tc::split_ticks(b, *tick_mark);
        ticks_a = spans_a.size();
        ticks_b = spans_b.size();
        const std::optional<tc::StateDiff> diff =
            tc::first_state_diff(a, b, *tick_mark, kMaxMismatchPrint);
        say(io, "tick 界标        : %s（A %zu 次 / B %zu 次写）\n", hex(*tick_mark).c_str(), mark_a,
            mark_b);
        say(io, "逐 tick 线索     : tick 数 A %zu / B %zu\n", *ticks_a, *ticks_b);
        if (!diff) {
            say(io, "                    每个 tick 折叠后都是 0 差异\n");
        } else if (diff->tick_count) {
            tick_count_mismatch = true;
            tick_diff_bytes = diff->mismatch_count;
            say(io, "                    第一个不同的 tick %zu：两边 tick 数不同（%zu vs %zu），"
                     "之后无从逐 tick 比\n",
                diff->tick, diff->tick_count->first, diff->tick_count->second);
        } else {
            first_diff_tick = diff->tick;
            tick_diff_bytes = diff->mismatch_count;
            tick_rows = diff->mismatches;
            say(io, "                    第一个不同的 tick %zu（0 起）\n", diff->tick);
            print_byte_diffs(io, diff->mismatches, diff->mismatch_count);
        }
    } else {
        say(io, "逐 tick 线索     : 没给 --tick-mark → 只报终态\n");
    }

    if (io.json) {
        Json j;
        j.flag("ok", true);
        j.text("subcommand", "diff");
        j.text("mode", "state");
        j.flag("equal", equal);
        j.opt_size("ticks", ticks_a);
        j.size("diff_bytes", final_count);
        j.opt_size("first_diff_tick", first_diff_tick);
        j.opt_size("ticks_b", ticks_b);
        j.flag("tick_count_mismatch", tick_count_mismatch);
        j.size("tick_diff_bytes", tick_diff_bytes);
        j.opt_num("tick_mark", tick_mark);
        j.put("tick_mark_writes", mark_a || mark_b || mark_raw_a || mark_raw_b
                                     ? "{\"a\":" + std::to_string(mark_a) + ",\"b\":" +
                                           std::to_string(mark_b) + ",\"raw_a\":" +
                                           std::to_string(mark_raw_a) + ",\"raw_b\":" +
                                           std::to_string(mark_raw_b) + "}"
                                     : std::string("null"));
        j.size("stores_a", raw_a.size());
        j.size("stores_b", raw_b.size());
        j.size("kept_a", a.size());
        j.size("kept_b", b.size());
        j.size("dropped_a", dropped_a);
        j.size("dropped_b", dropped_b);
        j.flag("filtered", !src.empty());
        j.text("src", src);
        j.put("final_diff", byte_diff_json(final_rows));
        j.put("tick_diff", byte_diff_json(tick_rows));
        std::printf("%s\n", j.str().c_str());
    }
    return equal ? kExitOk : kExitDiff;
}

// ---- where ------------------------------------------------------------------

// 一个 pc 的反汇编块（`before` / `after` 只是让上手能看清前后文）。
std::string pc_disasm(const tc::Disasm& disasm, const Image& image, u64 pc) {
    const u64 off = pc - image.code_base;
    if (pc < image.code_base || off >= image.code.size()) return "（pc 不在 code 内，跳过反汇编）\n";
    return disasm.range(image.code, image.code_base, off, 1, 2, 1);
}

int cmd_where(int argc, char** argv) {
    std::string seq_path, src, bin;
    std::optional<std::string> dll_dir;
    std::optional<u64> addr;
    Io io;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (is_help(a)) {
            usage_where(argv[0]);
            return kExitOk;
        } else if (a == "--seq" && more) {
            seq_path = argv[++i];
        } else if (a == "--addr" && more) {
            addr = parse_u64(argv[++i], "--addr");
        } else if (a == "--src" && more) {
            src = argv[++i];
        } else if (a == "--bin" && more) {
            bin = argv[++i];
        } else if (a == "--dll-dir" && more) {
            dll_dir = argv[++i];
        } else if (a == "--json") {
            io.json = true;
        } else {
            usage_where(argv[0]);
            throw std::runtime_error("where 认不出的参数: " + a);
        }
    }
    if (io.json) io.text = stderr;
    if (seq_path.empty() || !addr) {
        usage_where(argv[0]);
        throw std::runtime_error("where 要 --seq 与 --addr");
    }
    // 反汇编要机器码，所以要整组；只给一半会让「为什么没反汇编」变成猜的。
    if (src.empty() != bin.empty()) {
        usage_where(argv[0]);
        throw std::runtime_error("where 的反汇编要么不给 --src / --bin，要么两个都给");
    }

    const std::vector<tc::Store> seq = tc::load(seq_path);
    const u64 window_lo = *addr - *addr % kWhereWindow;
    const u64 window_hi = window_lo + kWhereWindow;

    std::vector<std::pair<size_t, tc::Store>> hits;
    for (size_t i = 0; i < seq.size(); ++i) {
        const tc::Store& s = seq[i];
        if (s.addr < window_hi && window_lo < s.addr + s.size) hits.emplace_back(i, s);
    }

    say(io, "序列             : %s（%zu 条写）\n", seq_path.c_str(), seq.size());
    say(io, "窗口             : [%s, %s)（`--addr %s` 所在的 %llu 字节窗口）\n",
        hex(window_lo).c_str(), hex(window_hi).c_str(), hex(*addr).c_str(),
        static_cast<unsigned long long>(kWhereWindow));
    say(io, "命中             : %zu 条\n", hits.size());
    for (const auto& [index, s] : hits) print_store(io, "", index, s);

    // 每条不同 pc 只反汇编一次：同一个写指令写多次时重复反汇编只会把输出灌满。
    std::vector<u64> seen;
    std::vector<std::pair<u64, std::string>> disasm_rows;
    if (!bin.empty()) {
        const EntryChoice entry = resolve_entry(bin, std::nullopt);
        const Image image = load_image(bin, Spec::parse(src), entry.value, resolve_dll_path(dll_dir));
        const tc::Disasm disasm;
        say(io, "\n反汇编（每条不同的 pc，before=2 after=1）：\n");
        for (const auto& hit : hits) {
            const tc::Store& s = hit.second;
            if (std::find(seen.begin(), seen.end(), s.pc) != seen.end()) continue;
            seen.push_back(s.pc);
            const std::string text = pc_disasm(disasm, image, s.pc);
            say(io, "  pc=%s（code+0x%llx）\n", hex(s.pc).c_str(),
                static_cast<unsigned long long>(s.pc - image.code_base));
            say(io, "%s", text.c_str());
            disasm_rows.emplace_back(s.pc, text);
        }
    } else {
        say(io, "\n反汇编           : 没给 --src / --bin，只列写\n");
    }

    if (io.json) {
        Json j;
        j.flag("ok", true);
        j.text("subcommand", "where");
        j.num("addr", *addr);
        j.text("addr_hex", hex(*addr));
        j.put("window", "[" + std::to_string(window_lo) + "," + std::to_string(window_hi) + "]");
        j.text("seq", seq_path);
        j.size("store_count", seq.size());
        j.size("hit_count", hits.size());
        std::string arr = "[";
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i) arr += ',';
            Json o;
            o.size("index", hits[i].first);
            o.num("pc", hits[i].second.pc);
            o.num("addr", hits[i].second.addr);
            o.num("size", hits[i].second.size);
            o.num("value", hits[i].second.value);
            o.text("pc_hex", hex(hits[i].second.pc));
            o.text("addr_hex", hex(hits[i].second.addr));
            o.text("value_hex", hex(hits[i].second.value));
            arr += o.str();
        }
        arr += ']';
        j.put("hits", arr);
        std::string dis = "[";
        for (size_t i = 0; i < disasm_rows.size(); ++i) {
            if (i) dis += ',';
            Json o;
            o.num("pc", disasm_rows[i].first);
            o.text("pc_hex", hex(disasm_rows[i].first));
            o.text("text", disasm_rows[i].second);
            dis += o.str();
        }
        dis += ']';
        j.put("disasm", disasm_rows.empty() ? std::string("null") : dis);
        std::printf("%s\n", j.str().c_str());
    }
    return hits.empty() ? kExitDiff : kExitOk;
}

}  // namespace

// `diff.cc` 的异常自带 `difftrace: ` 前缀（它不知道谁在调用），`main` 再拼上子命令就成了
// `difftrace diff: difftrace: 打不开…`。只剥这一层，正文原样。
static std::string strip_tool_prefix(std::string msg) {
    const std::string prefix = "difftrace: ";
    if (msg.rfind(prefix, 0) == 0) msg.erase(0, prefix.size());
    return msg;
}

int main(int argc, char** argv) {
    // stdout / stderr 都走二进制模式：CRT 的文本模式会把 `\n` 翻成 `\r\n`，而 mingw 的 ANSI
    // stdio 实现（`std::printf` 走它）不翻 —— 同一个程序里两条输出路径会给出两种行尾，
    // 落盘后就不能跟旧输出逐字节比。
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    // `--json` 要在异常路径上也能用（结果输出成一个 JSON 对象），所以先扫一遍：它是无值 flag。
    bool json = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--json") == 0) json = true;

    if (argc < 2) {
        usage_all(argv[0]);
        return kExitError;
    }
    if (is_help(argv[1])) {
        usage_all(argv[0]);
        return kExitOk;
    }
    const std::string sub = argv[1];
    if (sub != "run" && sub != "diff" && sub != "where") {
        std::fprintf(stderr, "difftrace: 认不出的子命令: %s\n\n", argv[1]);
        usage_all(argv[0]);
        return kExitError;
    }

    try {
        if (sub == "run") return cmd_run(argc, argv);
        if (sub == "diff") return cmd_diff(argc, argv);
        return cmd_where(argc, argv);
    } catch (const std::exception& e) {
        const std::string msg = strip_tool_prefix(e.what());
        std::fprintf(stderr, "difftrace %s: %s\n", sub.c_str(), msg.c_str());
        if (json) {
            Json j;
            j.flag("ok", false);
            j.text("subcommand", sub);
            j.text("error", msg);
            std::printf("%s\n", j.str().c_str());
        }
        return kExitError;
    }
}
