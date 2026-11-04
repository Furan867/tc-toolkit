#include "config.hh"

#include <charconv>
#include <fstream>
#include <sstream>

namespace hotspot {
namespace {

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// 注释只在引号外才算注释。
std::string_view strip_comment(std::string_view line)
{
    bool in_string = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
            in_string = !in_string;
        } else if ((c == '#' || c == ';') && !in_string) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::expected<IniTable::Value, std::string> parse_value(std::string_view text)
{
    text = trim(text);
    if (text.empty()) {
        return std::unexpected("值为空");
    }
    if (text.front() == '"') {
        if (text.size() < 2 || text.back() != '"') {
            return std::unexpected("字符串没有闭合的引号");
        }
        std::string out;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 2 < text.size()) {
                const char next = text[++i];
                switch (next) {
                case 't':
                    out.push_back('\t');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '"':
                    out.push_back('"');
                    break;
                default:
                    return std::unexpected(std::string("不支持的转义 \\") + next);
                }
                continue;
            }
            out.push_back(c);
        }
        return IniTable::Value{std::move(out)};
    }
    if (text == "true") {
        return IniTable::Value{true};
    }
    if (text == "false") {
        return IniTable::Value{false};
    }
    // 整数优先；带 . 或 e 的走浮点。
    if (text.find_first_of(".eE") != std::string_view::npos) {
        double value = 0.0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return std::unexpected("无法解析的浮点数");
        }
        return IniTable::Value{value};
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::unexpected("无法解析的整数");
    }
    return IniTable::Value{value};
}

} // namespace

std::optional<std::string> IniTable::parse(std::string_view text)
{
    std::string prefix;
    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

        line = trim(strip_comment(line));
        if (line.empty()) {
            continue;
        }
        const auto fail = [line_no](std::string_view what) {
            std::ostringstream out;
            out << "config.ini:" << line_no << ": " << what;
            return out.str();
        };

        if (line.front() == '[') {
            if (line.back() != ']') {
                return fail("段头没有闭合的 ]");
            }
            const std::string_view name = trim(line.substr(1, line.size() - 2));
            if (name.empty()) {
                return fail("空段头");
            }
            prefix = std::string(name) + ".";
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail("缺少 =");
        }
        const std::string_view key = trim(line.substr(0, eq));
        if (key.empty()) {
            return fail("空键名");
        }
        auto value = parse_value(line.substr(eq + 1));
        if (!value.has_value()) {
            return fail(value.error());
        }
        const std::string full_key = prefix + std::string(key);
        if (values_.contains(full_key)) {
            return fail("重复的键 " + full_key);
        }
        values_.emplace(full_key, std::move(*value));
    }
    return std::nullopt;
}

void IniTable::set(const std::string &key, Value value)
{
    values_.insert_or_assign(key, std::move(value));
}

std::optional<IniTable::Value> IniTable::get(std::string_view key) const
{
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> parse_backend(std::string_view name, Backend &out)
{
    if (name == "etw") {
        out = Backend::etw;
        return std::nullopt;
    }
    if (name == "suspend") {
        out = Backend::suspend;
        return std::nullopt;
    }
    return "sampler.backend 只能是 \"etw\" 或 \"suspend\"，实际是 \"" + std::string(name) + "\"";
}

const char *backend_name(Backend backend)
{
    return backend == Backend::etw ? "etw" : "suspend";
}

std::expected<Config, std::string> config_from_ini(std::string_view text)
{
    IniTable table;
    if (auto problem = table.parse(text); problem.has_value()) {
        return std::unexpected(*problem);
    }

    Config config;
    for (const auto &[key, value] : table.values()) {
        const auto type_error = [&key](std::string_view expected) {
            return key + " 的类型应为 " + std::string(expected);
        };

        if (key == "game.exe_name") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.game_exe_name = *text_value;
        } else if (key == "sampler.backend") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            if (auto problem = parse_backend(*text_value, config.backend); problem.has_value()) {
                return std::unexpected(*problem);
            }
        } else if (key == "sampler.interval_ms") {
            const auto *number = std::get_if<std::int64_t>(&value);
            if (number == nullptr) {
                return std::unexpected(type_error("整数"));
            }
            config.interval_ms = static_cast<int>(*number);
        } else if (key == "sampler.window_sec") {
            const auto *number = std::get_if<std::int64_t>(&value);
            if (number == nullptr) {
                return std::unexpected(type_error("整数"));
            }
            config.window_sec = static_cast<int>(*number);
        } else if (key == "sampler.suspend_probe_rounds") {
            const auto *number = std::get_if<std::int64_t>(&value);
            if (number == nullptr) {
                return std::unexpected(type_error("整数"));
            }
            config.suspend_probe_rounds = static_cast<int>(*number);
        } else if (key == "etw.session_name") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.etw_session_name = *text_value;
        } else if (key == "etw.keep_etl") {
            const auto *flag = std::get_if<bool>(&value);
            if (flag == nullptr) {
                return std::unexpected(type_error("布尔"));
            }
            config.etw_keep_etl = *flag;
        } else if (key == "lock.dir") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.lock_dir = *text_value;
        } else if (key == "paths.workspace") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.workspace = *text_value;
        } else if (key == "paths.artifacts") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.artifacts = *text_value;
        } else if (key == "freq.command") {
            const auto *text_value = std::get_if<std::string>(&value);
            if (text_value == nullptr) {
                return std::unexpected(type_error("字符串"));
            }
            config.freq_command = *text_value;
        } else if (key == "freq.timeout_sec") {
            const auto *real = std::get_if<double>(&value);
            const auto *integer = std::get_if<std::int64_t>(&value);
            if (real != nullptr) {
                config.freq_timeout_sec = *real;
            } else if (integer != nullptr) {
                config.freq_timeout_sec = static_cast<double>(*integer);
            } else {
                return std::unexpected(type_error("浮点"));
            }
        } else {
            // 拼错的键必须报出来：写错一个字母就静默用默认值 = 采到的参数不是想采的。
            return std::unexpected("config.ini 有未知键 " + key);
        }
    }
    return config;
}

std::expected<Config, std::string> load_config(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected("配置文件不存在：" + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("打不开配置文件：" + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return config_from_ini(buffer.str());
}

} // namespace hotspot
