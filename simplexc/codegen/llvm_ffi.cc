/* llvm_ffi.cc —— LLVM-C 薄封装的实现（接口见 llvm_ffi.hh）。 */
#include "llvm_ffi.hh"

namespace simplexc {

namespace {

/* DWARF 的类型编码（`DW_ATE_*`）。C API 这个形参是裸 `unsigned`，头里没有 C 常量可用。 */
constexpr unsigned kDwAteAddress = 0x01;

}  // namespace

DebugInfo::DebugInfo(LLVMContextRef ctx, LLVMModuleRef mod, const std::string& file) : ctx_(ctx) {
    db_ = LLVMCreateDIBuilder(mod);
    file_ = LLVMDIBuilderCreateFile(db_, file.c_str(), file.size(), nullptr, 0);
    /* CU 不需要持有句柄：`DIBuilder` 建它时已经挂进模块的 `llvm.dbg.cu` 具名元数据。 */
    (void)LLVMDIBuilderCreateCompileUnit(
        db_, LLVMDWARFSourceLanguageC, file_, "simplexc", 8, /*isOptimized=*/1, nullptr, 0,
        /*RuntimeVer=*/0, nullptr, 0, LLVMDWARFEmissionFull, /*DWOId=*/0,
        /*SplitDebugInlining=*/1, /*DebugInfoForProfiling=*/0, nullptr, 0, nullptr, 0);
    /* 子程序类型 = 无参 → 无返回值：`Codegen` 只喂行号，形参/返回值的语义不进调试信息（HIR 的
     * 值是 i64 位模式，写进 DWARF 只会是假的）。 */
    LLVMMetadataRef int_ty =
        LLVMDIBuilderCreateBasicType(db_, "i64", 3, 64, kDwAteAddress, LLVMDIFlagZero);
    LLVMMetadataRef param_types[] = {int_ty};
    sub_ty_ = LLVMDIBuilderCreateSubroutineType(db_, file_, param_types, 0, LLVMDIFlagZero);

    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMAddModuleFlag(mod, LLVMModuleFlagBehaviorWarning, "Debug Info Version", 18,
                      LLVMValueAsMetadata(LLVMConstInt(i32, 3, 0)));
    LLVMAddModuleFlag(mod, LLVMModuleFlagBehaviorWarning, "Dwarf Version", 13,
                      LLVMValueAsMetadata(LLVMConstInt(i32, 4, 0)));
}

DebugInfo::~DebugInfo() {
    /* 没 finalize 就析构（异常路径）也要 dispose 掉：dispose 之前没 finalize 会泄漏，而
     * DebugInfo 是「整份调试信息」的唯一主人。 */
    if (db_ != nullptr) {
        if (!finalized_) LLVMDIBuilderFinalize(db_);
        LLVMDisposeDIBuilder(db_);
    }
}

LLVMMetadataRef DebugInfo::attach_subprogram(LLVMValueRef fn, const std::string& name,
                                            int decl_line) {
    const unsigned line = decl_line >= 1 ? static_cast<unsigned>(decl_line) : 1u;
    LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
        db_, file_, name.c_str(), name.size(), name.c_str(), name.size(), file_, line, sub_ty_,
        /*IsLocalToUnit=*/1, /*IsDefinition=*/1, line, LLVMDIFlagZero, /*IsOptimized=*/1);
    LLVMSetSubprogram(fn, sp);
    return sp;
}

void DebugInfo::set_loc(LLVMBuilderRef bd, LLVMMetadataRef scope, int line) {
    if (bd == nullptr) return;
    if (scope == nullptr || line < 1) {
        LLVMSetCurrentDebugLocation2(bd, nullptr);
        return;
    }
    const auto key = std::make_pair(scope, line);
    auto it = locs_.find(key);
    if (it == locs_.end()) {
        it = locs_.emplace(key, LLVMDIBuilderCreateDebugLocation(
                                    ctx_, static_cast<unsigned>(line), /*Column=*/0, scope,
                                    /*InlinedAt=*/nullptr))
                 .first;
    }
    LLVMSetCurrentDebugLocation2(bd, it->second);
}

void DebugInfo::finalize() {
    if (db_ == nullptr || finalized_) return;
    LLVMDIBuilderFinalize(db_);
    finalized_ = true;
}

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
