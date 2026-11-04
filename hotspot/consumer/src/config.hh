// hotspot/config.ini 的读取。键与默认值见 spec §10；本文件不解码未知键以外的东西，
// 也不接受「键在但类型不对」——静默回落到默认值会让采错的参数看起来是对的。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace hotspot {

enum class Backend { etw, suspend };

struct Config {
    std::string game_exe_name = "Turing Complete.exe";
    Backend backend = Backend::etw;
    int interval_ms = 1;
    int window_sec = 10;
    int suspend_probe_rounds = 40;
    std::string etw_session_name = "hotspot";
    bool etw_keep_etl = false;
    std::string lock_dir = "work/tmp/locks";
    std::string workspace = "hotspot/workspace";
    std::string artifacts = "work/hook";
    std::string freq_command = "python tools/read_freq.py --samples 1";
    double freq_timeout_sec = 5.0;
};

// INI 子集：`[section]` 段头、`key = value`、`;` 与 `#` 注释（引号外）。
// 值与 Python 侧同一套 JSON 编码：字符串带引号、true/false、数字裸写。
// 不做的就是不支持的（数组、多行字符串、null），遇到直接报错而不是忽略。
class IniTable {
  public:
    using Value = std::variant<std::string, std::int64_t, double, bool>;

    // 重复键报错：配置里出现两遍同一个键，说明有人在猜哪一份生效。
    std::optional<std::string> parse(std::string_view text);
    void set(const std::string &key, Value value);

    std::optional<Value> get(std::string_view key) const;
    const std::map<std::string, Value, std::less<>> &values() const
    {
        return values_;
    }

  private:
    std::map<std::string, Value, std::less<>> values_;
};

std::optional<std::string> parse_backend(std::string_view name, Backend &out);
const char *backend_name(Backend backend);

// 只覆盖配置里出现的键；不存在 / 不是普通文件时用默认值。
std::expected<Config, std::string> load_config(const std::filesystem::path &path);
std::expected<Config, std::string> config_from_ini(std::string_view text);

} // namespace hotspot
