/* tcblob.cc —— 装配器的命令行前端（spike 用；agent 日常自测不碰游戏）。
 *
 * 用法
 * ----
 *   tcblob --obj a.obj --obj b.obj --entry __tc_entry --out blob.bin [--run] [--verbose]
 *
 *   --obj     可重复；顺序 = 链接顺序
 *   --entry   entry 符号名（我们的 shim `__tc_entry`）
 *   --run     另用一块可执行内存按**游戏的方式**跑一遍：
 *             分配 out_len 字节 RWX → `memcpy(dest, blob+8, out_len)`（与游戏 `jit` 的字节数一致，
 *             所以会多读 8 字节 —— blob 的缓冲必须带那 8 字节余量）→ `jmp dest+entry`，
 *             在独立线程里跑（程序根末尾是 thread_exit，会结束该线程）。
 *
 * 外部符号解析：按名字在宿主已有的模块里找（顺序见 `win32_export.hh`），失败即报错，不静默。
 */
#include "../assemble/assemble.hh"
#include "win32_export.hh"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* 自测探针的计数器：`--run` 后打印，证明 blob 真的执行到了那一步 */
struct ProbeState {
    uint64_t calls = 0;
    uint64_t last = 0;
};
ProbeState g_probe;
/* 探针：给自测用。blob 里的代码调 `tc_probe_ok(x)` 会落到这里 —— 用它证明
   「extern 桩 → 宿主函数」这条链真的通了（也能在游戏外用同一份产物对账）。 */
extern "C" void tc_probe_ok(uint64_t v);
extern "C" void tc_probe_ok(uint64_t v) {
    g_probe.calls++;
    g_probe.last = v;
}

class Win32Resolver : public tc::ExternResolver {
public:
    bool resolve(std::string_view symbol, uint64_t& addr) override {
        if (simplexc_cli::strip_import_prefix(symbol) == "tc_probe_ok") {   /* 自测探针：不经任何模块 */
            addr = reinterpret_cast<uint64_t>(&tc_probe_ok);
            return true;
        }
        return simplexc_cli::resolve_win32_export(symbol, addr);
    }
};

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "读不到 %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size_t(n > 0 ? n : 0));
    size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

struct ThreadArg {
    uint8_t* target;
};

DWORD WINAPI thread_proc(void* p) {
    auto* a = static_cast<ThreadArg*>(p);
    using fn_t = void (*)();
    reinterpret_cast<fn_t>(a->target)();     /* 游戏也是这么跳的：无参、无返回 */
    return 0;
}

