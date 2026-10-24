/* sema/errors.hh —— 编译期错误的分型与诊断条目。
 *
 * 两代模型并存，`Unsupported`/`CompileFail` 那一代是①b 从 `lower/lower.hh` 原样搬来的
 * （`lower/lower.hh` 用 using 引入 `simplexc::lower`，调用点一行没改）；`CompileError` 是
 * 方案 §2d 定的新模型，**新写的 sema 代码只用它**，旧路径 S3 再迁。 */
#pragma once

#include <stdexcept>
#include <string>

#include "span.hh"

namespace simplexc {
namespace sema {

/* ---------------------------------------------------------------- 错误
 * 两种错误分开：`Unsupported` = 没实现 / 没定的构造（带源码行号，接口 §8 要求错误能
 * 定位到 span），`CompileFail` = 内部失败或环境问题。壳按 `ErrKind` 分类回退，别在这里合并。 */
class Unsupported : public std::runtime_error {
public:
    Unsupported(const std::string& msg, int line)
        : std::runtime_error(msg), line_(line) {}
    int line() const { return line_; }

private:
    int line_;
};

class CompileFail : public std::runtime_error {
public:
    explicit CompileFail(const std::string& msg) : std::runtime_error(msg) {}
};


struct Diagnostic {
    int line = 0;
    std::string msg;
};

/* ---------------------------------------------------------------- 三轴错误（方案 §2d）
 *
 * 主设计 §3.4 要求三轴从第一层就带全；`Unsupported` 只带一个行号，责任与阶段无处放。
 * 三条约定：
 *   · 责任判定：认不出的源码构造 = `Unimplemented`；同一类输入两次行为不一致、内部断言失败 =
 *     `OurBug`；语法/类型错误 = `UserSource`。
 *   · **同一个函数里不混用** `Unsupported` 与 `CompileError` —— 混着写就分不清「这条路径已经
 *     迁过来了没有」。旧函数保持 `Unsupported`，新函数只抛 `CompileError`。
 *   · 过 C 边界时不许只剩类别名（`catch(...)` → `InternalError` 是禁止的），S3 做这一步。
 *
 * 为什么从 `std::runtime_error` 派生而不是裸聚合：`what()` 是判据与壳唯一能直接打印的东西
 * （`analyze_selftest` 只 catch `std::exception`），而 `detail` 是它的人读形式。四个字段照 §2d
 * 保留名字与顺序，所以调用点仍然写成 `CompileError{resp, stage, span, detail}`。 */
enum class Responsibility { UserSource, Unimplemented, OurBug };
enum class Stage { Lex, Parse, Sema, Codegen, Assemble, Shell };

inline const char* responsibility_name(Responsibility r) {
    switch (r) {
        case Responsibility::UserSource: return "用户源码错";
        case Responsibility::Unimplemented: return "未实现";
        case Responsibility::OurBug: return "我们的 bug";
    }
    return "?";
}

inline const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Lex: return "lex";
        case Stage::Parse: return "parse";
        case Stage::Sema: return "sema";
        case Stage::Codegen: return "codegen";
        case Stage::Assemble: return "assemble";
        case Stage::Shell: return "shell";
    }
    return "?";
}

struct CompileError : std::runtime_error {
    CompileError(Responsibility r, Stage s, Span sp, std::string d)
        : std::runtime_error(make_message(r, s, sp, d)),
          resp(r),
          stage(s),
          span(sp),
          detail(std::move(d)) {}

    Responsibility resp;
    Stage stage;
    Span span;
    std::string detail;

private:
    /* 人读形式：`sema/未实现 第 12 行：顶层 def f`。`what()` 是判据与壳唯一能直接打印的东西，
     * 三轴压在这里，所以它不能只剩一个类别名。 */
    static std::string make_message(Responsibility r, Stage s, Span sp, const std::string& d) {
        std::string out = std::string(stage_name(s)) + "/" + responsibility_name(r);
        if (sp.line > 0) out += " 第 " + std::to_string(sp.line) + " 行";
        return out + "：" + d;
    }
};

}  // namespace sema
}  // namespace simplexc