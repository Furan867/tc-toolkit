/* simplexc_core.cc —— `simplexc_core.h` 的实现：内存里的 Simplex 源码 → 游戏可 JIT 的 blob。
 *
 * 这一层是**唯一**被 C 壳（`tools/compile_hook.c`）调用的入口，也是 C 与 C++ 的唯一交界：
 * 边界是 C（纯 C 头 + `extern "C"` 定义），因为壳是 C 且继续演进，C++ 名字改编会让链接找不到符号；
 * 异常一律在**这里**折成返回码 —— 穿过 Nim 帧的展开没有保证（接口冻结版 §5）。
 *
 * 与 CLI（`cli/simplexc_cli.cc`）共用同一套核心：`frontend` → `build_module`（`codegen/module_build.hh`）
 * → `sema::analyze` → `codegen::Program` → `assemble()`。差别只有两处，都是「在游戏进程里」逼出来的：
 *   · 源码在内存里（没有临时文件可写）→ `Frontend(SrcText{...})`；
 *   · 装配的 `entry_shim.o` / `rt_min.o` 在构建期嵌进了 DLL → `embedded_objs.hh`。
 *
 * 不写文件、不动可观察的全局状态、不抛异常 —— 调用方仍应包 VEH（熔断是壳的责任）。
 */
#include "simplexc_core.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
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

/* 失败分类串：就是 `simplexc_core.h` 里列的那几个名字（接口 §8），只在这里拼一次。 */
constexpr const char* kUserError = "UserError";
constexpr const char* kUnsupportedConstruct = "UnsupportedConstruct";
constexpr const char* kInternalError = "InternalError";
constexpr const char* kBlobTooLarge = "BlobTooLarge";
constexpr const char* kRelocUnsupported = "RelocUnsupported";
constexpr const char* kSymbolUndefined = "SymbolUndefined";

/* 最近一次失败的诊断串（三轴都在里面，见 `simplexc_last_error()`）。
   静态缓冲：C 边界不给动态所有权，而 `simplexc_compile` 的签名被壳固定住了。
   壳在同一次调用里读完即弃，所以只有「下一次失败」会覆盖它 —— 不会跨调用串值。 */
char g_last_error[512];

/* 最近一次的**非致命观察**（解析期 notes、「不可达 def 被跳过」）。长度上限写在 header 里
 * （“同一次调用里读完即弃”），这里负责拼接与显式截断 —— 内容里带用户源码的标识符，
 * 长度不受我们控制，所以不用 `+=`，也不手算长度。 */
char g_last_warnings[1024];

/* notes 只是一行行独立事实，拼成一条串的目的是让 C 边界只多一个函数。`; ` 作分隔是因为
 * `；` 与 `;` 在日志里都常见得让人看不出来（用户的那条 note 里就有 `U8(…)`）。 */
void set_last_warnings(const std::vector<std::string>& notes) {
  std::string joined;
  for (const std::string& n : notes) {
    if (!joined.empty()) joined += "; ";
    joined += n;
  }
  std::snprintf(g_last_warnings, sizeof g_last_warnings, "%s", joined.c_str());
}

/* 产物上限（接口 §4 第 6 条，与装配器同一个数）与头后的尾部 padding（第 8 条：
 * 游戏会读到末尾之外，所以要多分配 8 字节）。上限在这里再判一次：malloc 的尺寸由本文件算。 */
constexpr std::size_t kMaxBlobBytes = 64u << 20;
constexpr std::size_t kTailPadding = 8;

/* `Blob::bytes` 的头 8 字节（bit62 → 游戏不 free 我们的缓冲，主设计 §2.3）。 */
constexpr std::size_t kHeaderBytes = 8;

/* 与 CLI 同一份查表（`cli/win32_export.hh`：kernel32 → ucrtbase → msvcrt），`__imp_X` 的剥法与
 * 模块顺序都只在那边定义。
 *
 * 为什么调 `find_export` 而不是 `resolve_win32_export`：后者失败时会往 stderr 打「查过哪些模块」，
 * 而壳在游戏进程里跑，stderr 进的是 hook.log —— 那是唯一一份记录型日志，多一行就多一处噪声。
 * 壳的失败信息已经由 `assemble()` 的 `res.detail`（符号名）经 `[simplexc] 装配失败：…` 报出。 */
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

