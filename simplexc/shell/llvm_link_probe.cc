/* llvm_link_probe.c —— M1 spike：**LLVM-C.dll 能不能链上，以及装配器的输入长什么样**。
 *
 * 它只回答两件事，别往里加功能：
 *   ① MinGW g++（w64devkit 14.2）能链上官方 MSVC 构建的 `LLVM-C.dll` 吗？
 *      → `gendef` + `dlltool` 造 `libLLVM-C.a`，链接与运行见 docs/notes。
 *   ② 三元组 `x86_64-pc-windows-gnu` + RelocMode=Static + CodeModel=Small 下，
 *      `LLVMRunPasses(default<O2>)` + `LLVMTargetMachineEmitToMemoryBuffer(ObjectFile)`
 *      给出的 COFF 里**有哪些节、哪些符号、哪些重定位**？（这就是 `simplexc/assemble` 的输入契约）
 *
 * 模块故意做成会逼出重定位的形状（第一版太简单，全被内联/丢弃，一个重定位都没有）：
 *   - `probe_helper` 带 `noinline` → 内部调用不能被内联 → 内部符号的 REL32
 *   - `__probe_extern` 只声明、被真实调用 → **未定义符号** + REL32（装配器要造 stub）
 *   - `probe_table` 是 .data 里的常量 → RIP 相对引用 → 指向 .data 的 REL32
 *   - `probe_bigframe` 栈帧 > 4 KiB → 看会不会拉出 `__chkstk`（CRT 符号，不是 kernel32）
 *
 * 编译（g++ / objdump 从 PATH 找，需要 MinGW-w64 GCC 14+）：
 *   g++ -std=c++17 -O2 -I simplexc/.deps/llvm/include simplexc/shell/llvm_link_probe.cc \
 *       -o simplexc/.deps/llvm/probe.exe -L simplexc/.deps/llvm/lib -lLLVM-C
 * 运行：`probe.exe <输出.obj>`（PATH 里要有 simplexc/.deps/llvm/bin 才找得到 LLVM-C.dll）
 * 看结果：`objdump -h/-t/-r/-d`（PATH 上的 objdump）
 */
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cstdio>
#include <cstring>

#define TRIPLE "x86_64-pc-windows-gnu"
#define DATALAYOUT "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-" \
                   "f80:128-n8:16:32:64-S128"

static int fail(const char *what, const char *detail) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, detail ? detail : "(no detail)");
    return 1;
}

static LLVMValueRef add_fn(LLVMModuleRef m, LLVMContextRef ctx, const char *name,
                           LLVMTypeRef ret, LLVMTypeRef *params, unsigned n, int noinline_) {
    LLVMValueRef f = LLVMAddFunction(m, name, LLVMFunctionType(ret, params, n, 0));
    if (noinline_) {
        unsigned kind = LLVMGetEnumAttributeKindForName("noinline", 8);
        LLVMAddAttributeAtIndex(f, LLVMAttributeFunctionIndex,
                               LLVMCreateEnumAttribute(ctx, kind, 0));
    }
    return f;
}