int run_blob(const tc::Blob& blob) {
    /* 与游戏 `jit` 完全同构：多分配 8 字节，memcpy 的长度是 out_len（从这个 8 字节头之后开始） */
    size_t total = size_t(blob.out_len()) + 8;
    uint8_t* dest = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!dest) {
        std::fprintf(stderr, "VirtualAlloc 失败：%lu\n", GetLastError());
        return 1;
    }
    /* 与游戏 `jit` 同构：**从 blob+8 起拷机器码**（不是从头拷，头那 8 字节不进目标缓冲）。
       游戏拷的是 out_len 字节（会多读 8 字节），这里只拷真实码长，剩下那 8 字节是
       VirtualAlloc 给的新页、本来就是 0 —— 效果等价，且不会读越界。 */
    std::memcpy(dest, blob.code(), size_t(blob.code_size()));
    ThreadArg arg{dest + blob.entry_off};
    HANDLE th = CreateThread(nullptr, 0, thread_proc, &arg, 0, nullptr);
    if (!th) {
        std::fprintf(stderr, "CreateThread 失败：%lu\n", GetLastError());
        return 1;
    }
    DWORD w = WaitForSingleObject(th, 10000);
    if (w != WAIT_OBJECT_0) {
        std::fprintf(stderr, "blob 10 s 没结束（WAIT=0x%lX）—— 可能死循环\n", w);
        return 1;
    }
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFree(dest, 0, MEM_RELEASE);
    std::cout << "RUN ok：blob 在 " << static_cast<const void*>(dest)
              << " 上跑完，线程退出码 " << code << "（0 = 正常 ExitThread）\n";
    if (g_probe.calls)
        std::cout << "probe：tc_probe_ok 被调用 " << g_probe.calls
                  << " 次，最后一次参数 = " << g_probe.last << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<tc::ObjectFile> objs;
    std::string entry = "__tc_entry";
    std::string out_path;
    bool run = false;
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--obj") {
            const char* p = next();
            if (!p) { std::fprintf(stderr, "--obj 缺参数\n"); return 2; }
            tc::ObjectFile o;
            o.name = p;
            if (!read_file(p, o.bytes)) return 1;
            objs.push_back(std::move(o));
        } else if (a == "--entry") {
            const char* p = next();
            if (!p) { std::fprintf(stderr, "--entry 缺参数\n"); return 2; }
            entry = p;
        } else if (a == "--out") {
            const char* p = next();
            if (!p) { std::fprintf(stderr, "--out 缺参数\n"); return 2; }
            out_path = p;
        } else if (a == "--run") {
            run = true;
        } else if (a == "--verbose" || a == "-v") {
            verbose = true;
        } else {
            std::fprintf(stderr, "未知参数 %s\n", a.c_str());
            return 2;
        }
    }
    if (objs.empty()) {
        std::fprintf(stderr, "至少给一个 --obj\n");
        return 2;
    }

    Win32Resolver resolver;
    tc::AsmOptions opt;
    opt.verbose = verbose;
    tc::AsmResult r = tc::assemble(objs, entry, resolver, opt);
    if (r.status != tc::AsmStatus::Ok) {
        std::fprintf(stderr, "装配失败（status=%d）：%s\n", int(r.status), r.detail.c_str());
        return 1;
    }
    {
        std::ostringstream line;
        line << "blob: out_len=" << r.blob.out_len() << " entry=0x" << std::hex
             << std::uppercase << r.blob.entry_off << std::dec << " text=" << r.text_size
             << " rdata=" << r.rdata_size << " data=" << r.data_size << " bss=" << r.bss_size
             << " stubs=" << r.stub_count << " relocs=" << r.reloc_count;
        std::cout << line.str() << "\n";
    }
    if (!out_path.empty()) {
        FILE* f = std::fopen(out_path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "写不出 %s\n", out_path.c_str());
            return 1;
        }
        std::fwrite(r.blob.bytes.data(), 1, size_t(r.blob.out_len()), f);
        std::fclose(f);
        std::cout << "写出 " << out_path << "（" << r.blob.out_len() << " 字节）\n";
        /* entry 偏移不在产物里（它是 Result 的字段）→ 写成 `<blob>.entry`（十六进制），
           `tools/compile_hook.c` 的自研路径靠它拿到入口。 */
        std::string epath = out_path + ".entry";
        FILE* fe = std::fopen(epath.c_str(), "w");
        if (fe) {
            std::fprintf(fe, "%llX\n", (unsigned long long)r.blob.entry_off);
            std::fclose(fe);
        } else {
            std::fprintf(stderr, "写不出 %s（壳的自研路径会因无 entry 而回退）\n", epath.c_str());
        }
        /* extern 跳板清单（接口冻结版 1.1 增量）：宿主靠它区分「产物自己的跳板落点」
           与「游戏地址/常量」，并把里面的真实地址换成宿主桩 —— 否则两遍跑的确定性直接没了。
           TSV：表头 `offset<TAB>kind<TAB>symbol`，offset 是**相对机器码起点**的十六进制小写；
           `stub` = 12 字节代码桩（宿主改写其中的 imm64，在 offset+2），
           `slot` = `__imp_X` 的 8 字节数据槽（宿主改写槽内容）。 */
        std::string spath = out_path + ".stubs";
        FILE* fs = std::fopen(spath.c_str(), "w");
        if (fs) {
            std::fprintf(fs, "offset\tkind\tsymbol\n");
            for (const tc::StubRef& s : r.stubs) {
                const char* kind = s.symbol.rfind("__imp_", 0) == 0 ? "slot" : "stub";
                std::fprintf(fs, "%llx\t%s\t%s\n", (unsigned long long)s.offset, kind,
                             s.symbol.c_str());
            }
            std::fclose(fs);
            std::cout << "写出 " << spath << "（" << r.stubs.size() << " 处 extern 跳板）\n";
        } else {
            std::fprintf(stderr, "写不出 %s（宿主会把 extern 桩当未知地址拒跑）\n", spath.c_str());
        }
    }
    if (run) return run_blob(r.blob);
    return 0;
}