/* 装配的三个输入，顺序与 CLI 一致（模块 COFF + shim + runtime）。
   后两个是构建期嵌进 DLL 的字节 —— 游戏进程里没有 build/objs/。 */
std::vector<tc::ObjectFile> assemble_inputs(std::vector<uint8_t> module_obj) {
  std::vector<tc::ObjectFile> objs;
  objs.push_back({"module", std::move(module_obj)});

  tc::ObjectFile shim;
  shim.name = "entry_shim.o";
  shim.bytes.assign(sh::kEntryShimObj, sh::kEntryShimObj + sh::kEntryShimObjSize);
  objs.push_back(std::move(shim));

  tc::ObjectFile rt;
  rt.name = "rt_min.o";
  rt.bytes.assign(sh::kRtMinObj, sh::kRtMinObj + sh::kRtMinObjSize);
  objs.push_back(std::move(rt));
  return objs;
}

/* 装配失败 → 分类串。`BadObject` 没有自己的类别（§8 的七个名字里没有它）→ InternalError：
 * 它只可能是「我们喂了非 COFF」，不是源码的构造问题。 */
const char* asm_kind(tc::AsmStatus status) {
  switch (status) {
    case tc::AsmStatus::TooLarge:
      return kBlobTooLarge;
    case tc::AsmStatus::UnsupportedReloc:
      return kRelocUnsupported;
    case tc::AsmStatus::UndefinedSymbol:
      return kSymbolUndefined;
    case tc::AsmStatus::Ok:
    case tc::AsmStatus::BadObject:
      break;
  }
  return kInternalError;
}

/* 唯一一处把产物拷给游戏的代码。调用前已确认：
 *   `code_len ≥ 8`、`code_len ≤ kMaxBlobBytes`、`entry ≤ code_len - 8`（入口在机器码内）。
 * 成功时缓冲的所有权**转移给调用方**（壳下次编译开始时用 `simplexc_free` 放掉）。 */
void* materialize(const tc::Blob& blob, long long* out_len, long long* out_entry) {
  const std::size_t code_len = blob.bytes.size();
  const std::size_t alloc_len = code_len + kTailPadding;
  if (alloc_len < code_len) return nullptr; /* size_t 溢出兜底（code_len 已被上限卡住，这只是防线） */

  void* buf = std::malloc(alloc_len);
  if (buf == nullptr) return nullptr;

  unsigned char* dst = static_cast<unsigned char*>(buf);
  /* 头单独搬：bit62 在这里兜一道（装配器已置位；漏了游戏会拿 Nim 的分配器去 free 我们的指针）。 */
  unsigned long long header = 0;
  std::memcpy(&header, blob.bytes.data(), kHeaderBytes);
  header |= 1ull << 62;
  std::memcpy(dst, &header, kHeaderBytes);
  std::memcpy(dst + kHeaderBytes, blob.bytes.data() + kHeaderBytes, code_len - kHeaderBytes);
  std::memset(dst + code_len, 0, kTailPadding);

  *out_len = static_cast<long long>(code_len);
  *out_entry = static_cast<long long>(blob.entry_off);
  return buf;
}

}  // namespace

