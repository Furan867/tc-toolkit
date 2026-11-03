// 极简单测框架：一个可执行文件跑全部用例，用例名就是函数名（失败时直接报文件:行）。
#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing_support {

struct Case {
    std::string name;
    std::function<void()> body;
};

std::vector<Case> &registry();

struct Registrar {
    Registrar(const char *name, std::function<void()> body);
};

// 断言失败一律抛异常：一条用例里第一个失败点就够定位，不攒报告。
[[noreturn]] inline void fail(const char *file, int line, const std::string &message)
{
    std::ostringstream out;
    out << file << ":" << line << ": " << message;
    throw std::runtime_error(out.str());
}

} // namespace testing_support

#define TEST_CASE(name)                                                              \
    static void name();                                                              \
    static ::testing_support::Registrar name##_registrar(#name, name);               \
    static void name()

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            ::testing_support::fail(__FILE__, __LINE__, #condition);                 \
        }                                                                            \
    } while (false)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        const auto &actual_value = (actual);                                         \
        const auto &expected_value = (expected);                                     \
        if (!(actual_value == expected_value)) {                                     \
            std::ostringstream message;                                              \
            message << #actual << " == " #expected << "（实际 " << actual_value      \
                    << "，期望 " << expected_value << "）";                          \
            ::testing_support::fail(__FILE__, __LINE__, message.str());              \
        }                                                                            \
    } while (false)
