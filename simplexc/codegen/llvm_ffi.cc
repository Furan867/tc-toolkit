/* llvm_ffi.cc —— LLVM-C 薄封装的实现（接口见 llvm_ffi.hh）。 */
#include "llvm_ffi.hh"

namespace simplexc {

void init_native_target() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    /* 内联汇编（`asm x86_64 { … }` 的发射路径）走 MC 的汇编器：不登记 AsmParser 时
     * 产物阶段直接报 `Inline asm not supported by this streamer because we don't have an
     * asm parser for this target`。C API 把 AsmParser 排除在「四个标准初始化」之外，
     * 所以这里必须单独调一次。 */
    LLVMInitializeX86AsmParser();
}

TargetMachine* TargetMachine::create(const char* triple, std::string* err) {
    LLVMTargetRef tgt = nullptr;
    Message msg;
    if (LLVMGetTargetFromTriple(triple, &tgt, msg.out())) {
        *err = "GetTargetFromTriple(" + std::string(triple) + "): " + msg.str();
        return nullptr;
    }
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        tgt, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelSmall);
    if (tm == nullptr) {
        *err = "CreateTargetMachine 返回 NULL";
        return nullptr;
    }
    return new TargetMachine(tm);
}

bool TargetMachine::run_default_o2(LLVMModuleRef m, std::string* err) {
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef e = LLVMRunPasses(m, "default<O2>", tm_, opts);
    LLVMDisposePassBuilderOptions(opts);
    if (e != nullptr) {
        *err = "RunPasses(default<O2>): " + error_message(e);
        return false;
    }
    return true;
}

bool TargetMachine::emit_object(LLVMModuleRef m, std::vector<uint8_t>* out, std::string* err) {
    LLVMMemoryBufferRef buf = nullptr;
    Message msg;
    if (LLVMTargetMachineEmitToMemoryBuffer(tm_, m, LLVMObjectFile, msg.out(), &buf)) {
        *err = "EmitToMemoryBuffer: " + msg.str();
        return false;
    }
    const char* p = LLVMGetBufferStart(buf);
    const size_t n = LLVMGetBufferSize(buf);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    out->assign(b, b + n);
    LLVMDisposeMemoryBuffer(buf);
    return true;
}

std::string print_module(LLVMModuleRef m) {
    char* s = LLVMPrintModuleToString(m);
    std::string out = s != nullptr ? std::string(s) : std::string();
    LLVMDisposeMessage(s);
    return out;
}

bool verify_module(LLVMModuleRef m, std::string* err) {
    char* s = nullptr;
    if (LLVMVerifyModule(m, LLVMReturnStatusAction, &s)) {
        *err = s != nullptr ? std::string(s) : std::string("IR 校验失败");
        LLVMDisposeMessage(s);
        return false;
    }
    return true;
}

}  // namespace simplexc
