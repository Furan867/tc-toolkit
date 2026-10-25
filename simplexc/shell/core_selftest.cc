/* core_selftest.cc —— 判据用的小 main：直接调 `simplexc_compile`，把产物 + extern 跳板清单
 * 落盘（**不进 DLL**）。
 *
 * 为什么需要它：壳在游戏进程里编译，源码只存在于内存、产物只经过 `Result`；离线要复现同一条路径
 * 就得有个能吃源码字节、吐产物字节的入口 —— 就是这个文件（M4.5 判据 J3）。
 * 它的第二条用途：同一份源码下，本入口的产物应当与 `simplexc_cli.exe` 的产物**逐字节相同**
 * （同一套核心、确定性），所以 md5 可以直接当「壳与 CLI 同源」的判据。
 *
 * 为什么还要**自己再走一遍**内部路径（而不是让核心顺手把 stubs 交出来）：
 * `simplexc_core.h` 是冻结的 C 边界，只交游戏要的那四个值 —— 契约里没有 stubs。
 * 而宿主（`tools/host/host.py`）要靠 `<blob>.stubs` 才知道产物里哪些 movabs 是 extern 跳板：
 * 没有它就只看到 movabs 的立即数是本进程里已映射的 kernel32 真实地址，于是按 spec §6 拒绝运行
 * （那些桩在生产进程里是 `GetProcAddress` 解出来的，产物里换一台机器就变）。
 * 所以这里用与 `shell/simplexc_core.cc` **相同**的链（`Frontend(SrcText)` → `build_module` →
 * `sema::analyze` → `codegen::Program` → `emit_object()` → `assemble()`）自己算一遍，拿到
 * `AsmResult::stubs`；同时把这一遍的产物与 `simplexc_compile` 的产物**逐字节比对**，不一致就报错
 * 且不落盘 —— 两条路给出同一个产物这条断言本身就是判据，不成立的话 stubs 的偏移也不再可信。
 *
 * 这条断言**只证明确定性**（同一套链跑两遍 = 同一个产物），不证明语义等价：跨路径的等价性
 * 由 `work/tmp/s2c/dual.py`（同源双跑，75/75）与 `blob_baseline --verify`（98 份语料比参考产物）
 * 承担。旧 lowering 删掉之后这里就没有「第二条路径」可比了，别把这条断言读成那个意思。
 *
 * 用法：core_selftest --src <file.si> --out <blob>   （同时写 <blob>.entry、<blob>.stubs）
 * 退出码：0 成功；1 编译失败 / 两条路不一致；2 读不了源文件 / 写不了产物 / 参数不对。
 */
#include "simplexc_core.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "assemble/assemble.hh"
#include "cli/win32_export.hh"
#include "codegen/codegen.hh"
#include "codegen/module_build.hh"
#include "codegen/module_source.hh"
#include "frontend/frontend.hh"
#include "sema/analyze.hh"
#include "shell/embedded_objs.hh"

namespace sx = simplexc;
namespace cg = simplexc::codegen;
namespace lw = simplexc::lower;
namespace sh = simplexc::shell;
namespace sm = simplexc::sema;

namespace {

void usage() {
  std::cerr << "用法：core_selftest --src <file.si> --out <blob>" << std::endl;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buf;
  buf << in.rdbuf();
  out = buf.str();
  return true;
}

/* 与 `shell/simplexc_core.cc` 的 `Win32Resolver` 逐字相同：查表与 `__imp_` 的剥法都在
   `cli/win32_export.hh`，模块顺序（kernel32 → ucrtbase → msvcrt）只在那里定义。
   失败静默（调 `find_export` 而非 `resolve_win32_export`），与壳在游戏进程里的行为一致 ——
   这个判据入口要复现的是那条路，不是 cli 那条。 */
class Win32Resolver : public tc::ExternResolver {
 public:
  bool resolve(std::string_view symbol, uint64_t& addr) override {
    std::string looked; /* `find_export` 的出参，静默路径上用不上 */
    const FARPROC p =
        simplexc_cli::find_export(simplexc_cli::strip_import_prefix(symbol), looked);
    if (p == nullptr) return false;
    addr = reinterpret_cast<uint64_t>(p);
    return true;
  }
};

/* 与 `shell/simplexc_core.cc` 同一条链、同一批装配输入（后两个目标文件是构建期嵌进来的字节，
   见 `shell/embedded_objs.hh`）。差别只有一处：核心把 `AsmResult` 折成游戏要的四个值，
   这里整个交出来 —— stubs 只在 `AsmResult` 上。 */
tc::AsmResult assemble_collecting_stubs(const std::string& text) {
  sx::Frontend fe(sx::SrcText{text});
  if (!fe.decl_errors.empty()) throw sm::CompileFail("声明层错误：" + fe.decl_errors.front());
  lw::OwnedModule owned = lw::build_module(fe, fe.src_path);
  const sm::Hir hir = sm::analyze(cg::module_source_of(owned.ast));
  cg::Program prog(hir);
  prog.build();

  std::vector<tc::ObjectFile> objs;
  tc::ObjectFile module;
  module.name = "module";
  module.bytes = prog.emit_object();
  objs.push_back(std::move(module));

  tc::ObjectFile shim;
  shim.name = "entry_shim.o";
  shim.bytes.assign(sh::kEntryShimObj, sh::kEntryShimObj + sh::kEntryShimObjSize);
  objs.push_back(std::move(shim));

  tc::ObjectFile rt;
  rt.name = "rt_min.o";
  rt.bytes.assign(sh::kRtMinObj, sh::kRtMinObj + sh::kRtMinObjSize);
  objs.push_back(std::move(rt));

  Win32Resolver resolver;
  return tc::assemble(objs, "__tc_entry", resolver);
}

/* 与 `cli/simplexc_cli.cc` 的 sidecar **逐字节**一致：表头 + `offset<TAB>kind<TAB>symbol`，
   offset 十六进制小写且相对机器码起点，`__imp_` 前缀 → `slot`，其余 → `stub`。
   故意不开 `std::ios::binary`：CLI 那边用 `fopen(..., "w")`（文本模式，'\n' → CRLF），
   两份 sidecar 要能直接比 md5，换行也得一样。 */
bool write_stubs(const std::string& path, const std::vector<tc::StubRef>& stubs) {
  std::ofstream f(path);
  if (!f) return false;
  f << "offset\tkind\tsymbol\n";
  for (const tc::StubRef& s : stubs) {
    std::ostringstream hex;
    hex << std::hex << s.offset;
    f << hex.str() << '\t' << (s.symbol.rfind("__imp_", 0) == 0 ? "slot" : "stub") << '\t'
      << s.symbol << '\n';
  }
  f.close();
  return !f.fail();
}

bool write_entry(const std::string& path, long long entry) {
  std::ofstream e(path);
  if (!e) return false;
  e << std::hex << std::uppercase << static_cast<unsigned long long>(entry) << "\n";
  e.close();
  return !e.fail();
}

}  // namespace

