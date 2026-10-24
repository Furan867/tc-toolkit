/* sema/errors.hh —— 编译期错误的两种分型与诊断条目（步①b 从 `lower/lower.hh` 原样搬来）。
 *
 * 三轴错误模型（责任 / 阶段 / span）是后续步骤的事，这一步只搬家：内容与 `lower.hh` 里那一段
 * 逐行相同。`lower/lower.hh` 用 using 把这三个名字引入 `simplexc::lower`，调用点一行没改。 */
#pragma once

#include <stdexcept>
#include <string>

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

}  // namespace sema
}  // namespace simplexc