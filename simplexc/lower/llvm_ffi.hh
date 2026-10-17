/* llvm_ffi.hh —— LLVM-C 的 C++ 薄封装（原型 ir_build.py:83-218 那份 ctypes 绑定的对应物）。
 *
 * 为什么只需要「薄」：产品实现是 C++，直接 include `llvm-c/Core.h` 之类的头就够了 —— 原型要
 * ctypes 是为了在 Python 里拿 C 符号。这里剩下的只有两件 Python 侧没有的东西：
 *   1. **RAII**：上下文 / 模块 / builder / 目标机都要在异常路径上被释放（壳里不许泄漏，
 *      见接口 §5「异常不得穿出边界」——那里的 catch-all 会跨过这些析构）；
 *   2. **落码管线**：`default<O2>` + `EmitToMemoryBuffer(ObjectFile)` 这一条固定链路，
 *      参数（三元组 / PIC / Small）是接口 §7 冻结的，别在调用点各写一份。
 *
 * 链接方式照抄 `tools/build.py:step_llvm_prog`：`-I build/llvm/include` + `-L build/llvm/lib -lLLVM-C`。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

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