int main(int argc, char** argv) {
  std::string src_path, out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--src" && i + 1 < argc) {
      src_path = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  if (src_path.empty() || out_path.empty()) {
    usage();
    return 2;
  }

  std::string src;
  if (!read_file(src_path, src) || src.empty()) {
    std::cerr << "读不了 " << src_path << "（或它是空文件）" << std::endl;
    return 2;
  }

  void* blob = nullptr;
  long long len = 0;
  long long entry = 0;
  const char* kind = simplexc_compile(src.data(), static_cast<long long>(src.size()), &blob, &len,
                                      &entry);
  if (kind != nullptr) {
    std::cerr << "编译失败：" << kind << "（源码 " << src.size() << " B）" << std::endl;
    return 1;
  }

  /* 内部路径：同一份源码、同一批装配输入，只是为了拿 stubs。它抛异常或装配失败都说明
     「核心成功但这条路走不通」—— 那是判据的失败，不是源码的问题。 */
  tc::AsmResult internal;
  try {
    internal = assemble_collecting_stubs(src);
  } catch (const std::exception& e) {
    std::cerr << "内部路径抛异常：" << e.what() << std::endl;
    simplexc_free(blob);
    return 1;
  } catch (...) {
    /* 前端抛的是非 std::exception 的值（ParseError / LexError / ReadError 都是普通结构体）。 */
    std::cerr << "内部路径抛异常（非 std::exception：多半是前端错误）" << std::endl;
    simplexc_free(blob);
    return 1;
  }
  if (internal.status != tc::AsmStatus::Ok) {
    std::cerr << "内部路径装配失败：status=" << static_cast<int>(internal.status) << " "
              << internal.detail << std::endl;
    simplexc_free(blob);
    return 1;
  }

  /* 两遍必须逐字节相同（同一套链、同一份源码 → 确定性）—— 不然下面写出的 stubs 说的可能是另一个产物。 */
  const auto* bytes = static_cast<const unsigned char*>(blob);
  const std::vector<uint8_t>& got = internal.blob.bytes;
  if (got.size() != static_cast<std::size_t>(len) ||
      std::memcmp(got.data(), bytes, static_cast<std::size_t>(len)) != 0 ||
      internal.blob.entry_off != static_cast<uint64_t>(entry)) {
    std::size_t at = 0;
    while (at < got.size() && at < static_cast<std::size_t>(len) && got[at] == bytes[at]) ++at;
    std::cerr << "两条路产物不一致：core=" << len << " B entry=0x" << std::hex << std::uppercase
              << static_cast<unsigned long long>(entry) << " / 内部路径=" << std::dec << got.size()
              << " B entry=0x" << std::hex << std::uppercase << internal.blob.entry_off << std::dec
              << "（首个不同字节 0x" << std::hex << at << std::dec << "）—— 不写产物" << std::endl;
    simplexc_free(blob);
    return 1;
  }

  uint64_t header = 0;
  for (int k = 0; k < 8; ++k) header |= static_cast<uint64_t>(bytes[k]) << (8 * k);

  {
    std::ofstream out(out_path, std::ios::binary);
    if (!out || !out.write(reinterpret_cast<const char*>(blob), static_cast<std::streamsize>(len))) {
      std::cerr << "写不了产物 " << out_path << std::endl;
      simplexc_free(blob);
      return 2;
    }
  }
  if (!write_entry(out_path + ".entry", entry) ||
      !write_stubs(out_path + ".stubs", internal.stubs)) {
    std::cerr << "写不了 " << out_path << ".entry / .stubs" << std::endl;
    simplexc_free(blob);
    return 2;
  }

  std::cout << "out_len=" << len << " entry=0x" << std::hex << std::uppercase
            << static_cast<unsigned long long>(entry) << std::dec << " 头 bit62="
            << ((header >> 62) & 1) << " 桩数=" << internal.stubs.size() << " → " << out_path
            << std::endl;
  simplexc_free(blob);
  return 0;
}