extern "C" const char* simplexc_compile(const char* src, long long src_len, void** out_blob,
                                        long long* out_len, long long* out_entry) {
  /* 出参先判空：失败时它们**一个都不动**（头文件的承诺），成功那一刻才一起写。 */
  if (src == nullptr || src_len < 0 || out_blob == nullptr || out_len == nullptr ||
      out_entry == nullptr) {
    return kInternalError;
  }

  try {
    const std::string text(src, static_cast<std::size_t>(src_len));
    g_last_error[0] = '\0';
    g_last_warnings[0] = '\0'; /* 观察串也只跟本次调用走 */
    sx::Frontend fe(sx::SrcText{text});
    /* 声明层错误（原型 `Frontend.decl_errors`）不是异常，但同样是我们不支持的源码形态。
       为什么直接判 InternalError 而不是继续 lowering：声明表不全时 lowering 只会报更离谱的错。 */
    if (!fe.decl_errors.empty()) {
      std::cerr << "[simplexc] 声明层错误：" << fe.decl_errors.front() << std::endl;
      return kInternalError;
    }

    lw::OwnedModule owned = lw::build_module(fe, fe.src_path);
    /* notes 在 lowering **之前**就搬进静态缓冲：它们描述的是前端已经发生的事，
       后面 sema/codegen 失败也不应该把它们弄丢（失败时用户更需要知道「这里被略过了什么」）。 */
    set_last_warnings(owned.notes);
    const sm::Hir hir = sm::analyze(cg::module_source_of(owned.ast));
    cg::Program prog(hir);
    prog.build();
    std::vector<uint8_t> module_obj = prog.emit_object();

    Win32Resolver resolver;
    tc::AsmResult assembled = tc::assemble(assemble_inputs(std::move(module_obj)), "__tc_entry",
                                           resolver);
    if (assembled.status != tc::AsmStatus::Ok) {
      std::cerr << "[simplexc] 装配失败：" << assembled.detail << std::endl;
      return asm_kind(assembled.status);
    }

    const std::size_t code_len = assembled.blob.bytes.size();
    if (code_len < kHeaderBytes) return kInternalError; /* 连头都不够：装配器给了个畸形产物 */
    if (code_len > kMaxBlobBytes) return kBlobTooLarge;
    if (assembled.blob.entry_off > code_len - kHeaderBytes) {
      return kInternalError; /* 入口落在机器码之外 → 游戏会跳飞 */
    }

    void* buf = materialize(assembled.blob, out_len, out_entry);
    if (buf == nullptr) return kInternalError;
    *out_blob = buf;
    return nullptr;
  } catch (const lw::Unsupported& e) {
    std::cerr << "[simplexc] 未支持构造：第 " << e.line() << " 行 " << e.what() << std::endl;
    return kUnsupportedConstruct;
  } catch (const sm::CompileError& e) {
    /* 三轴在 `e.what()` 里（`sema/errors.hh`），并原样交给壳（`simplexc_last_error`）——
       游戏是 GUI 进程，看不到我们的 stderr，失败信息得过 C 边界。分类按**责任轴**分三档：
       用户能改对的源码错 = `UserError`（壳不转交：转交只会白跑一次参考编译，还污染
       回退率的分母）；我们没实现的构造 = `UnsupportedConstruct`；其余（断言失败、同一
       输入两次不一致）= `InternalError`。这一档是壳决定回不回的判据（接口 §8）。 */
    std::cerr << "[simplexc] 编译失败：" << e.what() << std::endl;
    std::snprintf(g_last_error, sizeof g_last_error, "%s", e.what());
    switch (e.resp) {
      case sm::Responsibility::UserSource:
        return kUserError;
      case sm::Responsibility::Unimplemented:
        return kUnsupportedConstruct;
      case sm::Responsibility::OurBug:
        break;
    }
    return kInternalError;
  } catch (const lw::CompileFail& e) {
    std::cerr << "[simplexc] 编译失败：" << e.what() << std::endl;
    return kInternalError;
  } catch (const std::exception& e) {
    std::cerr << "[simplexc] 内部错误：" << e.what() << std::endl;
    return kInternalError;
  } catch (...) {
    /* 前端抛的是**非 std::exception**的值（`ParseError` / `LexError` / `ReadError` 都是普通结构体）。
       兜住它们同样重要：异常穿出这条边界就是穿过 Nim 帧（接口冻结版 §5）。 */
    std::cerr << "[simplexc] 未知异常（多半是前端错误：语法/词法/读源码）" << std::endl;
    return kInternalError;
  }
}

extern "C" const char* simplexc_last_error(void) { return g_last_error; }

extern "C" const char* simplexc_last_warnings(void) { return g_last_warnings; }

extern "C" void simplexc_free(void* blob) { std::free(blob); }
