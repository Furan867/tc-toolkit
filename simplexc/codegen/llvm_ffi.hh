/* llvm_ffi.hh —— LLVM-C 的 C++ 薄封装。
 *
 * 为什么只需要「薄」：产品实现是 C++，直接 include `llvm-c/Core.h` 之类的头就够了 —— 原型要
 * ctypes 是为了在 Python 里拿 C 符号。这里剩下的只有两件 Python 侧没有的东西：
 *   1. **RAII**：上下文 / 模块 / builder / 目标机都要在异常路径上被释放（壳里不许泄漏，
 *      见接口 §5「异常不得穿出边界」——那里的 catch-all 会跨过这些析构）；
 *   2. **落码管线**：`default<O2>` + `EmitToMemoryBuffer(ObjectFile)` 这一条固定链路，
 *      参数（三元组 / PIC / Small）是接口 §7 冻结的，别在调用点各写一份。
 *
 * 链接方式照抄 `CMakeLists.txt` 的 `llvm_prog` 目标（导入目标 `LLVMC::LLVMC`）：`-I simplexc/.deps/llvm/include`
 * 加 `-L simplexc/.deps/llvm/lib -lLLVM-C`。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <map>
#include <utility>

namespace simplexc {

/* 接口 §7：三元组与 DataLayout 与 `docs/facts.md` 记的值逐字符一致（改一处就换目标 ABI）。 */
inline constexpr const char* kTriple = "x86_64-pc-windows-gnu";
inline constexpr const char* kDataLayout =
    "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-"
    "f80:128-n8:16:32:64-S128";

/* X86 的四个 Initialize。重复调用无害（LLVM 内部只登记一次）。 */
void init_native_target();

class Context {
public:
    Context() : ctx_(LLVMContextCreate()) {}
    ~Context() { LLVMContextDispose(ctx_); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    LLVMContextRef get() const { return ctx_; }

private:
    LLVMContextRef ctx_;
};

class Module {
public:
    Module(LLVMContextRef ctx, const char* name)
        : mod_(LLVMModuleCreateWithNameInContext(name, ctx)) {
        LLVMSetTarget(mod_, kTriple);
        LLVMSetDataLayout(mod_, kDataLayout);
    }
    ~Module() { LLVMDisposeModule(mod_); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    LLVMModuleRef get() const { return mod_; }

private:
    LLVMModuleRef mod_;
};

/* 一个 C API 返回的 char*（`LLVMPrintModuleToString` / `LLVMGetTargetFromTriple` 的 err）。 */
class Message {
public:
    ~Message() { LLVMDisposeMessage(p_); }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message() = default;
    char** out() { return &p_; }
    std::string str() const { return p_ != nullptr ? std::string(p_) : std::string(); }

private:
    char* p_ = nullptr;
};

/* 目标机：`RelocMode=PIC` 与 `CodeModel=Small` 是接口 §7 的冻结取值，不放在调用点。
 *
 * 为什么必须 PIC：产物被整体 memcpy 到编译期未知的 JIT 基址；`Static` 下 LLVM 会为
 * 「往内部全局写常量」生成 `movq $imm32,[disp32]`，产物里出现指向内部符号的 ADDR32，
 * 而装配契约 §4-2 一律拒绝（见 docs/facts.md 同名条目）。 */
class TargetMachine {
public:
    /* 失败返回 nullptr 并填 `*err`（不抛：这一层不认识项目的错误分类）。 */
    static TargetMachine* create(const char* triple, std::string* err);
    ~TargetMachine() { LLVMDisposeTargetMachine(tm_); }
    TargetMachine(const TargetMachine&) = delete;
    TargetMachine& operator=(const TargetMachine&) = delete;
    LLVMTargetMachineRef get() const { return tm_; }

    bool run_default_o2(LLVMModuleRef m, std::string* err);
    /* `LLVMTargetMachineEmitToMemoryBuffer(..., LLVMObjectFile)` → COFF 字节。 */
    bool emit_object(LLVMModuleRef m, std::vector<uint8_t>* out, std::string* err);

private:
    explicit TargetMachine(LLVMTargetMachineRef tm) : tm_(tm) {}
    LLVMTargetMachineRef tm_ = nullptr;
};

/* 调试信息：**只到「行号表」这一档**（DIBuilder + 一个 CU/DIFile + 一个基础类型 + 一个子程序
 * 类型，每条指令只喂行号）。消费者是 `hotspot` 的归因链：后端据此产出 `.debug_line`，把机器码
 * 区间映射回源码行（`docs/specs/2026-11-01-hotspot-profiler-design.md` §5）。
 *
 * 为什么这里没有 `dbg.value` / `dbg.declare`：那会把值摊成 SSA 名字、改写机器码 —— G1 判据
 * （同源开/关调试信息，`objdump -d` 的 `.text` 必须逐字节相同）就是拦这个的。所以本类只有
 * 「位置」这一种 API。
 *
 * 两个 module flag 是硬要求，缺了后端**静默丢弃**整份调试信息：
 *   · `Debug Info Version=3`（版本不匹配时后端直接不生成）；
 *   · `Dwarf Version=4` —— C API 没有形参可给，LLVM 23 默认 DWARF 5，不显式置位就会静默变 5。 */
class DebugInfo {
public:
    /* 建 DIBuilder、CU、DIFile、基础类型与子程序类型，并置上两条 module flag。
     * `file` 是源文件路径（进 `.debug_line` 的文件名）。 */
    DebugInfo(LLVMContextRef ctx, LLVMModuleRef mod, const std::string& file);
    ~DebugInfo();
    DebugInfo(const DebugInfo&) = delete;
    DebugInfo& operator=(const DebugInfo&) = delete;

    /* 给一个函数挂 DISubprogram 并把位置设到它的声明行。返回的 scope 要喂回 `set_loc`。
     * `decl_line < 1` 按 1 处理：verifier 拒绝行号 0 的子程序。 */
    LLVMMetadataRef attach_subprogram(LLVMValueRef fn, const std::string& name, int decl_line);
    /* 把 builder 的当前位置设成 `scope` 里的 `line`（按 (scope,line) 缓存 DebugLoc）。
     * `line < 1` 或 `scope == nullptr` = 清掉位置（有些指令本来不属于任何源码行）。 */
    void set_loc(LLVMBuilderRef bd, LLVMMetadataRef scope, int line);
    /* `LLVMDIBuilderFinalize`：必须在 verify / 优化 / 落码**之前**（否则子程序与作用域的收尾
     * 信息还没算出来）。幂等。 */
    void finalize();

private:
    LLVMContextRef ctx_;
    LLVMDIBuilderRef db_ = nullptr;
    LLVMMetadataRef file_ = nullptr;
    LLVMMetadataRef sub_ty_ = nullptr;
    std::map<std::pair<LLVMMetadataRef, int>, LLVMMetadataRef> locs_;
    bool finalized_ = false;
};

inline std::string error_message(LLVMErrorRef e) {
    char* s = LLVMGetErrorMessage(e);
    std::string out = s != nullptr ? std::string(s) : std::string("?");
    LLVMDisposeErrorMessage(s);
    return out;
}

/* 模块的 IR 文本（原型 `ir_text()` 用的同一个 C API）。 */
std::string print_module(LLVMModuleRef m);
/* `LLVMVerifyModule(..., LLVMReturnStatusAction)`；失败填 `*err`。 */
bool verify_module(LLVMModuleRef m, std::string* err);

}  // namespace simplexc
