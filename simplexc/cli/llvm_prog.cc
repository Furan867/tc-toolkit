/* llvm_prog.cc —— 用 LLVM-C 生成**程序根 `tc_main`**，落成 COFF，交给装配器。
 *
 * 这条链是 M1 的核心：`LLVM 模块 → -O2 → COFF → 装配器（+ runtime + shim）→ blob`。
 * 它与将来真正的前端只差「谁生成模块」这一步 —— 前端接进来以后，本文件就退化成测试夹具。
 *
 * 模块形状刻意贴近真实产物：
 *   · `tc_main` 是外部可见的程序根（装配器按符号名找它）；
 *   · 调用一个**只声明不定义**的 `tc_probe_ok(i64)` → 逼出 extern 桩 + REL32；
 *   · 写一个内部全局 → 逼出 .bss / 内部引用。
 *
 * 用法：`llvm_prog <输出.obj>`（PATH 里要有 build/llvm/bin 才能找到 LLVM-C.dll）
 */
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TRIPLE "x86_64-pc-windows-gnu"
#define DATALAYOUT "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-" \
                   "f80:128-n8:16:32:64-S128"

static int die(const char* what, const char* detail) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, detail ? detail : "(no detail)");
    return 1;
}

int main(int argc, char** argv) {
    const char* out_path = (argc > 1) ? argv[1] : "llvm_prog.obj";
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("prog", ctx);
    LLVMSetTarget(m, TRIPLE);
    LLVMSetDataLayout(m, DATALAYOUT);

    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMBuilderRef bd = LLVMCreateBuilderInContext(ctx);

    /* 只声明：装配器要给它造桩 */
    LLVMTypeRef probe_params[1] = {i64};
    LLVMValueRef probe = LLVMAddFunction(m, "tc_probe_ok", LLVMFunctionType(
        LLVMVoidTypeInContext(ctx), probe_params, 1, 0));

    /* 内部全局（.bss）：让产物里出现数据节与内部引用 */
    LLVMValueRef slot = LLVMAddGlobal(m, i64, "tc_probe_slot");
    LLVMSetLinkage(slot, LLVMInternalLinkage);
    LLVMSetInitializer(slot, LLVMConstInt(i64, 7, 0));

    LLVMValueRef main_fn = LLVMAddFunction(m, "tc_main",
        LLVMFunctionType(LLVMVoidTypeInContext(ctx), nullptr, 0, 0));
    LLVMPositionBuilderAtEnd(bd, LLVMAppendBasicBlockInContext(ctx, main_fn, "entry"));
    LLVMValueRef v = LLVMBuildLoad2(bd, i64, slot, "v");
    LLVMValueRef arg = LLVMBuildAdd(bd, v, LLVMConstInt(i64, 35, 0), "arg");
    LLVMValueRef args[1] = {arg};
    LLVMBuildCall2(bd, LLVMGlobalGetValueType(probe), probe, args, 1, "");
    LLVMBuildRetVoid(bd);

    LLVMTargetRef tgt = nullptr;
    char* err = nullptr;
    if (LLVMGetTargetFromTriple(TRIPLE, &tgt, &err)) return die("GetTargetFromTriple", err);
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        tgt, TRIPLE, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocStatic, LLVMCodeModelSmall);
    if (!tm) return die("CreateTargetMachine", "NULL");

    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef perr = LLVMRunPasses(m, "default<O2>", tm, opts);
    if (perr) {
        char* s = LLVMGetErrorMessage(perr);
        int rc = die("RunPasses(default<O2>)", s);
        LLVMDisposeErrorMessage(s);
        return rc;
    }

    LLVMMemoryBufferRef buf = nullptr;
    if (LLVMTargetMachineEmitToMemoryBuffer(tm, m, LLVMObjectFile, &err, &buf))
        return die("EmitToMemoryBuffer", err);
    const char* p = LLVMGetBufferStart(buf);
    size_t n = LLVMGetBufferSize(buf);
    FILE* f = std::fopen(out_path, "wb");
    if (!f) return die("fopen", out_path);
    std::fwrite(p, 1, n, f);
    std::fclose(f);
    std::printf("COFF %zu 字节 → %s\n", n, out_path);

    LLVMDisposeMemoryBuffer(buf);
    LLVMDisposePassBuilderOptions(opts);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(m);
    LLVMContextDispose(ctx);
    return 0;
}