static LLVMModuleRef build_module(LLVMContextRef ctx) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("probe", ctx);
    LLVMSetTarget(m, TRIPLE);
    LLVMSetDataLayout(m, DATALAYOUT);

    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(ctx);
    LLVMTypeRef i8p = LLVMPointerType(i8, 0);
    LLVMTypeRef voidty = LLVMVoidTypeInContext(ctx);
    LLVMBuilderRef bd = LLVMCreateBuilderInContext(ctx);

    /* ---- .data 里的常量表：被 lea 引用 → 指向 .data 的 RIP 相对重定位 ---- */
    LLVMTypeRef tbl_ty = LLVMArrayType(i64, 4);
    LLVMValueRef tbl = LLVMAddGlobal(m, tbl_ty, "probe_table");
    LLVMSetGlobalConstant(tbl, 1);
    LLVMSetLinkage(tbl, LLVMInternalLinkage);
    LLVMSetInitializer(tbl, LLVMConstArray(i64, (LLVMValueRef[]){
        LLVMConstInt(i64, 11, 0), LLVMConstInt(i64, 22, 0),
        LLVMConstInt(i64, 33, 0), LLVMConstInt(i64, 44, 0)}, 4));

    /* ---- 内部函数（noinline）→ 内部 REL32 调用 ---- */
    LLVMTypeRef h_params[1] = {i64};
    LLVMValueRef helper = add_fn(m, ctx, "probe_helper", i64, h_params, 1, 1);
    LLVMPositionBuilderAtEnd(bd, LLVMAppendBasicBlockInContext(ctx, helper, "entry"));
    LLVMValueRef hx = LLVMGetParam(helper, 0);
    LLVMBuildRet(bd, LLVMBuildMul(bd, hx, LLVMConstInt(i64, 3, 0), "m"));

    /* ---- extern：只声明、被真实调用 → 未定义符号 ---- */
    LLVMTypeRef e_params[3] = {i8p, i64, i64};
    LLVMValueRef ext = add_fn(m, ctx, "__probe_extern", i64, e_params, 3, 0);

    /* ---- 大栈帧：必须真用到那块缓存，否则整个函数会被 DCE 成一堆 nop ---- */
    LLVMTypeRef bg_params[1] = {i64};
    LLVMValueRef big = add_fn(m, ctx, "probe_bigframe", i64, bg_params, 1, 1);
    LLVMPositionBuilderAtEnd(bd, LLVMAppendBasicBlockInContext(ctx, big, "entry"));
    LLVMValueRef buf = LLVMBuildArrayAlloca(bd, i8, LLVMConstInt(LLVMInt32TypeInContext(ctx), 8192, 0), "buf");
    LLVMBuildStore(bd, LLVMConstInt(i8, 7, 0), buf);
    LLVMValueRef bg_args[3] = {buf, LLVMGetParam(big, 0), LLVMConstInt(i64, 8192, 0)};
    LLVMValueRef bg_r = LLVMBuildCall2(bd, LLVMGlobalGetValueType(ext), ext, bg_args, 3, "bg");
    LLVMBuildRet(bd, bg_r);

    /* ---- main_like：内部调用 + extern 调用 + 引用常量表 + 读写内存 ---- */
    LLVMTypeRef ml_params[1] = {i8p};
    LLVMValueRef ml = add_fn(m, ctx, "main_like", i64, ml_params, 1, 0);
    LLVMPositionBuilderAtEnd(bd, LLVMAppendBasicBlockInContext(ctx, ml, "entry"));
    LLVMValueRef base = LLVMGetParam(ml, 0);
    LLVMValueRef p64 = LLVMBuildBitCast(bd, base, LLVMPointerType(i64, 0), "p64");
    LLVMValueRef args1[1] = {LLVMBuildLoad2(bd, i64, p64, "in")};
    LLVMValueRef v = LLVMBuildCall2(bd, LLVMGlobalGetValueType(helper), helper, args1, 1, "v");
    LLVMValueRef idx[2] = {LLVMConstInt(i64, 0, 0), LLVMConstInt(i64, 2, 0)};
    LLVMValueRef slot = LLVMBuildGEP2(bd, tbl_ty, tbl, idx, 2, "slot");
    LLVMValueRef tv = LLVMBuildLoad2(bd, i64, slot, "tv");
    LLVMBuildStore(bd, LLVMBuildAdd(bd, tv, v, "nt"), slot);   /* 写回去，保证 .bss 真的被引用 */
    LLVMValueRef args2[3] = {base, v, tv};
    LLVMValueRef ev = LLVMBuildCall2(bd, LLVMGlobalGetValueType(ext), ext, args2, 3, "ev");
    LLVMBuildStore(bd, ev, p64);
    LLVMBuildRet(bd, LLVMBuildAdd(bd, ev, v, "r"));

    LLVMDisposeBuilder(bd);
    (void)voidty;
    return m;
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "probe_obj.obj";
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef m = build_module(ctx);

    char *dump = LLVMPrintModuleToString(m);
    printf("ir      : %zu bytes\n", strlen(dump));
    LLVMDisposeMessage(dump);

    LLVMTargetRef tgt = NULL;
    char *err = NULL;
    if (LLVMGetTargetFromTriple(TRIPLE, &tgt, &err))
        return fail("LLVMGetTargetFromTriple", err);
    printf("target  : %s / %s\n", LLVMGetTargetName(tgt), LLVMGetTargetDescription(tgt));

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        tgt, TRIPLE, "generic", "",
        LLVMCodeGenLevelDefault, LLVMRelocStatic, LLVMCodeModelSmall);
    if (!tm)
        return fail("LLVMCreateTargetMachine", "返回 NULL");

    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef perr = LLVMRunPasses(m, "default<O2>", tm, opts);
    if (perr) {
        char *s = LLVMGetErrorMessage(perr);
        int rc = fail("LLVMRunPasses(default<O2>)", s);
        LLVMDisposeErrorMessage(s);
        return rc;
    }
    printf("passes  : default<O2> ok\n");

    LLVMMemoryBufferRef buf = NULL;
    err = NULL;
    if (LLVMTargetMachineEmitToMemoryBuffer(tm, m, LLVMObjectFile, &err, &buf))
        return fail("EmitToMemoryBuffer(ObjectFile)", err);

    const char *p = LLVMGetBufferStart(buf);
    size_t n = LLVMGetBufferSize(buf);
    /* COFF 头的两个字段：Machine(2B) / NumberOfSections(2B)。别拿 PE 的 0x014C(4C 01) 去对。 */
    unsigned machine = (unsigned char)p[0] | ((unsigned)(unsigned char)p[1] << 8);
    unsigned nsec = (unsigned char)p[2] | ((unsigned)(unsigned char)p[3] << 8);
    printf("object  : %zu bytes, machine=0x%04X, sections=%u\n", n, machine, nsec);
    printf("bytes   : ");
    for (size_t i = 0; i < 12 && i < n; i++) printf("%02X ", (unsigned char)p[i]);
    printf("\n");

    int ok = (machine == 0x8664 && nsec > 0);
    {
        FILE *f = fopen(out_path, "wb");
        if (f) {
            fwrite(p, 1, n, f);
            fclose(f);
            printf("written : %s\n", out_path);
        } else {
            printf("WARN    : 写不出 %s（不影响判定）\n", out_path);
        }
    }
    LLVMDisposeMemoryBuffer(buf);
    LLVMDisposePassBuilderOptions(opts);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(m);
    LLVMContextDispose(ctx);
    printf(ok ? "RESULT: OK —— LLVM-C 链上了，拿到 COFF 目标码\n"
              : "RESULT: BAD —— 目标码不是合法 COFF（machine/节数不对）\n");
    return ok ? 0 : 1;
}
