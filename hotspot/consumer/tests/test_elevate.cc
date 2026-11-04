// 提权链的纯函数：命令行拼接（CommandLineToArgvW 规则）与「该不该提权」的判据。
// 真弹 UAC 的那一半（relaunch_elevated）不在这里 —— 单测不许弹框。
#include "config.hh"
#include "elevate.hh"
#include "test_support.hh"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

using hotspot::Backend;
using hotspot::build_command_line;
using hotspot::needs_elevation;
using hotspot::quote_argument;

namespace {

std::string to_utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}

// 用系统自己的解析器验拼接：这才叫「拼对了」，自己再写一遍逆变换只能验出自洽。
std::vector<std::string> parse_back(const std::wstring &command_line)
{
    int count = 0;
    wchar_t **argv = CommandLineToArgvW(command_line.c_str(), &count);
    if (argv == nullptr) {
        return {};
    }
    std::vector<std::string> arguments;
    for (int i = 0; i < count; ++i) {
        arguments.push_back(to_utf8(argv[i]));
    }
    LocalFree(argv);
    return arguments;
}

} // namespace

TEST_CASE(quote_argument_leaves_plain_argument_alone)
{
    CHECK_EQ(to_utf8(quote_argument(L"--config")), std::string{"--config"});
    CHECK_EQ(to_utf8(quote_argument(L"hotspot/config.toml")), std::string{"hotspot/config.toml"});
}

TEST_CASE(quote_argument_wraps_whitespace)
{
    CHECK_EQ(to_utf8(quote_argument(L"a b")), std::string{"\"a b\""});
    CHECK_EQ(to_utf8(quote_argument(L"a\tb")), std::string{"\"a\tb\""});
    // 空参数必须显式写一对引号，否则它会在命令行里消失。
    CHECK_EQ(to_utf8(quote_argument(L"")), std::string{"\"\""});
}

TEST_CASE(quote_argument_escapes_quote_and_backslash)
{
    CHECK_EQ(to_utf8(quote_argument(L"a\"b")), std::string{"\"a\\\"b\""});
    // 不含空格/引号的参数不包引号，末尾反斜杠照原样。
    CHECK_EQ(to_utf8(quote_argument(L"C:\\dir\\")), std::string{"C:\\dir\\"});
    // 一旦要包引号，末尾反斜杠就得翻倍：单写一个会把收尾引号转义掉。
    CHECK_EQ(to_utf8(quote_argument(L"C:\\my dir\\")), std::string{"\"C:\\my dir\\\\\""});
    // 反斜杠只在紧挨着要转义的引号时才翻倍。
    CHECK_EQ(to_utf8(quote_argument(L"C:\\a b\\c\"d")), std::string{"\"C:\\a b\\c\\\"d\""});
}

TEST_CASE(build_command_line_joins_arguments)
{
    const std::vector<std::wstring> arguments{L"--config", L"my dir/config.toml"};
    CHECK_EQ(to_utf8(build_command_line(arguments)),
             std::string{"--config \"my dir/config.toml\""});
    CHECK_EQ(to_utf8(build_command_line({})), std::string{});
}

TEST_CASE(build_command_line_round_trips_through_command_line_to_argv)
{
    // argv[0] 的引号规则与其余参数不同，所以拿一个不含特殊字符的假程序名打底。
    const std::vector<std::wstring> arguments{
        L"--config", L"C:\\Users\\a b\\hotspot\\config.toml", L"--out", L"",
        L"带引号\"的\"参数", L"trailing\\", L"a\\\\\"b",
    };
    const std::wstring command_line = L"hotspot-consumer.exe " + build_command_line(arguments);

    const std::vector<std::string> expected{to_utf8(L"hotspot-consumer.exe"),
                                            to_utf8(L"--config"),
                                            to_utf8(L"C:\\Users\\a b\\hotspot\\config.toml"),
                                            to_utf8(L"--out"),
                                            std::string{},
                                            to_utf8(L"带引号\"的\"参数"),
                                            to_utf8(L"trailing\\"),
                                            to_utf8(L"a\\\\\"b")};
    CHECK(parse_back(command_line) == expected);
}

TEST_CASE(needs_elevation_only_for_etw_without_admin)
{
    CHECK(needs_elevation(Backend::etw, false));
    CHECK(!needs_elevation(Backend::etw, true));
    // suspend 免管理员：提权只会白弹一次 UAC，还会把 stdin/stdout 换成新控制台。
    CHECK(!needs_elevation(Backend::suspend, false));
    CHECK(!needs_elevation(Backend::suspend, true));
}
