#!python3
"""M3.0 lowering v0：AST → LLVM IR → COFF → `tcblob` 装配（一条命令）。

范围：**只求正确，不求优化**。语义按 `docs/facts.md` 的语义档案逐条落（下面注释指到出处），
没定 / 没做的构造一律 `UNSUPPORTED: <构造> @ 第 N 行` 报错 —— 不猜。

一条命令做的事
--------------
    1. 前端两刀（`frontend/decls.py` 声明表 + `frontend/expr.py` 语句/表达式 AST）；
    2. 顶层声明 → 模块骨架：枚举成员常量、struct 类型、全局变量、字符串字面量；
    3. 程序根（`run_sim:` 顶层块）→ `tc_main`；其余 def **按需**生成（只生成真被调用的）；
    4. `LLVMRunPasses(default<O2>)` → `LLVMTargetMachineEmitToMemoryBuffer(ObjectFile)` → COFF；
    5. `build/tcblob.exe` 装配（+ `entry_shim.o` + `rt_min.o`）→ `<out>` / `.entry` / `.stubs`；
    6. `--check`：宿主各跑一遍参考产物与我们的产物，逐 tick 比 pages / 状态字段 / extern。

值表示（为什么这样定）
----------------------
整数 / 指针 / 枚举的 SSA 值一律 **i64**，内容是该类型范围内的**位模式**（`0 <= v < 2^bits`，
即 `zext(trunc_W(v))` 形态）。这是 U-L7「运行期窄值零扩展」的直接落地，也是
`dossier/ul7_promote_truncate.md` 指示 2「用 i64 运算 + 掩码表达宽度，别用 LLVM 原生 iN」。
宽度靠显式掩码表达，掩码点两处：**转换**与**运算结果**（U-L7 指示 1）。

加宽分两路（都是参考实现的行为，不是我们的选择；U-L7 指示 3 + `ul7_narrow_var.md`）：
**编译期常量**按源类型符号性 `sext`/`zext`，**运行期窄值**一律 `zext(trunc_W)`。
`Program.pattern_of()` 就是这条规则，所有转换 / 移位 / 有符号比较都过它。所以
`U64 (S32 -1)` = 全 FF，而运行期的 S32 −8 加宽成 U64 是 `0x00000000FFFFFFF8`。

内存模型（U-L1）
----------------
地址是**字节地址**，算术就是 `add i64`（不乘元素宽度）；`load`/`store` 对齐**一律 1**、
**不许**插对齐检查（参考实现在非对齐时静默成功）。写宽度 = 值类型的字节宽度
（`ceil(bits/8)`；U8/U16/U32/U64 已实测，U1 取 1 字节 —— 见笔记的「未定」一节）。

用法
----
    python simplexc/lower/ir_build.py --src simplexc/corpus/src/006.si \\
        --out work/tmp/lower0/006.bin --check --ticks 10
    python simplexc/lower/ir_build.py --src simplexc/corpus/rv32im/rv32im.si --list-unsupported
    python simplexc/lower/ir_build.py --src <si> --out <bin> --ir-out <ll>   # 顺手 dump IR

退出码：0 = 产物已出（带 `--check` 时还要求逐 tick 全等）；1 = 有 unsupported / 内部错误；
2 = 参数或环境问题；3 = `--list-unsupported` 有清单（不算失败）。
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes as ct
import importlib.util
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, fields as dc_fields
from functools import partial

for _stream in (sys.stdout, sys.stderr):
    _rec = getattr(_stream, "reconfigure", None)
    if _rec is not None:
        with contextlib.suppress(ValueError, OSError):
            _rec(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))            # simplexc/lower
SIMPLEXC = os.path.dirname(HERE)                            # simplexc
REPO = os.path.dirname(SIMPLEXC)                            # reversing/
BUILD = os.path.join(REPO, "build")
LLVM_DLL = os.path.join(BUILD, "llvm", "bin", "LLVM-C.dll")
TCBLOB = os.path.join(BUILD, "tcblob.exe")
ENTRY_SHIM = os.path.join(BUILD, "objs", "entry_shim.o")
RT_MIN = os.path.join(BUILD, "objs", "rt_min.o")
HOST_PY = os.path.join(SIMPLEXC, "tools", "host", "host.py")
PROBE_PY = os.path.join(SIMPLEXC, "tools", "probe_006.py")   # 只借它的 compare_ticks

TRIPLE = "x86_64-pc-windows-gnu"
DATALAYOUT = ("e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-"
              "f80:128-n8:16:32:64-S128")
M64 = (1 << 64) - 1
FILE_TIME_UNIX = 116444736000000000          # 1970-01-01 的 FILETIME（100ns 单位）
NS_PER_MS = 1000000


def load_module(path, name):
    """按路径加载一个模块（`simplexc/` 不是包，前端两刀只能这样引）。"""
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"加载不了 {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod            # dataclass 要能从 sys.modules 找到自己的模块
    spec.loader.exec_module(mod)
    return mod


# 前端第二刀 `frontend/expr.py`（它自己会把 `frontend/` 放进 sys.path 再 import decls）
fe_expr = load_module(os.path.join(SIMPLEXC, "frontend", "expr.py"), "si_expr")


# ---------------------------------------------------------------- LLVM-C 绑定
#
# 只绑用到的那几十个符号。句柄一律 `c_void_p`（LLVM-C 的不透明指针），数组参数用
# `POINTER(c_void_p)` —— 与 `LLVMOpaqueValue **` 的 ABI 一致。

def _bind(lib, name, res, *args):
    """取一个 LLVM-C 符号，绑上 ctypes 签名，返回可调用的东西。

    放模块级而不是 `LLVM.__init__` 里：嵌套函数里的 `return` 会被 lint 当成
    构造函数在返回值。
    """
    fn = getattr(lib, name)
    fn.restype = res
    fn.argtypes = list(args)
    if not any(a is ct.c_char_p for a in args):
        return fn

    def call(*a):
        # 名字参数（`const char*`）容忍 Python str：统一在这里转 bytes
        return fn(*[x.encode('utf-8') if isinstance(x, str) else x for x in a])

    return call


class LLVM:
    def __init__(self, dll=LLVM_DLL):
        if not os.path.isfile(dll):
            raise SystemExit(f"找不到 {dll}（LLVM 没解包？版本锁定见 docs/facts.md）")
        lib = ct.CDLL(dll)
        p, u, i, b, s = (ct.c_void_p, ct.c_uint, ct.c_int, ct.c_int, ct.c_char_p)
        arr = ct.POINTER(ct.c_void_p)
        f = partial(_bind, lib)          # `f(name, res, *argtypes)`，见模块级 `_bind`

        self.ContextCreate = f("LLVMContextCreate", p)
        self.ContextDispose = f("LLVMContextDispose", None, p)
        self.ModuleCreate = f("LLVMModuleCreateWithNameInContext", p, s, p)
        self.SetTarget = f("LLVMSetTarget", None, p, s)
        self.SetDataLayout = f("LLVMSetDataLayout", None, p, s)
        self.IntType = f("LLVMIntTypeInContext", p, p, u)
        self.Int64 = f("LLVMInt64TypeInContext", p, p)
        self.VoidType = f("LLVMVoidTypeInContext", p, p)
        self.PointerType = f("LLVMPointerType", p, p, u)
        self.StructType = f("LLVMStructTypeInContext", p, p, arr, u, b)
        self.ArrayType = f("LLVMArrayType2", p, p, ct.c_uint64)
        self.FunctionType = f("LLVMFunctionType", p, p, arr, u, b)
        self.AddFunction = f("LLVMAddFunction", p, p, s, p)
        self.AddGlobal = f("LLVMAddGlobal", p, p, p, s)
        self.SetInitializer = f("LLVMSetInitializer", None, p, p)
        self.SetLinkage = f("LLVMSetLinkage", None, p, i)
        self.SetGlobalConstant = f("LLVMSetGlobalConstant", None, p, b)
        self.ConstInt = f("LLVMConstInt", p, p, ct.c_ulonglong, b)
        self.ConstNull = f("LLVMConstNull", p, p)
        self.ConstArray = f("LLVMConstArray2", p, p, arr, ct.c_uint64)
        self.ConstGEP2 = f("LLVMConstGEP2", p, p, p, arr, u)
        self.ConstIntToPtr = f("LLVMConstIntToPtr", p, p, p)
        self.BuildGEP2 = f("LLVMBuildGEP2", p, p, p, p, arr, u, s)
        self.AppendBlock = f("LLVMAppendBasicBlockInContext", p, p, p, s)
        self.CreateBuilder = f("LLVMCreateBuilderInContext", p, p)
        self.DisposeBuilder = f("LLVMDisposeBuilder", None, p)
        self.PositionAtEnd = f("LLVMPositionBuilderAtEnd", None, p, p)
        self.PositionBefore = f("LLVMPositionBuilderBefore", None, p, p)
        self.BuildAlloca = f("LLVMBuildAlloca", p, p, p, s)
        self.SetAlignment = f("LLVMSetAlignment", None, p, u)
        self.SetVolatile = f("LLVMSetVolatile", None, p, b)
        self.BuildLoad2 = f("LLVMBuildLoad2", p, p, p, p, s)
        self.BuildStore = f("LLVMBuildStore", p, p, p, p)
        self.BuildIntToPtr = f("LLVMBuildIntToPtr", p, p, p, p, s)
        self.BuildPtrToInt = f("LLVMBuildPtrToInt", p, p, p, p, s)
        for name in ("Add", "Sub", "Mul", "And", "Or", "Xor", "Shl", "LShr", "AShr",
                     "UDiv", "SDiv", "URem", "SRem"):
            setattr(self, "Build" + name, f("LLVMBuild" + name, p, p, p, p, s))
        self.BuildICmp = f("LLVMBuildICmp", p, p, u, p, p, s)
        self.BuildSelect = f("LLVMBuildSelect", p, p, p, p, p, s)
        self.BuildZExt = f("LLVMBuildZExt", p, p, p, p, s)
        self.BuildTrunc = f("LLVMBuildTrunc", p, p, p, p, s)
        self.BuildBr = f("LLVMBuildBr", p, p, p)
        self.BuildCondBr = f("LLVMBuildCondBr", p, p, p, p, p)
        self.BuildRet = f("LLVMBuildRet", p, p, p)
        self.BuildRetVoid = f("LLVMBuildRetVoid", p, p)
        self.BuildCall2 = f("LLVMBuildCall2", p, p, p, p, arr, u, s)
        self.BuildPhi = f("LLVMBuildPhi", p, p, p, s)
        self.AddIncoming = f("LLVMAddIncoming", None, p, arr, arr, u)
        self.GetParam = f("LLVMGetParam", p, p, u)
        self.AddAttributeAtIndex = f("LLVMAddAttributeAtIndex", None, p, u, p)
        self.CreateEnumAttribute = f("LLVMCreateEnumAttribute", p, p, u, ct.c_uint64)
        self.GetEnumAttributeKindForName = f("LLVMGetEnumAttributeKindForName", u, s,
                                             ct.c_size_t)
        self.GlobalGetValueType = f("LLVMGlobalGetValueType", p, p)
        self.GetBlockTerminator = f("LLVMGetBasicBlockTerminator", p, p)
        self.GetFirstInstruction = f("LLVMGetFirstInstruction", p, p)
        self.PrintModule = f("LLVMPrintModuleToString", ct.c_void_p, p)
        self.DisposeMessage = f("LLVMDisposeMessage", None, ct.c_void_p)
        self.InitializeX86TargetInfo = f("LLVMInitializeX86TargetInfo", None)
        self.InitializeX86Target = f("LLVMInitializeX86Target", None)
        self.InitializeX86TargetMC = f("LLVMInitializeX86TargetMC", None)
        self.InitializeX86AsmPrinter = f("LLVMInitializeX86AsmPrinter", None)
        self.GetTargetFromTriple = f("LLVMGetTargetFromTriple", b, s, ct.POINTER(p),
                                     ct.POINTER(s))
        self.CreateTargetMachine = f("LLVMCreateTargetMachine", p, p, s, s, s, i, i, i)
        self.DisposeTargetMachine = f("LLVMDisposeTargetMachine", None, p)
        self.CreatePassBuilderOptions = f("LLVMCreatePassBuilderOptions", p)
        self.DisposePassBuilderOptions = f("LLVMDisposePassBuilderOptions", None, p)
        self.RunPasses = f("LLVMRunPasses", p, p, s, p, p)
        self.EmitToMemoryBuffer = f("LLVMTargetMachineEmitToMemoryBuffer", b, p, p, i,
                                    ct.POINTER(s), ct.POINTER(p))
        self.GetBufferStart = f("LLVMGetBufferStart", ct.POINTER(ct.c_char), p)
        self.GetBufferSize = f("LLVMGetBufferSize", ct.c_size_t, p)
        self.DisposeMemoryBuffer = f("LLVMDisposeMemoryBuffer", None, p)
        self.VerifyModule = f("LLVMVerifyModule", b, p, i, ct.POINTER(s))
        # LLVM-C 的枚举值（抄自 build/llvm/include/llvm-c/）
        self.InternalLinkage = 8
        self.PrivateLinkage = 9
        self.IEQ, self.INE = 32, 33
        self.IUGT, self.IUGE, self.IULT, self.IULE = 34, 35, 36, 37
        self.ISGT, self.ISGE, self.ISLT, self.ISLE = 38, 39, 40, 41
        self.CodeGenLevelDefault = 0
        self.RelocStatic, self.RelocPIC = 1, 2
        self.CodeModelSmall = 0
        self.ObjectFile = 1


def carr(vals):
    """LLVMValueRef / LLVMTypeRef 数组 → `T**` 实参。"""
    return (ct.c_void_p * max(1, len(vals)))(*vals)


# ---------------------------------------------------------------- 类型

@dataclass(frozen=True)
class Ty:
    """lowering 自己的类型（不是冻结接口 §1 的 `Type`，那个属于 C++ 前端）。

    只留「生成 IR 需要的那几件事」：宽度、符号性、struct 字段、数组元素。
    """
    kind: str                 # void int ptr enum struct array str
    bits: int = 64
    signed: bool = False
    name: str = ''
    fields: tuple = ()
    elem: object = None


VOID = Ty('void')
I64 = Ty('int', 64, True)
BOOL = Ty('int', 1, False)
PTR = Ty('ptr', 64, False)
STR = Ty('str', 64, False, elem=Ty('int', 8, False))     # `Str = Arr[Char]`
SEED = Ty('int', 64, False)
TIME = Ty('int', 64, False)      # `Time` 的单位是纳秒（facts.md §006 的语义）


def sized(bits, signed):
    return Ty('int', bits, signed)


def mask_of(bits):
    return M64 if bits >= 64 else (1 << bits) - 1


def is_int(ty):
    return ty.kind == 'int'


def storage_bytes(ty):
    """内存里的字节宽度（U-L7 指示 4：写宽度 = 值类型宽度）。

    `ceil(bits/8)`：U8/U16/U32/U64 已实测；U1 取 1 字节（见笔记「未定」）。
    """
    if ty.kind in ('ptr', 'str', 'array', 'enum'):
        return 8
    if ty.kind == 'int':
        return (ty.bits + 7) // 8
    raise Unsupported(f"类型 {ty.kind} 没有字节宽度", 0)


@dataclass
class V:
    """表达式的 lowering 结果：i64（或 struct 聚合）值 + 类型 + 可选的编译期常量。"""
    ll: object
    ty: Ty
    const: int | None = None


class Unsupported(Exception):
    """没定 / 没做的构造。带源码行号 —— 冻结接口 §8 要求错误能定位到 span。"""

    def __init__(self, msg, line):
        super().__init__(msg)
        self.msg = msg
        self.line = line


class CompileFail(Exception):
    pass


# ---------------------------------------------------------------- 常量与内建表

ENUM_RE = re.compile(r'Enum\s*\[(.*)\]', re.S)
FIELD_RE = re.compile(r'([A-Za-z_]\w*)\s*:\s*(@?[A-Za-z_]\w*)')
SIZED_RE = re.compile(r'([US])(\d+)')

# 前缀（stdlib）里定义、生成源码当作「已存在」用的名字。只登记 006 用到的那几个；
# 其余一律 UNSUPPORTED。**这不是内建表** —— 内建表的唯一来源是接口 §2 的 builtins.def。
PREFIX_GLOBALS = {'global_seed': SEED}
PREFIX_FUNCS = {'get_time', 'sleep', 'memory_clear', 'memory_copy', 'thread_exit'}
# 源码里直接用、文件里没声明的编译期常量（前缀里的）。`// -1` 是源码自带的注释。
HASH_CONSTS = {'#U64_HIGH': (M64, Ty('int', 64, False))}
CMP_OPS = {'==': 'EQ', '!=': 'NE', '<': 'LT', '<=': 'LE', '>': 'GT', '>=': 'GE'}
ARITH_OPS = {'+', '-', '*', '/', '%', '&', '|', '^', '<<', '>>'}
ASSIGN_BINOP = {'+=': '+', '-=': '-', '*=': '*', '/=': '/', '%=': '%',
                '&=': '&', '|=': '|', '^=': '^', '<<=': '<<', '>>=': '>>'}


def as_signed(x, bits):
    """把 bits 位位模式解释成有符号整数。"""
    x &= mask_of(bits)
    if bits < 64 and (x >> (bits - 1)) & 1:
        x -= 1 << bits
    return x


def fold_arith(op, a, b, ty):
    """编译期折叠，结果是**该类型的位模式**（`0 <= v < 2^bits`）。

    两条参考实现的行为照抄：截断发生在运算结果上（U-L7）、除法的符号性看被除数
    （facts.md §`/`、`%`）、除数为 0 时商 0 余数为被除数（U-L8 Q3）。
    """
    bits = ty.bits if ty.kind == 'int' else 64
    a &= mask_of(bits)
    b &= mask_of(bits)
    if op in ('+', '-', '*'):
        r = {'+': a + b, '-': a - b, '*': a * b}[op]
        return r & mask_of(bits)
    if op in ('&', '|', '^'):
        return {'&': a & b, '|': a | b, '^': a ^ b}[op] & mask_of(bits)
    if op == '<<':
        return (a << (b & (bits - 1))) & mask_of(bits)
    if op == '>>':
        # U-L8 指示 1：trunc_W(lshr(ext64(a), n & (W-1)))。常量按符号性 sext。
        ext = (as_signed(a, bits) & M64) if ty.signed else a
        return (ext >> (b & (bits - 1))) & mask_of(bits)
    if op in ('/', '%'):
        if b == 0:
            return 0 if op == '/' else a
        if ty.signed:
            sa, sb = as_signed(a, bits), as_signed(b, bits)
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q
            r = sa - q * sb
        else:
            q, r = a // b, a % b
        return (q if op == '/' else r) & mask_of(bits)
    raise Unsupported(f"运算符 {op}", 0)


def fold_unary(op, a, ty):
    bits = ty.bits if ty.kind == 'int' else 64
    a &= mask_of(bits)
    if op == '-':
        return (-a) & mask_of(bits)
    if op == '~':
        return (~a) & mask_of(bits)
    if op == '+':
        return a
    raise Unsupported(f"一元运算符 {op}", 0)


# ---------------------------------------------------------------- 函数级 lowering
#
# 一个函数的状态（builder / 作用域 / break 目标）在「按需生成」时保存-切换-恢复，
# 所以边发一个函数边发它调用的函数是安全的。局部变量一律 alloca（由 mem2reg 提升），
# 先求正确，不手写 phi/分配。
#
# `Scope` 里的绑定（元组的第一个元素）：
#   ('local', alloca, ty)    局部变量 —— 地址就是那块 alloca
#   ('capture', ptr, ty)     闭包捕获 —— 外层 alloca 的指针（**按引用捕获**）
#   ('const', V)             `let`/`const` 且初始化式是编译期常量
#   ('def', DefStmt, caps)   嵌套 def —— 只登记，被调用时才生成

class Lower:
    # ---- 状态：保存 / 恢复
    def _save_state(self):
        return {k: getattr(self, k, None) for k in
                ('bd', 'fnref', 'ret_ty', 'cur_block', 'entry', 'scopes', 'breaks',
                 'cur_name', 'cur_def')}

    def _restore_state(self, s):
        for k, v in s.items():
            setattr(self, k, v)

    def _mark_no_builtin(self, fnref):
        kind = self.L.GetEnumAttributeKindForName("nobuiltin", 9)
        if kind:
            self.L.AddAttributeAtIndex(fnref, -1, self.L.CreateEnumAttribute(
                self.ctx, kind, 0))

    def begin_function(self, fnref, ret_ty):
        L = self.L
        self._mark_no_builtin(fnref)
        self.bd = L.CreateBuilder(self.ctx)
        self.fnref = fnref
        self.ret_ty = ret_ty
        self.entry = L.AppendBlock(self.ctx, fnref, b"entry")
        L.PositionAtEnd(self.bd, self.entry)
        self.cur_block = self.entry
        self.scopes = [{}]
        self.breaks = []

    def finish_function(self):
        L = self.L
        if not L.GetBlockTerminator(self.cur_block):
            if self.ret_ty.kind == 'void':
                L.BuildRetVoid(self.bd)
            else:
                self.warnings.append(f"{self.cur_name}：控制流落到函数末尾，补 ret 零值")
                L.BuildRet(self.bd, L.ConstNull(self.val_ll(self.ret_ty)))
        L.DisposeBuilder(self.bd)
        self.bd = None

    def start_block(self, name="b"):
        """新开一个**只由 condbr/branch 进入**的块（本函数不替调用者发 br）。"""
        bb = self.L.AppendBlock(self.ctx, self.fnref, name.encode())
        self.L.PositionAtEnd(self.bd, bb)
        self.cur_block = bb
        return bb

    def br_new_block(self, name):
        """从当前块**无条件跳**进一个新块。"""
        bb = self.L.AppendBlock(self.ctx, self.fnref, name.encode())
        self.L.BuildBr(self.bd, bb)
        self.L.PositionAtEnd(self.bd, bb)
        self.cur_block = bb
        return bb

    def alloca(self, ty, name):
        """局部变量一律 alloca，插在 **entry 块最前面**（循环里声明也不会每次迭代吃栈）。"""
        L = self.L
        first = L.GetFirstInstruction(self.entry)
        if first is None:
            L.PositionAtEnd(self.bd, self.entry)
        else:
            L.PositionBefore(self.bd, first)
        a = L.BuildAlloca(self.bd, self.ll_type(ty), name.encode())
        L.SetAlignment(a, 1)
        L.PositionAtEnd(self.bd, self.cur_block)
        return a

    # ---- 作用域
    def bind(self, name, entry):
        self.scopes[-1][name] = entry

    def bind_root_scope(self, name, entry):
        self.scopes[0][name] = entry

    def lookup(self, name):
        for sc in reversed(self.scopes):
            if name in sc:
                return sc[name]
        return None

    def push_scope(self):
        self.scopes.append({})

    def pop_scope(self):
        self.scopes.pop()

    # ---- 指令小工具
    def llv(self, v):
        if v.ll is None:
            if v.const is None:
                raise Unsupported("（内部）常量与指令都为空", 0)
            v.ll = (self.const_ptr(v.const) if v.ty.kind in ('str', 'array')
                    else self.const_int(v.const))
        return v.ll

    def pat_const(self, v):
        """常量形态下按源类型符号性扩展后的 64 位内容；非常量返回 None。"""
        if v.const is None:
            return None
        if not is_int(v.ty):
            return v.const & M64
        n = v.const & mask_of(v.ty.bits)
        if v.ty.signed and v.ty.bits < 64 and (n >> (v.ty.bits - 1)) & 1:
            n |= ~mask_of(v.ty.bits) & M64
        return n & M64

    def pattern_of(self, v):
        """`ext64`：常量按符号性 sext/zext，运行期恒等（本来就是 zext 形态）。"""
        c = self.pat_const(v)
        if c is not None:
            return self.const_int(c)
        return self.llv(v)

    def mask_ll(self, ll, bits):
        if bits >= 64:
            return ll
        return self.L.BuildAnd(self.bd, ll, self.const_int(mask_of(bits)), "m")

    def convert(self, v, target, line):
        """显式转换 / 赋值配合（U-L7 指示 1+3）。"""
        if target.kind == 'void':
            raise Unsupported("转换成 None", line)
        if v.ty.kind == 'struct' or target.kind == 'struct':
            if v.ty.kind == 'struct' and target.kind == 'struct' and v.ty.name == target.name:
                return v
            raise Unsupported(f"转换 {v.ty.kind} → {target.kind}", line)
        c = self.pat_const(v)
        if target.kind == 'int':
            if v.ty.kind in ('str', 'array'):           # 真指针 → 整数
                return V(self.L.BuildPtrToInt(self.bd, self.llv(v), self.i64, "pi"),
                         target, None)
            if c is not None:
                return V(None, target, c & mask_of(target.bits))
            return V(self.mask_ll(self.llv(v), target.bits), target, None)
        if target.kind in ('str', 'array'):            # 整数地址 → 真指针
            if v.ty.kind in ('str', 'array'):
                return V(self.llv(v), target, None)
            if c is not None:
                return V(None, target, c & M64)
            return V(self.to_ptr(v), target, None)
        # ptr / enum：i64 之间位重解释，只换类型标签
        if c is not None:
            return V(None, target, c & M64)
        return V(self.llv(v), target, None)

    # ---- 内存
    def to_ptr(self, v):
        """地址值 → LLVM `ptr`。`Ptr`（i64 游戏地址）走 inttoptr；`Str`/`Arr`/槽本来就是
        ptr，直接用（这样全局/局部地址只会出 `lea rip+…`，不出绝对重定位）。"""
        if v.ty.kind in ('str', 'array'):
            return self.llv(v)
        return self.L.BuildIntToPtr(self.bd, self.llv(v), self.ptr, "p")

    def load_addr(self, p, ty):
        L = self.L
        st = self.ll_type(ty)
        inst = L.BuildLoad2(self.bd, st, p, "ld")
        L.SetAlignment(inst, 1)
        if not is_int(ty):
            return V(inst, ty)
        v = inst
        if st != self.i64:
            v = L.BuildZExt(self.bd, v, self.i64, "z")
        if ty.bits < 64:
            v = self.mask_ll(v, ty.bits)
        return V(v, ty)

    def store_addr(self, p, v, ty, line=0):
        L = self.L
        if not is_int(ty):
            inst = L.BuildStore(self.bd, self.llv(v), p)
            L.SetAlignment(inst, 1)
            return
        cv = self.convert(v, ty, line)
        st = self.storage_ll(ty)
        ll = self.llv(cv)
        if st != self.i64:
            ll = L.BuildTrunc(self.bd, ll, st, "t")
        inst = L.BuildStore(self.bd, ll, p)
        L.SetAlignment(inst, 1)

    def lvalue(self, node):
        """左值 → (i64 字节地址, 类型)。地址是字节地址（U-L1）。"""
        L = self.L
        T = type(node).__name__
        if T == 'IdentExpr':
            b = self.lookup(node.name)
            if b is None:
                if node.name in self.globals:
                    return self.globals[node.name]
                if node.name in PREFIX_GLOBALS:
                    return self.prefix_global(node.name, node.src_line)
                raise Unsupported(f"未定义的名字 {node.name}", node.src_line)
            if b[0] in ('local', 'capture'):
                return b[1], b[2]
            if b[0] == 'const':
                raise Unsupported(f"给常量 {node.name} 赋值", node.src_line)
            raise Unsupported(f"{node.name} 不是左值", node.src_line)
        if T == 'IndexExpr':
            base = self.e(node.obj)
            if base.ty.kind not in ('array', 'str'):
                raise Unsupported(f"下标用在 {base.ty.kind} 上", node.src_line)
            idx = self.convert(self.e(node.index), I64, node.src_line)
            step = storage_bytes(base.ty.elem)
            off = L.BuildMul(self.bd, self.llv(idx), self.const_int(step), "io")
            p = L.BuildGEP2(self.bd, self.storage_ll(Ty('int', 8, False)),
                            self.to_ptr(base), carr([off]), 1, "ea")
            return p, base.ty.elem
        if T == 'MemberExpr':
            base_ptr, bty = self.lvalue(node.obj)
            if bty.kind != 'struct':
                raise Unsupported(f"字段访问用在 {bty.kind} 上", node.src_line)
            try:
                off, fty = self._field_offsets[bty.name][node.name]
            except KeyError:
                raise Unsupported(f"struct {bty.name} 没有字段 {node.name}",
                                  node.src_line) from None
            if off == 0:
                return base_ptr, fty
            p = L.BuildGEP2(self.bd, self.storage_ll(Ty('int', 8, False)), base_ptr,
                            carr([self.const_int(off)]), 1, "fa")
            return p, fty
        raise Unsupported(f"左值形态 {T}", node.src_line)

    def prefix_global(self, name, line):
        ty = PREFIX_GLOBALS[name]
        g = self.L.AddGlobal(self.module, self.ll_type(ty), name.encode())
        self.L.SetLinkage(g, self.L.InternalLinkage)
        self.L.SetInitializer(g, self.L.ConstNull(self.ll_type(ty)))
        self.globals[name] = (g, ty)
        return g, ty

    # ------------------------------------------------------------ 语句
    def lower_block(self, block):
        self.push_scope()
        for st in block.stmts:
            self.lower_stmt(st)
        self.pop_scope()

    def lower_stmt(self, st):
        try:
            getattr(self, 's_' + type(st).__name__)(st)
        except Unsupported as e:
            self.unsupported.append((e.line or st.src_line, e.msg))
            if not self.collect:
                raise
        except AttributeError as e:
            self.unsupported.append((st.src_line, f"语句形态 {type(st).__name__}"))
            if not self.collect:
                raise Unsupported(f"语句形态 {type(st).__name__}", st.src_line) from e
        # 块已被终止符收口（return / break）→ 后续语句放进一个不可达块继续发
        if self.L.GetBlockTerminator(self.cur_block):
            self.start_block("dead")

    def s_VarDeclStmt(self, st):
        if st.kw == 'const' and st.name.startswith('#'):
            v = self.const_value(st.init)
            if v is None:
                raise Unsupported(f"编译期常量 {st.name} 的初始化式不是常量", st.src_line)
            self.bind(st.name, ('const', v))
            return
        v = self.e(st.init)
        if v.ty.kind == 'void':
            raise Unsupported(f"{st.name} 的初始化式没有值", st.src_line)
        if st.kw in ('let', 'const') and v.const is not None:
            # 参考实现把「常量变量」当常量（`ul7_narrow_var.md` 的 fE +304）→ 保真
            self.bind(st.name, ('const', v))
            return
        a = self.alloca(v.ty, st.name)
        self.store_addr(a, v, v.ty, st.src_line)
        self.bind(st.name, ('local', a, v.ty))

    def s_AssignStmt(self, st):
        addr, ty = self.lvalue(st.target)
        if st.op == '=':
            val = self.convert(self.e(st.value), ty, st.src_line)
        else:
            cur = self.load_addr(addr, ty)
            merged = self.binop(ASSIGN_BINOP[st.op], cur, self.e(st.value), st.src_line)
            val = self.convert(merged, ty, st.src_line)
        self.store_addr(addr, val, ty, st.src_line)

    def s_ExprStmt(self, st):
        self.e(st.expr)

    def s_IfStmt(self, st):
        """if / elif / else：所有分支跑完汇到同一个 cont 块（惰性创建）。

        `in_scope("…")` 是**编译期谓词**，哪怕写在裸 `if` 里也一样：006 的
        `if in_scope("get_input")` / `if in_scope("check_output")` 就是这种形态
        （`#if` 那三处是 `on_ui_update` / `on_manual_input`）。判定为假的块**整块丢掉**，
        连里面的名字都不解析 —— 与参考实现一致（facts.md §006 的语义）。
        """
        cases = []
        for case in st.cases:
            decided = self.in_scope_const(case.cond)
            if decided is not None:      # `is True`/`is False` 过不了 lint；这就是那个三态
                if decided:
                    self.lower_block(case.block)
                    return
                continue
            cases.append(case)
        if not cases:
            if st.else_block is not None:
                self.lower_block(st.else_block)
            return
        st = fe_expr.IfStmt(src_line=st.src_line, cases=cases, else_block=st.else_block)
        self.lower_if_runtime(st)

    def lower_if_runtime(self, st):
        L = self.L
        cont = None

        def join():
            nonlocal cont
            if cont is None:
                cont = L.AppendBlock(self.ctx, self.fnref, b"ifend")
            L.BuildBr(self.bd, cont)

        for case in st.cases:
            cond = self.as_cond(self.e(case.cond))
            then_bb = L.AppendBlock(self.ctx, self.fnref, b"then")
            next_bb = L.AppendBlock(self.ctx, self.fnref, b"next")
            L.BuildCondBr(self.bd, cond, then_bb, next_bb)
            L.PositionAtEnd(self.bd, then_bb)
            self.cur_block = then_bb
            self.lower_block(case.block)
            if not L.GetBlockTerminator(self.cur_block):
                join()
            L.PositionAtEnd(self.bd, next_bb)
            self.cur_block = next_bb
        if st.else_block is not None:
            self.lower_block(st.else_block)
            if not L.GetBlockTerminator(self.cur_block):
                join()
        if cont is not None:
            # 没有 else 时，`next_bb` 就是「条件全不成立」那条路——它也要汇进 cont
            if not L.GetBlockTerminator(self.cur_block):
                L.BuildBr(self.bd, cont)
            L.PositionAtEnd(self.bd, cont)
            self.cur_block = cont

    def s_PpIfStmt(self, st):
        """`#if in_scope("name")`。U-L14 未定 —— 这里只实现 006 需要的那一档：
        「名字是**顶层** def ⇒ 真，否则假」。006 里三处被查的名字都不是顶层 def。
        """
        for case in st.cases:
            if self.ppif_true(case.cond):
                self.lower_block(case.block)
                return
        if st.else_block is not None:
            self.lower_block(st.else_block)

    def ppif_true(self, cond):
        decided = self.in_scope_const(cond)
        if decided is None:
            raise Unsupported("#if 的条件不是 in_scope(\"名字\")", cond.src_line)
        return decided

    def in_scope_const(self, cond):
        """`in_scope("名字")` → True/False；不是这个形态 → None（按运行期处理）。

        U-L14 未定，这里只实现 006 需要的那一档：**名字是顶层 def ⇒ 真，否则假**。
        """
        if (type(cond).__name__ == 'CallExpr'
                and type(cond.callee).__name__ == 'IdentExpr'
                and cond.callee.name == 'in_scope' and len(cond.args) == 1):
            arg = cond.args[0]
            if type(arg).__name__ == 'LiteralExpr' and arg.kind in ('str', 'template'):
                return arg.text in self.defs
            raise Unsupported("in_scope() 的参数不是字符串字面量", cond.src_line)
        return None

    def s_WhileStmt(self, st):
        L = self.L
        head = self.br_new_block("loop")
        body_bb = L.AppendBlock(self.ctx, self.fnref, b"body")
        exit_bb = L.AppendBlock(self.ctx, self.fnref, b"exit")
        cond = self.as_cond(self.e(st.cond))
        L.BuildCondBr(self.bd, cond, body_bb, exit_bb)
        L.PositionAtEnd(self.bd, body_bb)
        self.cur_block = body_bb
        self.breaks.append((st.label, exit_bb))
        self.lower_block(st.block)
        self.breaks.pop()
        if not L.GetBlockTerminator(self.cur_block):
            L.BuildBr(self.bd, head)
        L.PositionAtEnd(self.bd, exit_bb)
        self.cur_block = exit_bb

    def s_BreakStmt(self, st):
        target = None
        for label, bb in reversed(self.breaks):
            if st.label is None or st.label == label:
                target = bb
                break
        if target is None:
            raise Unsupported(f"break {st.label or ''}".strip(), st.src_line)
        self.L.BuildBr(self.bd, target)

    def s_ReturnStmt(self, st):
        if st.value is None:
            if self.ret_ty.kind != 'void':
                raise Unsupported("非 None 函数里裸 return", st.src_line)
            self.L.BuildRetVoid(self.bd)
            return
        if self.ret_ty.kind == 'void':
            raise Unsupported("None 函数里 return 值", st.src_line)
        v = self.convert(self.e(st.value), self.ret_ty, st.src_line)
        if self.ret_ty.kind == 'struct':
            self.L.BuildRet(self.bd, self.llv(v))
        else:
            self.L.BuildRet(self.bd, self.llv(v))

    def s_SwitchStmt(self, st):
        L = self.L
        subj = self.e(st.subject)
        subj_ll = subj.ll if subj.const is None else self.const_int(self.pat_const(subj) or 0)
        plain = [c for c in st.cases if c.value is not None]
        default = [c for c in st.cases if c.value is None]
        end = None

        def join():
            nonlocal end
            if end is None:
                end = L.AppendBlock(self.ctx, self.fnref, b"swend")
            L.BuildBr(self.bd, end)

        for case in plain:
            cv = self.const_value(case.value)
            if cv is None:
                raise Unsupported("switch case 的值不是常量", case.src_line)
            cval = self.const_int(self.pat_const(cv) or 0)
            then_bb = L.AppendBlock(self.ctx, self.fnref, b"case")
            next_bb = L.AppendBlock(self.ctx, self.fnref, b"casenext")
            cnd = L.BuildICmp(self.bd, L.IEQ, subj_ll, cval, "sc")
            L.BuildCondBr(self.bd, cnd, then_bb, next_bb)
            L.PositionAtEnd(self.bd, then_bb)
            self.cur_block = then_bb
            self.lower_block(case.block)
            if not L.GetBlockTerminator(self.cur_block):
                join()
            L.PositionAtEnd(self.bd, next_bb)
            self.cur_block = next_bb
        if default:
            self.lower_block(default[0].block)
            if not L.GetBlockTerminator(self.cur_block):
                join()
        elif end is not None:
            L.BuildBr(self.bd, end)
        if end is not None:
            L.PositionAtEnd(self.bd, end)
            self.cur_block = end

    def s_DefStmt(self, st):
        """嵌套 def 只登记、不生成 —— 真被调用时才发。006 的 `halt` / `handle_test_result`
        在 `#if in_scope` 被丢掉之后就是死的，不该出现在 IR 里（也不该因为「捕获了另一个
        嵌套 def」而报错）。捕获表在**被调用时**才算，那时外层作用域还活着。"""
        self.bind(st.name, ('def', st))

    def captures_of(self, st):
        """嵌套 def 体里引用了哪些**外层局部**（按引用捕获 → 传 alloca 指针）。

        自己的参数、`var`/`let`/内层 `def` 都算「自己的」，同名遮蔽外层 —— 不往内层
        `DefStmt` 的体里钻（那是它自己的作用域）。
        """
        block = st.body
        caps = []
        own = {p.name for p in st.params}

        def collect_decls(n):
            if isinstance(n, list):
                for x in n:
                    collect_decls(x)
                return
            if not isinstance(n, fe_expr.Node):
                return
            T = type(n).__name__
            if T in ('VarDeclStmt', 'DefStmt'):
                own.add(n.name)
            for f in dc_fields(n):
                collect_decls(getattr(n, f.name))

        collect_decls(block)

        def rec(n):
            if isinstance(n, list):
                for x in n:
                    rec(x)
                return
            if not isinstance(n, fe_expr.Node):
                return
            T = type(n).__name__
            if T == 'DefStmt':
                return
            if T == 'IdentExpr':
                if n.name in own:
                    return
                b = self.lookup(n.name)
                if b is not None:
                    if b[0] in ('local', 'capture'):
                        if n.name not in [c[0] for c in caps]:
                            caps.append((n.name, b[1], b[2]))
                    elif b[0] == 'def':
                        raise Unsupported(
                            f"嵌套 def 调用另一个嵌套 def（{n.name}）", n.src_line)
            for f in dc_fields(n):
                rec(getattr(n, f.name))

        rec(block)
        return caps

    # ------------------------------------------------------------ 表达式
    def e(self, node):
        try:
            return getattr(self, 'e_' + type(node).__name__)(node)
        except AttributeError as e:
            raise Unsupported(f"表达式形态 {type(node).__name__}（{e}）",
                              node.src_line) from e

    def e_LiteralExpr(self, node):
        if node.kind == 'int':
            v = node.value & M64
            return V(self.const_int(v), I64, v)
        if node.kind in ('str', 'template'):
            return self.string_literal(node)
        raise Unsupported(f"字面量 {node.kind}", node.src_line)

    def e_IdentExpr(self, node):
        name = node.name
        if name in ('true', 'false'):
            c = 1 if name == 'true' else 0
            return V(self.const_int(c), BOOL, c)
        b = self.lookup(name)
        if b is not None:
            if b[0] in ('local', 'capture'):
                return self.load_addr(b[1], b[2])
            if b[0] == 'const':
                return b[1]
            raise Unsupported(f"{name} 只能作为被调函数", node.src_line)
        if name in self.globals:
            g, ty = self.globals[name]
            return self.load_addr(g, ty)
        if name in PREFIX_GLOBALS:
            g, ty = self.prefix_global(name, node.src_line)
            return self.load_addr(g, ty)
        if name in self.enum_member:
            ename, ordv = self.enum_member[name]
            return V(self.const_int(ordv), Ty('enum', 64, False, ename), ordv)
        if name in self.consts:
            return self.consts[name]
        if name in HASH_CONSTS:
            val, ty = HASH_CONSTS[name]
            return V(self.const_int(val), ty, val)
        raise Unsupported(f"未定义的名字 {name}", node.src_line)

    def e_CastExpr(self, node):
        target = self.ty_of_ref(node.target, node.src_line)
        return self.convert(self.e(node.expr), target, node.src_line)

    def e_UnaryExpr(self, node):
        if node.op == '+':
            return self.e(node.expr)
        if node.op == '!':
            v = self.e(node.expr)
            c = self.pat_const(v)
            if c is not None:
                return V(self.const_int(0 if c else 1), BOOL, 0 if c else 1)
            cnd = self.L.BuildICmp(self.bd, self.L.IEQ, self.llv(v),
                                   self.const_int(0), "n")
            return V(self.L.BuildZExt(self.bd, cnd, self.i64, "nb"), BOOL, None)
        v = self.e(node.expr)
        if not is_int(v.ty):
            raise Unsupported(f"一元 {node.op} 用在 {v.ty.kind}", node.src_line)
        c = self.pat_const(v)
        if c is not None:
            return V(None, v.ty, fold_unary(node.op, c, v.ty))
        if node.op == '-':
            ll = self.L.BuildSub(self.bd, self.const_int(0), self.llv(v), "neg")
        elif node.op == '~':
            ll = self.L.BuildXor(self.bd, self.llv(v),
                                 self.const_int(mask_of(v.ty.bits)), "bnot")
        else:
            raise Unsupported(f"一元运算符 {node.op}", node.src_line)
        return V(self.mask_ll(ll, v.ty.bits), v.ty, None)

    def e_BinaryExpr(self, node):
        if node.op in ('&&', '||'):
            return self.logical(node)
        a = self.e(node.left)
        b = self.e(node.right)
        if node.op in CMP_OPS:
            return self.compare(node.op, a, b, node.src_line)
        return self.binop(node.op, a, b, node.src_line)

    def e_CondExpr(self, node):
        """`cond ? a : b`。

        形状是 **condbr + phi**，不是 `select`：两臂里都可能有 `load(…)`（rv32im 的
        `com_mux` 就是两支 load），`select` 会两支都求值 —— 与短路语义不一致。

        每个臂的 `convert` 必须在**它自己那个块**里做完：`convert` 会把掩码 / `inttoptr`
        发进当前块，跑到别的块里就是错的。所以先各自求值拿到类型，`unify` 之后再回到各
        臂的块尾部补转换，然后两支一起 `br` 到 `t_end`，在那里用 phi 取一支。
        类型统一与掩码全走 `unify`/`convert`，与 if 语句的写法一致。
        """
        L = self.L
        cond = self.as_cond(self.e(node.cond))
        then_bb = L.AppendBlock(self.ctx, self.fnref, b"t_then")
        else_bb = L.AppendBlock(self.ctx, self.fnref, b"t_else")
        end_bb = L.AppendBlock(self.ctx, self.fnref, b"t_end")
        L.BuildCondBr(self.bd, cond, then_bb, else_bb)

        L.PositionAtEnd(self.bd, then_bb)
        self.cur_block = then_bb
        tv = self.e(node.then)
        t_bb = self.cur_block
        L.PositionAtEnd(self.bd, else_bb)
        self.cur_block = else_bb
        ev = self.e(node.else_)
        e_bb = self.cur_block

        if tv.ty.kind == 'void' or ev.ty.kind == 'void':
            raise Unsupported("三目的一支没有值", node.src_line)
        ty = self.unify(tv.ty, ev.ty, node.src_line)
        L.PositionAtEnd(self.bd, t_bb)
        tv = self.convert(tv, ty, node.src_line)
        L.BuildBr(self.bd, end_bb)
        L.PositionAtEnd(self.bd, e_bb)
        ev = self.convert(ev, ty, node.src_line)
        L.BuildBr(self.bd, end_bb)

        L.PositionAtEnd(self.bd, end_bb)
        self.cur_block = end_bb
        # phi 的结果类型是**值的类型**（SSA 里窄整数一律 i64 位模式，见文件头「值表示」），
        # 不是 `ll_type` 那个内存宽度；只有 struct / ptr 两类值不是 i64。
        vty = self.val_ll(ty)
        phi = L.BuildPhi(self.bd, vty, "cnd")
        L.AddIncoming(phi, carr([self.llv(tv), self.llv(ev)]), carr([t_bb, e_bb]), 2)
        return V(phi, ty, None)

    def e_CallExpr(self, node):
        callee = node.callee
        if type(callee).__name__ == 'IdentExpr':
            return self.call_named(callee.name, node)
        if type(callee).__name__ == 'MemberExpr':
            return self.call_method(callee, node)
        raise Unsupported(f"被调者形态 {type(callee).__name__}", node.src_line)

    def e_MemberExpr(self, node):
        addr, ty = self.lvalue(node)
        return self.load_addr(addr, ty)

    def e_IndexExpr(self, node):
        addr, ty = self.lvalue(node)
        return self.load_addr(addr, ty)

    def e_CompositeLitExpr(self, node):
        return self.composite_literal(node)

    def e_ArrayLitExpr(self, node):
        return self.array_literal(node)

    # ---- 运算
    def unify(self, a, b, line):
        if a.kind == 'ptr' or b.kind == 'ptr':
            if a.kind in ('ptr', 'int') and b.kind in ('ptr', 'int'):
                return PTR
            raise Unsupported(f"指针与 {b.kind if a.kind == 'ptr' else a.kind} 混用", line)
        if a.kind == 'struct' or b.kind == 'struct':
            raise Unsupported("struct 参与运算", line)
        if a.kind == 'array' or b.kind == 'array':
            raise Unsupported("Arr 参与运算", line)
        if a.kind == 'str' or b.kind == 'str':
            if a.kind == b.kind == 'str':
                return STR
            raise Unsupported("Str 与整数混用", line)
        if a.kind == 'enum' and b.kind == 'enum':
            if a.name != b.name:
                raise Unsupported(f"枚举 {a.name} 与 {b.name} 混用", line)
            return a
        if a.kind == 'enum':
            return b
        if b.kind == 'enum':
            return a
        if is_int(a) and is_int(b):
            if a.bits == b.bits:
                return a                      # 同宽不同符号性：以左操作数为准（见笔记）
            return a if a.bits > b.bits else b
        raise Unsupported(f"类型 {a.kind} 与 {b.kind} 混用", line)

    def as_cond(self, v):
        if v.ty.kind == 'void':
            raise Unsupported("None 值当条件", 0)
        return self.L.BuildICmp(self.bd, self.L.INE, self.llv(v), self.const_int(0), "c")

    def compare(self, op, a, b, line):
        L = self.L
        ty = self.unify(a.ty, b.ty, line)
        a = self.convert(a, ty, line)
        b = self.convert(b, ty, line)
        if self.pat_const(a) is not None and self.pat_const(b) is not None:
            return self.fold_cmp(op, a, b, ty)
        x, y = self.llv(a), self.llv(b)
        signed = is_int(ty) and ty.signed
        if signed and ty.bits < 64:
            x, y = self.sext64(x, ty.bits), self.sext64(y, ty.bits)
        base = CMP_OPS[op]
        if base == 'EQ':
            pred = L.IEQ
        elif base == 'NE':
            pred = L.INE
        else:
            pred = getattr(L, ('IS' if signed else 'IU') + base)
        cnd = L.BuildICmp(self.bd, pred, x, y, "cmp")
        return V(L.BuildZExt(self.bd, cnd, self.i64, "cmpz"), BOOL, None)

    def sext64(self, ll, bits):
        """bits 位位模式 → 有符号 64 位（运行期窄值做有符号比较时用）。"""
        if bits >= 64:
            return ll
        sh = 64 - bits
        return self.L.BuildAShr(self.bd, self.L.BuildShl(self.bd, ll,
                                                        self.const_int(sh), "sh"),
                                self.const_int(sh), "sa")

    def logical(self, node):
        """`&&` / `||` 短路（`get_command` 读的是游戏内存，不能随意重排）。"""
        L = self.L
        lhs = self.as_cond(self.e(node.left))
        lhs_bb = self.cur_block
        rhs_bb = L.AppendBlock(self.ctx, self.fnref, b"rhs")
        end_bb = L.AppendBlock(self.ctx, self.fnref, b"lend")
        if node.op == '&&':
            L.BuildCondBr(self.bd, lhs, rhs_bb, end_bb)
        else:
            L.BuildCondBr(self.bd, lhs, end_bb, rhs_bb)
        L.PositionAtEnd(self.bd, rhs_bb)
        self.cur_block = rhs_bb
        rhs = self.as_cond(self.e(node.right))
        rhs_end = self.cur_block
        L.BuildBr(self.bd, end_bb)
        L.PositionAtEnd(self.bd, end_bb)
        self.cur_block = end_bb
        i1 = L.IntType(self.ctx, 1)
        phi = L.BuildPhi(self.bd, i1, "lg")
        first = L.ConstInt(i1, 0 if node.op == '&&' else 1, 0)
        L.AddIncoming(phi, carr([first, rhs]), carr([lhs_bb, rhs_end]), 2)
        return V(L.BuildZExt(self.bd, phi, self.i64, "lgz"), BOOL, None)

    def binop(self, op, a, b, line):
        L = self.L
        if op not in ARITH_OPS:
            raise Unsupported(f"运算符 {op}", line)
        ty = self.unify(a.ty, b.ty, line)
        a = self.convert(a, ty, line)
        b = self.convert(b, ty, line)
        ca, cb = self.pat_const(a), self.pat_const(b)
        if ty.kind == 'ptr':
            if op not in ('+', '-'):
                raise Unsupported(f"指针 {op}", line)
            if ca is not None and cb is not None:
                r = (ca + cb) if op == '+' else (ca - cb)
                return V(None, PTR, r & M64)
            fn = L.BuildAdd if op == '+' else L.BuildSub
            return V(fn(self.bd, self.llv(a), self.llv(b), "pa"), PTR, None)
        if ca is not None and cb is not None:
            return V(None, ty, fold_arith(op, ca, cb, ty))
        bits = ty.bits
        x, y = self.llv(a), self.llv(b)
        if op in ('/', '%'):
            # 运行期结果**不**截断到 W（div_sign 指示 4）
            return V(self.div(op, x, y, ty), ty, None)
        if op == '<<':
            amt = L.BuildAnd(self.bd, y, self.const_int(bits - 1), "amt")
            return V(self.mask_ll(L.BuildShl(self.bd, x, amt, "shl"), bits), ty, None)
        if op == '>>':
            amt = L.BuildAnd(self.bd, y, self.const_int(bits - 1), "amt")
            ll = L.BuildLShr(self.bd, self.pattern_of(a), amt, "shr")
            return V(self.mask_ll(ll, bits), ty, None)
        fn = {'+': L.BuildAdd, '-': L.BuildSub, '*': L.BuildMul,
              '&': L.BuildAnd, '|': L.BuildOr, '^': L.BuildXor}[op]
        return V(self.mask_ll(fn(self.bd, x, y, "op"), bits), ty, None)

    def div(self, op, x, y, ty):
        """`/`、`%`：opcode 跟**被除数**的符号性（facts.md §`/`、`%`），零除数不 trap
        （商 0、余数 = 被除数，U-L8 Q3）；有符号再避开 `INT_MIN / -1`。"""
        L = self.L
        bits = ty.bits
        zero = self.const_int(0)
        bz = L.BuildICmp(self.bd, L.IEQ, y, zero, "bz")
        safe = L.BuildSelect(self.bd, bz, self.const_int(1), y, "sb")
        bm1 = None
        if ty.signed:
            bm1 = L.BuildICmp(self.bd, L.IEQ, y, self.const_int(mask_of(bits)), "bm1")
            safe = L.BuildSelect(self.bd, bm1, self.const_int(1), safe, "sb2")
        if op == '/':
            raw = (L.BuildSDiv if ty.signed else L.BuildUDiv)(self.bd, x, safe, "q")
        else:
            raw = (L.BuildSRem if ty.signed else L.BuildURem)(self.bd, x, safe, "r")
        if op == '/':
            res = L.BuildSelect(self.bd, bz, zero, raw, "qz")
            if bm1 is not None:
                neg = self.mask_ll(L.BuildSub(self.bd, zero, x, "nq"), bits)
                res = L.BuildSelect(self.bd, bm1, neg, res, "qz2")
        else:
            res = L.BuildSelect(self.bd, bz, x, raw, "rz")
            if bm1 is not None:
                res = L.BuildSelect(self.bd, bm1, zero, res, "rz2")
        return res

    # ------------------------------------------------------------ 调用
    def static_type(self, node):
        """只用于**重载粗判**；拿不到就返回 None（不猜）。"""
        T = type(node).__name__
        try:
            if T == 'LiteralExpr':
                if node.kind == 'int':
                    return I64
                return STR if node.kind in ('str', 'template') else None
            if T == 'IdentExpr':
                b = self.lookup(node.name)
                if b is not None:
                    if b[0] in ('local', 'capture'):
                        return b[2]
                    if b[0] == 'const':
                        return b[1].ty
                    return None
                if node.name in self.globals:
                    return self.globals[node.name][1]
                if node.name in PREFIX_GLOBALS:
                    return PREFIX_GLOBALS[node.name]
                if node.name in self.enum_member:
                    return Ty('enum', 64, False, self.enum_member[node.name][0])
                if node.name in self.consts:
                    return self.consts[node.name].ty
                if node.name in HASH_CONSTS:
                    return HASH_CONSTS[node.name][1]
                return None
            if T == 'CastExpr':
                return self.ty_of_ref(node.target, node.src_line)
            if T == 'UnaryExpr':
                return BOOL if node.op == '!' else self.static_type(node.expr)
            if T == 'BinaryExpr':
                if node.op in CMP_OPS or node.op in ('&&', '||'):
                    return BOOL
                a, b = self.static_type(node.left), self.static_type(node.right)
                return self.unify(a, b, node.src_line) if a and b else None
            if T == 'CallExpr':
                callee = node.callee
                if type(callee).__name__ == 'MemberExpr':
                    if callee.name == 'len':
                        return I64
                    if callee.name == 'ms':
                        return TIME
                    return None
                if type(callee).__name__ != 'IdentExpr':
                    return None
                name = callee.name
                if name in ('load', 'raw_cast') and node.type_args:
                    return self.ty_of_ref(node.type_args[0], node.src_line)
                if name in ('min', 'max', 'asr') and node.args:
                    return self.static_type(node.args[0])
                if name in ('sleep', 'memory_clear', 'memory_copy', 'thread_exit'):
                    return VOID
                if name == 'get_time':
                    return TIME
                d = self.pick_def(name, node)
                if d is not None:
                    return self.ty_of_name(d.ret or 'None', d.lo)
                b = self.lookup(name)
                if b is not None and b[0] == 'def':
                    return self.defstmt_ret(b[1])
                return None
            if T == 'MemberExpr':
                obj = self.static_type(node.obj)
                if obj is not None and obj.kind == 'struct':
                    return self._field_offsets[obj.name][node.name][1]
                return None
            if T == 'IndexExpr':
                obj = self.static_type(node.obj)
                if obj is not None and obj.kind in ('array', 'str'):
                    return obj.elem
                return None
            if T == 'CompositeLitExpr':
                return self.ty_of_ref(node.typ, node.src_line)
        except Unsupported:
            return None
        return None

    def compatible(self, arg, param):
        if arg is None or arg.kind != param.kind:
            return False
        if arg.kind in ('int',):
            return True
        if arg.kind in ('enum', 'struct'):
            return arg.name == param.name
        return True

    def pick_def(self, name, node):
        """同名重载：参数类型对得上的本文件 def（dossier `m3_0_constructs.tsv` L133）。"""
        for d in self.defs.get(name, []):
            types = self.decl_param_types(d)
            if types is None or len(types) != len(node.args):
                continue
            if all(self.compatible(self.static_type(a), p)
                   for a, p in zip(node.args, types, strict=False)):
                return d
        return None

    def defstmt_ret(self, st):
        """嵌套 `DefStmt` 的返回类型（注意：它存的是 `TypeRef`，顶层 `Decl.ret` 是字符串）。"""
        return self.ty_of_ref(st.ret, st.src_line) if st.ret is not None else VOID

    def defstmt_params(self, st):
        out = []
        for p in st.params:
            if p.typ is None:
                raise Unsupported(f"参数 {p.name} 缺类型", p.src_line)
            out.append(self.ty_of_ref(p.typ, p.src_line))
        return out

    def decl_param_types(self, d):
        if not d.params or d.params == '-':
            return []
        out = []
        for t in d.params.split(','):
            if ':' not in t:
                return None
            out.append(self.ty_of_name(t.split(':', 1)[1].strip(), d.lo))
        return out

    def call_named(self, name, node):
        d = self.pick_def(name, node)
        if d is not None:
            return self.call_user(d, node)
        b = self.lookup(name)
        if b is not None and b[0] == 'def':
            return self.call_nested(b, node)
        if name == 'load':
            return self.builtin_load(node)
        if name == 'store':
            return self.builtin_store(node)
        if name == 'raw_cast':
            return self.builtin_raw_cast(node)
        if name == 'asr':
            return self.builtin_asr(node)
        if name in ('min', 'max'):
            return self.builtin_minmax(name, node)
        if name in ('in_scope', 'type_of', 'size'):
            raise Unsupported(f"{name}() 只支持编译期形态", node.src_line)
        if name in PREFIX_FUNCS:
            return self.call_prefix(name, node)
        if self.defs.get(name):
            raise Unsupported(f"{name}() 的重载解析拿不到参数类型", node.src_line)
        raise Unsupported(f"未定义的函数 {name}", node.src_line)

    def _emit_call(self, fnref, args):
        return self.L.BuildCall2(self.bd, self.L.GlobalGetValueType(fnref), fnref,
                                 carr(args), len(args), "")

    def call_user(self, d, node):
        ret_ty = self.ty_of_name(d.ret or 'None', d.lo)
        param_tys = self.decl_param_types(d)
        if param_tys is None or len(param_tys) != len(node.args):
            raise Unsupported(f"{d.name}() 的参数表对不上调用", node.src_line)
        fnref = self.ensure_def_fn(('def', d.name, d.lo), d, ret_ty, param_tys)
        vals = []
        for a, pt in zip(node.args, param_tys, strict=False):
            cv = self.convert(self.e(a), pt, node.src_line)
            vals.append(cv.ll if pt.kind == 'struct' else self.llv(cv))
        ll = self._emit_call(fnref, vals)
        return V(None, VOID) if ret_ty.kind == 'void' else V(ll, ret_ty)

    def call_nested(self, binding, node):
        st = binding[1]
        caps = self.captures_of(st)
        ret_ty = self.defstmt_ret(st)
        param_tys = self.defstmt_params(st)
        if len(param_tys) != len(node.args):
            raise Unsupported(f"{st.name}() 的参数个数对不上", node.src_line)
        fnref = self.ensure_nested_fn(('nested', self.cur_name, st.name, st.src_line),
                                      st, ret_ty, param_tys, caps)
        vals = [self.llv(self.convert(self.e(a), pt, node.src_line))
                for a, pt in zip(node.args, param_tys, strict=False)]
        vals += [c[1] for c in caps]
        ll = self._emit_call(fnref, vals)
        return V(None, VOID) if ret_ty.kind == 'void' else V(ll, ret_ty)

    def builtin_load(self, node):
        if not node.type_args or len(node.args) != 1:
            raise Unsupported("load(<T>, p) 的形态", node.src_line)
        ty = self.ty_of_ref(node.type_args[0], node.src_line)
        return self.load_addr(self.to_ptr(self.e(node.args[0])), ty)

    def builtin_store(self, node):
        if len(node.args) != 2:
            raise Unsupported("store(p, v) 的形态", node.src_line)
        addr = self.to_ptr(self.e(node.args[0]))
        val = self.e(node.args[1])
        if val.ty.kind == 'void':
            raise Unsupported("store 的值是 None", node.src_line)
        self.store_addr(addr, val, val.ty, node.src_line)
        return V(None, VOID)

    def builtin_raw_cast(self, node):
        if not node.type_args or len(node.args) != 1:
            raise Unsupported("raw_cast(<T>, v) 的形态", node.src_line)
        ty = self.ty_of_ref(node.type_args[0], node.src_line)
        v = self.e(node.args[0])
        c = self.pat_const(v)
        if c is not None:
            return V(None, ty, c & M64)
        return V(self.llv(v), ty, None)

    def builtin_asr(self, node):
        """`asr`：按 W 位符号位算术右移，与 `>>` 不同（U-L8 指示 3）。"""
        if len(node.args) != 2:
            raise Unsupported("asr(x, n) 的形态", node.src_line)
        x = self.e(node.args[0])
        n = self.e(node.args[1])
        if not is_int(x.ty):
            raise Unsupported(f"asr 用在 {x.ty.kind}", node.src_line)
        bits = x.ty.bits
        cx, cn = self.pat_const(x), self.pat_const(n)
        if cx is not None and cn is not None:
            amt = (cn & 63) if bits == 64 else min(cn, bits - 1)
            return V(None, x.ty, (as_signed(cx, bits) >> amt) & mask_of(bits))
        amt = self.llv(self.convert(n, x.ty, node.src_line))
        if bits == 64:
            amt = self.L.BuildAnd(self.bd, amt, self.const_int(63), "amt")
        else:
            amt = self.L.BuildSelect(self.bd, self.L.BuildICmp(
                self.bd, self.L.IULT, amt, self.const_int(bits), "lt"),
                amt, self.const_int(bits - 1), "sat")
        ll = self.L.BuildAShr(self.bd, self.sext64(self.llv(x), bits), amt, "asr")
        return V(self.mask_ll(ll, bits), x.ty, None)

    def builtin_minmax(self, name, node):
        if len(node.args) != 2:
            raise Unsupported(f"{name}(a, b) 的形态", node.src_line)
        ty = self.unify(self.static_type(node.args[0]) or I64,
                        self.static_type(node.args[1]) or I64, node.src_line)
        a = self.convert(self.e(node.args[0]), ty, node.src_line)
        b = self.convert(self.e(node.args[1]), ty, node.src_line)
        ca, cb = self.pat_const(a), self.pat_const(b)
        if ca is not None and cb is not None:
            x = as_signed(ca, ty.bits) if ty.signed else ca
            y = as_signed(cb, ty.bits) if ty.signed else cb
            pick = min(x, y) if name == 'min' else max(x, y)
            return V(None, ty, pick & mask_of(ty.bits))
        L = self.L
        pred = (L.ISLT if ty.signed else L.IULT) if name == 'min' \
            else (L.ISGT if ty.signed else L.IUGT)
        x, y = self.llv(a), self.llv(b)
        if ty.signed and ty.bits < 64:
            x, y = self.sext64(x, ty.bits), self.sext64(y, ty.bits)
        cnd = L.BuildICmp(self.bd, pred, x, y, "mm")
        ll = L.BuildSelect(self.bd, cnd, self.llv(a), self.llv(b), "mmv")
        return V(self.mask_ll(ll, ty.bits), ty, None)

    def call_method(self, callee, node):
        obj = self.e(callee.obj)
        if callee.name == 'len':
            if obj.ty.kind not in ('str', 'array'):
                raise Unsupported(f".len() 用在 {obj.ty.kind}", node.src_line)
            # 接口 §3：`len = u64(p-8) >> 16`
            raw = self.load_addr(self.L.BuildGEP2(
                self.bd, self.storage_ll(Ty('int', 8, False)), self.to_ptr(obj),
                carr([self.const_int(-8)]), 1, "lp"), Ty('int', 64, False))
            return V(self.L.BuildLShr(self.bd, self.llv(raw), self.const_int(16), "len"),
                     I64, None)
        if callee.name == 'ms':
            if not is_int(obj.ty):
                raise Unsupported(f".ms() 用在 {obj.ty.kind}", node.src_line)
            c = self.pat_const(obj)
            if c is not None:
                return V(None, TIME, (c * NS_PER_MS) & M64)
            return V(self.L.BuildMul(self.bd, self.llv(obj),
                                     self.const_int(NS_PER_MS), "ns"), TIME, None)
        raise Unsupported(f"方法 .{callee.name}()", node.src_line)

    # ------------------------------------------------------------ 前缀（stdlib）函数
    def call_prefix(self, name, node):
        if name == 'thread_exit':
            fn = self.get_extern(b"thread_exit", VOID, [])
            self._emit_call(fn, [])
            return V(None, VOID)
        if name == 'get_time':
            fn = self.ensure_prefix_fn('get_time', TIME, [])
            return V(self._emit_call(fn, []), TIME)
        if name == 'sleep':
            if len(node.args) != 1:
                raise Unsupported("sleep(t) 的形态", node.src_line)
            fn = self.ensure_prefix_fn('sleep', VOID, [TIME])
            self._emit_call(fn, [self.llv(self.convert(self.e(node.args[0]), TIME,
                                                       node.src_line))])
            return V(None, VOID)
        if name == 'memory_clear':
            if len(node.args) != 2:
                raise Unsupported("memory_clear(p, n) 的形态", node.src_line)
            fn = self.ensure_prefix_fn('memory_clear', VOID, [PTR, I64])
            self._emit_call(fn, [self.llv(self.e(node.args[0])),
                                 self.llv(self.convert(self.e(node.args[1]), I64,
                                                       node.src_line))])
            return V(None, VOID)
        if name == 'memory_copy':
            if len(node.args) != 3:
                raise Unsupported("memory_copy(src, dst, n) 的形态", node.src_line)
            fn = self.ensure_prefix_fn('memory_copy', VOID, [PTR, PTR, I64])
            self._emit_call(fn, [self.llv(self.e(node.args[0])),
                                 self.llv(self.e(node.args[1])),
                                 self.llv(self.convert(self.e(node.args[2]), I64,
                                                       node.src_line))])
            return V(None, VOID)
        raise Unsupported(f"前缀函数 {name}", node.src_line)

    def get_extern(self, name, ret_ty, param_tys):
        """只声明、不定义的外部符号（装配器给它造桩）。签名按**目标 ABI** 给：
        `Ptr` 参数就是真指针（我们的内部函数才用 i64 传地址）。"""
        key = ('extern', name.decode())
        if key in self._fn:
            return self._fn[key][0]
        fnty = self.L.FunctionType(
            self.ll_type(ret_ty) if ret_ty.kind != 'void' else self.void,
            carr([self.ptr if t.kind == 'ptr' else self.ll_type(t) for t in param_tys]),
            len(param_tys), 0)
        fnref = self.L.AddFunction(self.module, name, fnty)
        self._fn[key] = (fnref, ret_ty, param_tys, [])
        return fnref

    def ensure_prefix_fn(self, name, ret_ty, param_tys):
        key = ('prefix', name)
        if key in self._fn:
            return self._fn[key][0]
        fnty = self.L.FunctionType(
            self.val_ll(ret_ty) if ret_ty.kind != 'void' else self.void,
            carr([self.val_ll(t) for t in param_tys]), len(param_tys), 0)
        fnref = self.L.AddFunction(self.module, name.encode(), fnty)
        self.L.SetLinkage(fnref, self.L.InternalLinkage)
        self._fn[key] = (fnref, ret_ty, param_tys, [])
        self.fn_seen += 1
        saved = self._save_state()
        self.cur_name = name
        self.begin_function(fnref, ret_ty)
        getattr(self, '_body_' + name)(param_tys)
        self.finish_function()
        self._restore_state(saved)
        return fnref

    def _body_get_time(self, param_tys):
        L = self.L
        slot = self.alloca(Ty('int', 64, False), "ft")
        ext = self.get_extern(b"GetSystemTimePreciseAsFileTime", VOID, [PTR])
        self._emit_call(ext, [slot])
        ft = self.load_addr(slot, Ty('int', 64, False))
        d = L.BuildSub(self.bd, self.llv(ft), self.const_int(FILE_TIME_UNIX), "d")
        L.BuildRet(self.bd, L.BuildMul(self.bd, d, self.const_int(100), "ns"))

    def _body_sleep(self, param_tys):
        L = self.L
        ms = L.BuildUDiv(self.bd, L.GetParam(self.fnref, 0), self.const_int(NS_PER_MS), "ms")
        ext = self.get_extern(b"Sleep", VOID, [Ty('int', 32, False)])
        self._emit_call(ext, [L.BuildTrunc(self.bd, ms, self.storage_ll(
            Ty('int', 32, False)), "m")])
        L.BuildRetVoid(self.bd)

    def _byte_loop_prologue(self, n_param):
        """`i = 0` + 循环头；返回 (i 的地址槽, 循环头块, n 的 LLVM 值)。"""
        L = self.L
        slot = self.alloca(I64, "i")
        self.store_addr(slot, V(self.const_int(0), I64, 0), I64)
        head = self.br_new_block("lp")
        return slot, head, L.GetParam(self.fnref, n_param)

    def _body_memory_clear(self, param_tys):
        L = self.L
        addr = L.BuildIntToPtr(self.bd, L.GetParam(self.fnref, 0), self.ptr, "ap")
        slot, head, n = self._byte_loop_prologue(1)
        i = self.load_addr(slot, I64)
        cnd = L.BuildICmp(self.bd, L.IULT, self.llv(i), n, "lt")
        body = L.AppendBlock(self.ctx, self.fnref, b"lpb")
        done = L.AppendBlock(self.ctx, self.fnref, b"lpd")
        L.BuildCondBr(self.bd, cnd, body, done)
        L.PositionAtEnd(self.bd, body)
        self.cur_block = body
        p = L.BuildGEP2(self.bd, self.storage_ll(Ty('int', 8, False)), addr,
                        carr([self.llv(i)]), 1, "p")
        st = L.BuildStore(self.bd, L.ConstInt(self.storage_ll(Ty('int', 8, False)), 0, 0), p)
        L.SetAlignment(st, 1)
        L.SetVolatile(st, 1)
        self.store_addr(slot, V(
            L.BuildAdd(self.bd, self.llv(i), self.const_int(1), "i1"), I64), I64)
        L.BuildBr(self.bd, head)
        L.PositionAtEnd(self.bd, done)
        self.cur_block = done
        L.BuildRetVoid(self.bd)

    def _body_memory_copy(self, param_tys):
        L = self.L
        src = L.BuildIntToPtr(self.bd, L.GetParam(self.fnref, 0), self.ptr, "sp0")
        dst = L.BuildIntToPtr(self.bd, L.GetParam(self.fnref, 1), self.ptr, "dp0")
        slot, head, n = self._byte_loop_prologue(2)
        i = self.load_addr(slot, I64)
        cnd = L.BuildICmp(self.bd, L.IULT, self.llv(i), n, "lt")
        body = L.AppendBlock(self.ctx, self.fnref, b"lpb")
        done = L.AppendBlock(self.ctx, self.fnref, b"lpd")
        L.BuildCondBr(self.bd, cnd, body, done)
        L.PositionAtEnd(self.bd, body)
        self.cur_block = body
        i8 = self.storage_ll(Ty('int', 8, False))
        s = L.BuildGEP2(self.bd, i8, src, carr([self.llv(i)]), 1, "sp")
        d = L.BuildGEP2(self.bd, i8, dst, carr([self.llv(i)]), 1, "dp")
        ld = L.BuildLoad2(self.bd, self.storage_ll(Ty('int', 8, False)), s, "b")
        L.SetAlignment(ld, 1)
        L.SetVolatile(ld, 1)
        st = L.BuildStore(self.bd, ld, d)
        L.SetAlignment(st, 1)
        L.SetVolatile(st, 1)
        self.store_addr(slot, V(
            L.BuildAdd(self.bd, self.llv(i), self.const_int(1), "i1"), I64), I64)
        L.BuildBr(self.bd, head)
        L.PositionAtEnd(self.bd, done)
        self.cur_block = done
        L.BuildRetVoid(self.bd)

    # ------------------------------------------------------------ 函数按需生成
    def ensure_def_fn(self, key, d, ret_ty, param_tys):
        if key in self._fn:
            return self._fn[key][0]
        L = self.L
        fnty = L.FunctionType(
            self.val_ll(ret_ty) if ret_ty.kind != 'void' else self.void,
            carr([self.val_ll(t) for t in param_tys]), len(param_tys), 0)
        gname = d.name if len(self.defs.get(d.name, [])) == 1 else f"{d.name}#L{d.lo}"
        fnref = L.AddFunction(self.module, gname.encode(), fnty)
        L.SetLinkage(fnref, L.InternalLinkage)
        self._fn[key] = (fnref, ret_ty, param_tys, [])
        self.fn_seen += 1
        try:
            fd, _p = self.fe.parse_def(d)
        except (fe_expr.ParseError, fe_expr.LexError) as e:
            self.record(f"{d.name} 的函数体解析失败：{getattr(e, 'msg', e)}", d.lo)
            fd = None
        saved = self._save_state()
        self.cur_name, self.cur_def = d.name, d
        self.begin_function(fnref, ret_ty)
        if fd is not None:
            for i, p in enumerate(fd.params):
                ty = param_tys[i]
                a = self.alloca(ty, p.name)
                self.store_addr(a, V(L.GetParam(fnref, i), ty), ty,
                                p.src_line)
                self.bind_root_scope(p.name, ('local', a, ty))
            self.lower_block(fd.body)
        self.finish_function()
        self._restore_state(saved)
        return fnref

    def ensure_nested_fn(self, key, st, ret_ty, param_tys, caps):
        if key in self._fn:
            return self._fn[key][0]
        L = self.L
        fnty = L.FunctionType(
            self.val_ll(ret_ty) if ret_ty.kind != 'void' else self.void,
            carr([self.val_ll(t) for t in param_tys] + [self.ptr] * len(caps)),
            len(param_tys) + len(caps), 0)
        owner = getattr(self, 'cur_def', None)
        prefix = owner.name if owner is not None else "closure"
        fnref = L.AddFunction(self.module,
                              f"{prefix}${st.name}#L{st.src_line}".encode(), fnty)
        L.SetLinkage(fnref, L.InternalLinkage)
        self._fn[key] = (fnref, ret_ty, param_tys, caps)
        self.fn_seen += 1
        saved = self._save_state()
        self.cur_name = f"{prefix}${st.name}"
        self.begin_function(fnref, ret_ty)
        for i, p in enumerate(st.params):
            ty = param_tys[i]
            a = self.alloca(ty, p.name)
            self.store_addr(a, V(L.GetParam(fnref, i), ty), ty, p.src_line)
            self.bind_root_scope(p.name, ('local', a, ty))
        for j, (cname, _ptr, cty) in enumerate(caps):
            if any(p.name == cname for p in st.params):
                continue
            # 捕获变量按**引用**绑到外层那块 alloca 上（指针由调用方传进来）
            self.bind_root_scope(cname, ('capture', L.GetParam(
                fnref, len(param_tys) + j), cty))
        self.lower_block(st.body)
        self.finish_function()
        self._restore_state(saved)
        return fnref


# ---------------------------------------------------------------- 主体

class Program(Lower):
    """一份 `.si` → 一个 LLVM 模块 → 一份 COFF。"""

    def __init__(self, src_path, collect=False):
        self.src = src_path
        self.collect = collect           # True：遇到 unsupported 记下来继续（只为出清单）
        self.unsupported: list[tuple[int, str]] = []
        self.warnings: list[str] = []
        self.L = LLVM()
        try:
            self.fe = fe_expr.Frontend(src_path)
        except (OSError, fe_expr.LexError) as e:
            raise CompileFail(f"读 / 词法失败：{e}") from e
        if self.fe.decl_errors:
            raise CompileFail("声明层错误：" + "；".join(self.fe.decl_errors))
        self.enums: dict[str, list[str]] = {}
        self.enum_member: dict[str, tuple[str, int]] = {}
        self.structs: dict[str, Ty] = {}
        self.globals: dict[str, tuple[object, Ty]] = {}
        self.consts: dict[str, V] = {}
        self.defs: dict[str, list] = {}
        self.label = None
        self.top_vars: list = []
        self.fn_seen = 0
        self._scan_decls()
        self._make_context()
        self._make_types()
        self._make_globals()

    def record(self, msg, line):
        self.unsupported.append((line, msg))
        if not self.collect:
            raise Unsupported(msg, line)

    # ------------------------------------------------------------ 声明表
    def _scan_decls(self):
        for d in self.fe.decls:
            try:
                if d.kind == 'type':
                    self._scan_type(d)
                elif d.kind in ('var', 'const') and d.depth == 0:
                    self.top_vars.append((d, self.parse_decl_stmt(d)))
                elif d.kind == 'def' and d.depth == 0:
                    self.defs.setdefault(d.name, []).append(d)
                elif d.kind == 'label':
                    if self.label is not None:
                        raise CompileFail("一个文件里有两个顶层语句块（程序根只能有一个）")
                    self.label = d
            except Unsupported as e:
                self.record(e.msg, e.line or d.lo)

    def _scan_type(self, d):
        text = " ".join(self.fe.code_lines[d.lo - 1:d.hi])
        if d.note == 'Enum':
            m = ENUM_RE.search(text)
            if not m:
                raise CompileFail(f"第 {d.lo} 行：枚举声明看不懂：{text.strip()}")
            members = [x.strip() for x in m.group(1).split(',') if x.strip()]
            self.enums[d.name] = members
            for i, mem in enumerate(members):
                if mem in self.enum_member:
                    raise CompileFail(f"枚举成员 {mem} 重名（第 {d.lo} 行）")
                self.enum_member[mem] = (d.name, i)
        elif d.note == 'Struct':
            body = text[text.find('{') + 1:text.rfind('}')]
            fields = []
            for part in body.split(','):
                fm = FIELD_RE.search(part)
                if fm:
                    fields.append((fm.group(1), self.ty_of_name(fm.group(2), d.lo)))
            if not fields:
                # 空 struct（`type X Struct { }`，rv32im 的 `Input`/`Output`）：U-L4 布局未定，
                # 取 **1 字节占位**而不是零宽度聚合 —— 零宽度的 alloca / 全局大小是 0，
                # 那条路没验证过；1 字节同时也保住「对象有地址」。
                self.structs[d.name] = Ty('struct', 8, False, d.name, ())
                return
            self.structs[d.name] = Ty('struct', sum(storage_bytes(t) for _, t in fields) * 8,
                                      False, d.name, tuple(fields))
        else:
            raise Unsupported(f"type 声明形态（{d.note}）", d.lo)

    def parse_decl_stmt(self, d):
        """顶层 `var`/`const` 的初始化式交给前端自己的 parser（不另写一份）。"""
        toks = [t for t in self.fe.toks if d.lo <= t.line <= d.hi]
        toks.append(fe_expr.Tok('eof', '', d.hi, 1, True))
        p = fe_expr.Parser(toks, self.fe.type_names, self.fe.func_names)
        try:
            stmt = p.parse_statement()
        except fe_expr.ParseError as e:
            raise CompileFail(f"第 {e.line} 行：{e.msg}") from e
        if not isinstance(stmt, fe_expr.VarDeclStmt):
            raise CompileFail(f"第 {d.lo} 行：顶层 {d.kind} 解析成了 {type(stmt).__name__}")
        return stmt

    # ------------------------------------------------------------ 类型桥
    def ty_of_name(self, name, line):
        if name.startswith('@'):
            raise Unsupported(f"编译期类型参数 {name}", line)
        base = name
        if base == 'Int':
            return I64
        if base in ('Uint', 'Sint'):
            return I64                  # 重载族约束（接口 §1）；006 不走这条路
        if base == 'Bool':
            return BOOL
        if base == 'Ptr':
            return PTR
        if base == 'Str':
            return STR
        if base == 'Time':
            return TIME
        if base == 'Seed':
            return SEED
        if base in ('None', 'Void'):
            return VOID
        if base in self.enums:
            return Ty('enum', 64, False, base)
        if base in self.structs:
            return self.structs[base]
        m = SIZED_RE.fullmatch(base)
        if m:
            digits = m.group(2)
            try:
                bits = int(digits)
            except ValueError:      # `\d+` 保证是十进制数字；真变了要报出来而不是裸 ValueError
                raise Unsupported(f"位宽 {name}（{digits!r} 不是十进制数）", line) from None
            if not 1 <= bits <= 64:
                raise Unsupported(f"位宽 {name}", line)
            return sized(bits, m.group(1) == 'S')
        raise Unsupported(f"类型 {name}", line)

    def ty_of_ref(self, ref, line):
        if ref.elem is not None:
            if ref.name != 'Arr':
                raise Unsupported(f"泛型类型 {ref.name}[…]", line)
            return Ty('array', 64, False, elem=self.ty_of_ref(ref.elem, line))
        return self.ty_of_name(ref.name, line)

    # ------------------------------------------------------------ 模块骨架
    def _make_context(self):
        L = self.L
        L.InitializeX86TargetInfo()
        L.InitializeX86Target()
        L.InitializeX86TargetMC()
        L.InitializeX86AsmPrinter()
        self.ctx = L.ContextCreate()
        self.module = L.ModuleCreate(b"simplex", self.ctx)
        L.SetTarget(self.module, TRIPLE.encode())
        L.SetDataLayout(self.module, DATALAYOUT.encode())
        self.i64 = L.Int64(self.ctx)
        self.ptr = L.PointerType(self.i64, 0)
        self.void = L.VoidType(self.ctx)
        self._int_types = {64: self.i64}
        self._fn = {}                   # key → (LLVMValueRef, 返回 Ty, [参数 Ty], 捕获)
        self._str_pool = {}
        self._struct_ll = {}
        self._field_offsets = {}
        self.bd = None

    def _make_types(self):
        for name, ty in self.structs.items():
            elems = [self.storage_ll(t) for _, t in ty.fields]
            if not elems:                       # 空 struct：取 `Ty.bits` 那个占位宽度
                elems = [self.storage_ll(sized(ty.bits, False))]
            self._struct_ll[name] = self.L.StructType(self.ctx, carr(elems), len(elems), 1)
        for name, ty in self.structs.items():
            offs, off = {}, 0
            for fname, fty in ty.fields:
                offs[fname] = (off, fty)
                off += storage_bytes(fty)
            self._field_offsets[name] = offs

    def _make_globals(self):
        L = self.L
        for d, stmt in self.top_vars:
            try:
                if stmt.kw == 'const' and stmt.name.startswith('#'):
                    v = self.const_value(stmt.init)
                    if v is None:
                        raise Unsupported(f"编译期常量 {stmt.name} 的初始化式不是常量", d.lo)
                    self.consts[stmt.name] = v
                    continue
                v = self.const_value(stmt.init)
                if v is None:
                    raise Unsupported(f"顶层 var {stmt.name} 的初始化式不是编译期常量"
                                      "（运行期初始化未做）", d.lo)
                g = L.AddGlobal(self.module, self.ll_type(v.ty), stmt.name.encode())
                L.SetLinkage(g, L.InternalLinkage)
                L.SetInitializer(g, self.const_for_storage(v))
                self.globals[stmt.name] = (g, v.ty)
            except Unsupported as e:
                if not self.collect:
                    raise
                self.unsupported.append((e.line or d.lo, e.msg))
                g = L.AddGlobal(self.module, self.i64, stmt.name.encode())
                L.SetLinkage(g, L.InternalLinkage)
                L.SetInitializer(g, L.ConstNull(self.i64))
                self.globals[stmt.name] = (g, I64)

    def val_ll(self, ty):
        """一个**值**的 LLVM 表示类型（不是内存宽度，也不是 ABI 宽度）。

        SSA 里整数一律 i64 位模式 + 掩码承载宽度（U-L7），`Str`/`Arr` 是真 `ptr`，
        聚合保持自身。内部函数（`ensure_def_fn`/`ensure_nested_fn`/`ensure_prefix_fn`）
        的参数与返回也走这一层 —— 它们只在**函数体内**按声明类型掩码，不留窄 `iN` 在
        签名上（否则 `mask_ll` 的 i64 掩码常量与 i8/i16/i32 参数类型对不上）。
        唯一按**目标 ABI** 给宽度的是 `get_extern`（`Sleep(i32)` 那种），它在调用点做转换。
        """
        return self.ll_type(ty) if ty.kind in ('struct', 'str', 'array') else self.i64

    def ll_type(self, ty):
        """LLVM 侧的类型。

        `Str` / `Arr` 的值是**真指针**（`ptr`）而不是 i64：把数据地址放进整数运算会让
        后端生成 `movabs $imm64` + **ADDR64 绝对重定位**，而冻结的装配规则 §4 第 2 条
        明令拒绝指向内部符号的绝对重定位（blob 会被拷到任意地址）。保持 ptr 就只会出
        `lea rip+…`（REL32）。`Ptr` 仍是 i64 —— 它装的都是游戏地址常量，不涉及内部符号。
        """
        if ty.kind == 'struct':
            return self._struct_ll[ty.name]
        if ty.kind in ('str', 'array'):
            return self.ptr
        return self.storage_ll(ty)

    def storage_ll(self, ty):
        n = max(8, storage_bytes(ty) * 8)
        if n not in self._int_types:
            self._int_types[n] = self.L.IntType(self.ctx, n)
        return self._int_types[n]

    def const_int(self, v):
        return self.L.ConstInt(self.i64, v & M64, 0)

    def const_ptr(self, addr):
        return self.L.ConstIntToPtr(self.const_int(addr), self.ptr)

    def const_for_storage(self, v):
        """把一个常量 V 落成「内存里那个类型」的 LLVM 常量。"""
        if v.ty.kind == 'struct':
            return self.L.ConstNull(self._struct_ll[v.ty.name])
        if v.ty.kind in ('str', 'array'):
            return self.llv(v) if v.ll is not None else self.const_ptr(v.const or 0)
        if v.const is None:
            raise Unsupported("（内部）const_for_storage 拿到非常量", 0)
        return self.L.ConstInt(self.storage_ll(v.ty), v.const & mask_of(v.ty.bits), 0)

    # ------------------------------------------------------------ 常量求值
    #
    # 只认「整棵子树都是常量」的形态；任何依赖内存 / 函数调用的都返回 None。
    # 除法与移位折的是**位模式**（见 fold_arith），与参考实现的常量路径同形。

    def const_value(self, node):
        try:
            return self._cv(node)
        except Unsupported:
            return None

    def _cv(self, node):
        T = type(node).__name__
        if T == 'LiteralExpr':
            if node.kind == 'int':
                v = node.value & M64
                return V(None, I64, v)
            if node.kind in ('str', 'template'):
                return self.string_literal(node)
            raise Unsupported(f"字面量 {node.kind}", node.src_line)
        if T == 'IdentExpr':
            name = node.name
            if name in ('true', 'false'):
                return V(None, BOOL, 1 if name == 'true' else 0)
            if name in self.consts:
                return self.consts[name]
            if name in self.enum_member:
                ename, ordv = self.enum_member[name]
                return V(None, Ty('enum', 64, False, ename), ordv)
            if name in HASH_CONSTS:
                val, ty = HASH_CONSTS[name]
                return V(None, ty, val)
            raise Unsupported(f"名字 {name}", node.src_line)
        if T == 'CastExpr':
            return self.convert(self._cv(node.expr),
                                self.ty_of_ref(node.target, node.src_line), node.src_line)
        if T == 'UnaryExpr':
            inner = self._cv(node.expr)
            if node.op == '+':
                return inner
            if not is_int(inner.ty):
                raise Unsupported(f"一元 {node.op} 用在 {inner.ty.kind}", node.src_line)
            return V(None, inner.ty, fold_unary(node.op, inner.const, inner.ty))
        if T == 'BinaryExpr':
            if node.op in ('&&', '||'):
                raise Unsupported("常量逻辑运算", node.src_line)
            ty = self.unify(self._cv(node.left).ty, self._cv(node.right).ty, node.src_line)
            a = self.convert(self._cv(node.left), ty, node.src_line)
            b = self.convert(self._cv(node.right), ty, node.src_line)
            if node.op in CMP_OPS:
                return self.fold_cmp(node.op, a, b, ty)
            if node.op not in ARITH_OPS:
                raise Unsupported(f"运算符 {node.op}", node.src_line)
            if ty.kind == 'ptr':
                if node.op not in ('+', '-'):
                    raise Unsupported(f"指针 {node.op}", node.src_line)
                ca, cb = self.pat_const(a), self.pat_const(b)
                if ca is None or cb is None:
                    raise Unsupported("指针算术的操作数不是常量", node.src_line)
                r = (ca + cb) if node.op == '+' else (ca - cb)
                return V(None, PTR, r & M64)
            return V(None, ty, fold_arith(node.op, a.const, b.const, ty))
        if T == 'CallExpr':
            callee = node.callee
            if (type(callee).__name__ == 'IdentExpr' and callee.name == 'raw_cast'
                    and node.type_args and len(node.args) == 1):
                ty = self.ty_of_ref(node.type_args[0], node.src_line)
                v = self._cv(node.args[0])
                return V(None, ty, (self.pat_const(v) or 0) & M64)
            raise Unsupported("常量求值遇到函数调用", node.src_line)
        if T == 'ArrayLitExpr':
            return self.array_literal(node)
        if T == 'CompositeLitExpr':
            return self.composite_literal(node)
        raise Unsupported(f"常量求值遇到 {T}", node.src_line)

    def fold_cmp(self, op, a, b, ty):
        bits = ty.bits if ty.kind == 'int' else 64
        x, y = a.const & mask_of(bits), b.const & mask_of(bits)
        if ty.kind == 'int' and ty.signed:
            x, y = as_signed(x, bits), as_signed(y, bits)
        r = {'==': x == y, '!=': x != y, '<': x < y, '<=': x <= y,
             '>': x > y, '>=': x >= y}[op]
        return V(None, BOOL, 1 if r else 0)

    def string_literal(self, node):
        """字符串字面量 → Nim 布局的常量（`p[-8] = (len << 16) | cap_log2`，接口 §3）。

        U-L3/U-L10 未定：只支持**不含反斜杠**的字面量（逃逸语义没查）；`cap_log2` 取 0
        （只有 `len = (p-8) >> 16` 那半被用到）；末尾照加一个 NUL。
        """
        text = node.text
        if '\\' in text:
            raise Unsupported("含转义的字符串字面量（U-L10 未定）", node.src_line)
        hit = self._str_pool.get(text)
        if hit is not None:
            return hit
        data = text.encode('utf-8')
        header = len(data) << 16
        raw = header.to_bytes(8, 'little') + data + b"\x00"
        i8 = self.storage_ll(Ty('int', 8, False))
        elems = [self.L.ConstInt(i8, ch, 0) for ch in raw]
        arrty = self.L.ArrayType(i8, len(elems))
        g = self.L.AddGlobal(self.module, arrty, b".str")
        self.L.SetLinkage(g, self.L.PrivateLinkage)
        self.L.SetGlobalConstant(g, 1)
        self.L.SetInitializer(g, self.L.ConstArray(i8, carr(elems), len(elems)))
        payload = self.L.ConstGEP2(arrty, g, carr([self.const_int(0),
                                                  self.const_int(8)]), 2)
        v = V(payload, STR, None)
        self._str_pool[text] = v
        return v

    def array_literal(self, node):
        elems_v = [self._cv(x) for x in node.items]
        if not elems_v:
            raise Unsupported("空数组字面量", node.src_line)
        ty = elems_v[0].ty
        elems = [self.const_for_storage(e) for e in elems_v]
        arrty = self.L.ArrayType(self.storage_ll(ty), len(elems))
        g = self.L.AddGlobal(self.module, arrty, f".arr{node.src_line}".encode())
        self.L.SetLinkage(g, self.L.PrivateLinkage)
        self.L.SetGlobalConstant(g, 1)
        self.L.SetInitializer(g, self.L.ConstArray(self.storage_ll(ty), carr(elems),
                                                  len(elems)))
        return V(g, Ty('array', 64, False, elem=ty), None)

    def composite_literal(self, node):
        ty = self.ty_of_ref(node.typ, node.src_line)
        if ty.kind != 'struct':
            raise Unsupported(f"复合字面量类型 {ty.kind}", node.src_line)
        if node.fields:
            raise Unsupported("带字段初始化的复合字面量（U-L4 布局未定）", node.src_line)
        return V(self.L.ConstNull(self._struct_ll[ty.name]), ty)

    # ------------------------------------------------------------ 落码
    def build(self):
        """生成整个模块：程序根 → `tc_main`，其余 def 在 lowering 中按需生成。"""
        if self.label is None:
            raise CompileFail("没有顶层语句块（程序根）")
        try:
            self.emit_main(self.label)
        except Unsupported as e:
            self.record(e.msg, e.line)

    def emit_main(self, d):
        L = self.L
        fnty = L.FunctionType(self.void, carr([]), 0, 0)
        fnref = L.AddFunction(self.module, b"tc_main", fnty)
        fd, _p = self.fe.parse_def(d)
        saved = self._save_state()
        self.cur_name, self.cur_def = 'tc_main', None
        self.begin_function(fnref, VOID)
        self.lower_block(fd.body)
        # 程序根末尾隐式 thread_exit（接口 §1「程序根」）
        ext = self.get_extern(b"thread_exit", VOID, [])
        self._emit_call(ext, [])
        L.BuildRetVoid(self.bd)
        self.finish_function()
        self._restore_state(saved)
        return fnref

    def ir_text(self):
        s = self.L.PrintModule(self.module)
        try:
            return ct.string_at(s).decode('utf-8', 'replace')
        finally:
            self.L.DisposeMessage(s)

    def emit_object(self):
        L = self.L
        tgt = ct.c_void_p()
        err = ct.c_char_p()
        if L.GetTargetFromTriple(TRIPLE.encode(), ct.byref(tgt), ct.byref(err)):
            raise CompileFail(f"GetTargetFromTriple: {err.value}")
        # RelocMode = PIC（接口 §7，2026-10-18 由 Static 改成 PIC；§4-2 保留）：
        # Static 下 LLVM 用 `[disp32]` 绝对寻址访问内部全局 → 产物里出现指向内部符号的
        # ADDR32 重定位，而 blob 会被整体拷到编译期未知的 JIT 基址（§4 第 2 条明令拒绝）。
        # PIC 让内部引用只剩 REL32，§4-2 自然成立；clang 对这个三元组本来就默认 PIC。
        tm = L.CreateTargetMachine(tgt, TRIPLE.encode(), b"generic", b"",
                                   L.CodeGenLevelDefault, L.RelocPIC, L.CodeModelSmall)
        if not tm:
            raise CompileFail("CreateTargetMachine 返回 NULL")
        msg = ct.c_char_p()
        if L.VerifyModule(self.module, 1, ct.byref(msg)):
            text = msg.value.decode('utf-8', 'replace') if msg.value else "?"
            L.DisposeMessage(msg)
            L.DisposeTargetMachine(tm)
            raise CompileFail("IR 校验失败：" + text)
        opts = L.CreatePassBuilderOptions()
        if L.RunPasses(self.module, b"default<O2>", tm, opts):
            L.DisposePassBuilderOptions(opts)
            L.DisposeTargetMachine(tm)
            raise CompileFail("RunPasses(default<O2>) 失败")
        buf = ct.c_void_p()
        err = ct.c_char_p()
        if L.EmitToMemoryBuffer(tm, self.module, L.ObjectFile, ct.byref(err), ct.byref(buf)):
            msg = err.value.decode('utf-8', 'replace') if err.value else "?"
            L.DisposePassBuilderOptions(opts)
            L.DisposeTargetMachine(tm)
            raise CompileFail(f"EmitToMemoryBuffer: {msg}")
        data = ct.string_at(L.GetBufferStart(buf), L.GetBufferSize(buf))
        L.DisposeMemoryBuffer(buf)
        L.DisposePassBuilderOptions(opts)
        L.DisposeTargetMachine(tm)
        return data


# ---------------------------------------------------------------- CLI

def rel(p):
    try:
        r = os.path.relpath(p, REPO)
    except ValueError:
        return p
    return p if r.startswith('..') else r


def run(cmd, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                       errors='replace', timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def assemble(obj, blob):
    if not os.path.isfile(TCBLOB):
        raise CompileFail(f"找不到 {TCBLOB}")
    rc, out, err = run([TCBLOB, "--obj", obj, "--obj", ENTRY_SHIM, "--obj", RT_MIN,
                        "--entry", "__tc_entry", "--out", blob])
    if rc != 0:
        raise CompileFail(f"tcblob 失败 rc={rc}\n{out}\n{err}")
    try:
        with open(blob + ".entry", encoding='ascii') as f:
            entry = int(f.read().strip(), 16)
    except (OSError, ValueError) as e:
        raise CompileFail(f"读不了 {blob}.entry（装配器写出的 entry 文件）：{e}") from e
    return entry, out.strip()


def host_run(src, binpath, entry, ticks, out):
    rc, _so, se = run([sys.executable, HOST_PY, "--src", src, "--bin", binpath,
                       "--entry", hex(entry), "--ticks", str(ticks), "--out", out,
                       "--quiet"], timeout=max(300, ticks * 120))
    snaps = []
    if os.path.isfile(out):
        try:
            with open(out, encoding='utf-8') as f:
                for line in f:
                    if line.strip():
                        try:
                            snaps.append(json.loads(line))
                        except json.JSONDecodeError:
                            continue        # 半截行（host 被杀）不该当崩
        except OSError as e:
            print(f"读不了 {out}（host 的逐 tick 输出）：{e}", file=sys.stderr)
    return rc, snaps, se.strip()


def check_against_ref(src, our_bin, our_entry, ref_bin, ref_entry, ticks, workdir):
    """ours vs ref 的逐 tick 比对。判据逻辑复用 `tools/probe_006.py:compare_ticks`
    （「判据只有一处定义」），白名单恒为空。"""
    probe = load_module(PROBE_PY, 'probe006')
    try:
        os.makedirs(workdir, exist_ok=True)
    except OSError as e:
        print(f"检查失败：建不了目录 {workdir}：{e}", file=sys.stderr)
        return False
    ours_jsonl = os.path.join(workdir, "ours.jsonl")
    ref_jsonl = os.path.join(workdir, "ref.jsonl")
    for p in (ours_jsonl, ref_jsonl):
        try:
            os.remove(p)
        except OSError:
            continue                    # 上一轮的产物，不存在就没什么可删
    rc_o, ours, err_o = host_run(src, our_bin, our_entry, ticks, ours_jsonl)
    rc_r, ref, err_r = host_run(src, ref_bin, ref_entry, ticks, ref_jsonl)
    diffs, whitelisted = probe.compare_ticks(ref, ours, {})
    ok = not diffs and len(ours) == ticks and len(ref) == ticks
    print(f"[check] 参考 tick {len(ref)}/{ticks}（rc={rc_r}，entry={hex(ref_entry)}）· "
          f"我们 tick {len(ours)}/{ticks}（rc={rc_o}，entry={hex(our_entry)}）· "
          f"白名单 {len(whitelisted)} 条（要求 0）")
    print(f"[check] 逐 tick 全等={'是' if not diffs else '否'}"
          f"（比 pages / 状态字段 / extern 序列；差异 {len(diffs)} 处）")
    for d in diffs[:12]:
        print("  差异：" + json.dumps(d, ensure_ascii=False))
    if err_o and rc_o != 0:
        print("  --- host(ours) stderr ---\n" + err_o[-1500:])
    if err_r and rc_r != 0:
        print("  --- host(ref) stderr ---\n" + err_r[-800:])
    return ok


def resolve_ref(src):
    manifest = os.path.join(SIMPLEXC, "corpus", "manifest.tsv")
    try:
        with open(manifest, encoding='utf-8') as f:
            rows = [ln.rstrip("\n").split("\t") for ln in f if ln.strip()]
    except OSError as e:
        print(f"读不了 {manifest}（可用 --ref/--ref-entry 直推）：{e}", file=sys.stderr)
        return None, None
    head = rows[0]
    stem = os.path.splitext(os.path.basename(src))[0]
    for row in rows[1:]:
        if os.path.splitext(os.path.basename(row[1]))[0] == stem:
            # `corpus/ref/<src 的 stem>.bin`（manifest 没有「产物文件名」列）
            return (os.path.join(SIMPLEXC, "corpus", "ref", stem + ".bin"),
                    int(row[head.index('entry')], 0))
    return None, None


def main(argv=None):
    ap = argparse.ArgumentParser(description="Simplex AST → LLVM IR → COFF → blob（M3.0 第一刀）")
    ap.add_argument('--src', required=True, help=".si 源文件")
    ap.add_argument('--out', help="装配出的 blob 路径（不给就只出 IR/COFF）")
    ap.add_argument('--obj-out', help="COFF 路径（默认 <out>.o 或 <workdir>/<name>.o）")
    ap.add_argument('--ir-out', help="LLVM IR 文本 dump 到这个路径")
    ap.add_argument('--workdir', default=os.path.join(REPO, "work", "tmp", "lower0"),
                    help="中间文件目录（默认 work/tmp/lower0）")
    ap.add_argument('--list-unsupported', action='store_true',
                    help="收集并打印全部 UNSUPPORTED（不出产物；有清单时退出码 3）")
    ap.add_argument('--check', action='store_true',
                    help="宿主里跑参考产物与我们的产物，逐 tick 比对")
    ap.add_argument('--ref', help="参考产物（默认从 corpus/manifest.tsv 查）")
    ap.add_argument('--ref-entry', help="参考产物的 entry 偏移（默认从 manifest 查）")
    ap.add_argument('--entry', help="覆盖我们产物的 entry 偏移（默认读 <out>.entry）")
    ap.add_argument('--ticks', type=int, default=10)
    args = ap.parse_args(argv)

    src = os.path.abspath(args.src)
    if not os.path.isfile(src):
        print(f"读不了 {args.src}", file=sys.stderr)
        return 2
    try:
        prog = Program(src, collect=args.list_unsupported)
        prog.build()
    except Unsupported as e:
        print(f"UNSUPPORTED: {e.msg} @ 第 {e.line} 行", file=sys.stderr)
        return 1
    except CompileFail as e:
        print(f"编译失败：{e}", file=sys.stderr)
        return 1

    if args.list_unsupported:
        seen = sorted(set(prog.unsupported))
        print(f"# {rel(src)}：UNSUPPORTED {len(seen)} 条")
        for line, msg in seen:
            print(f"UNSUPPORTED: {msg} @ 第 {line} 行")
        for w in prog.warnings:
            print("WARNING: " + w)
        return 3 if seen else 0

    if prog.unsupported:
        for line, msg in sorted(set(prog.unsupported)):
            print(f"UNSUPPORTED: {msg} @ 第 {line} 行", file=sys.stderr)
        return 1

    try:
        os.makedirs(args.workdir, exist_ok=True)
    except OSError as e:
        print(f"建不了中间目录 {args.workdir}：{e}", file=sys.stderr)
        return 2
    ir = prog.ir_text()
    ir_path = args.ir_out or os.path.join(args.workdir, os.path.basename(src) + ".ll")
    try:
        os.makedirs(os.path.dirname(os.path.abspath(ir_path)) or ".", exist_ok=True)
        with open(ir_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(ir)
    except OSError as e:
        print(f"写不了 IR 文件 {ir_path}：{e}", file=sys.stderr)
        return 2
    print(f"[lower] IR {len(ir)} B（函数 {prog.fn_seen + 1} 个）→ {rel(ir_path)}")

    try:
        obj = prog.emit_object()
    except CompileFail as e:
        print(f"编译失败：{e}", file=sys.stderr)
        return 1
    obj_path = args.obj_out or ((args.out + ".o") if args.out else
                                os.path.join(args.workdir,
                                             os.path.basename(src) + ".o"))
    try:
        with open(obj_path, "wb") as f:
            f.write(obj)
    except OSError as e:
        print(f"写不了 COFF 文件 {obj_path}：{e}", file=sys.stderr)
        return 2
    print(f"[lower] COFF {len(obj)} B → {rel(obj_path)}")

    if not args.out:
        return 0
    out = os.path.abspath(args.out)
    try:
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    except OSError as e:
        print(f"建不了产物目录 {os.path.dirname(out) or '.'}：{e}", file=sys.stderr)
        return 2
    try:
        entry, _ = assemble(obj_path, out)
    except CompileFail as e:
        print(f"装配失败：{e}", file=sys.stderr)
        return 1
    if args.entry:
        entry = int(args.entry, 0)
    print(f"[lower] blob {os.path.getsize(out)} B → {rel(out)}（entry={hex(entry)}）")

    if not args.check:
        return 0
    ref, ref_entry = args.ref, None
    auto_ref, auto_entry = resolve_ref(src)
    if not ref:
        ref = auto_ref
    ref_entry = int(args.ref_entry, 0) if args.ref_entry else auto_entry
    if not ref or ref_entry is None or not os.path.isfile(ref):
        print(f"找不到 {os.path.basename(src)} 的参考产物（corpus/ref + manifest.tsv）",
              file=sys.stderr)
        return 2
    ok = check_against_ref(src, out, entry, os.path.abspath(ref), ref_entry,
                           args.ticks, args.workdir)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
