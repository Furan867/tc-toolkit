// 会话行为：无游戏/无产物时的错误路径不崩、状态不串（真采样要游戏+锁，不在单测里）。
#include "session.hh"
#include "test_support.hh"

#include <filesystem>
#include <fstream>
#include <vector>

using hotspot::Config;
using hotspot::Session;

namespace {

std::filesystem::path temp_root(const std::string &tag)
{
    const auto dir = std::filesystem::temp_directory_path() / ("hotspot_session_" + tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

Config temp_config(const std::filesystem::path &root)
{
    Config config;
    config.lock_dir = (root / "locks").string();
    config.workspace = (root / "workspace").string();
    config.freq_command = "cmd /c echo 没在运行";
    config.freq_timeout_sec = 5.0;
    return config;
}

// 造一份能过 load_artifact 的产物（8 字节长度头 + 机器码）。
std::filesystem::path make_artifact(const std::filesystem::path &dir, std::uint64_t total_len)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total_len), 0x90);
    const std::uint64_t head = total_len | (1ull << 62);
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(head >> (i * 8));
    }
    const auto path = dir / "hook_out_001.bin";
    std::filesystem::create_directories(dir);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace

TEST_CASE(session_idle_commands_explain_what_is_missing)
{
    const auto root = temp_root("idle");
    {
    Session session(temp_config(root));

    const auto window = session.execute("window 1");
    CHECK(!window.has_value());
    CHECK(window.error().find("open") != std::string::npos);

    const auto save = session.execute("save");
    CHECK(!save.has_value());
    CHECK(save.error().find("open") != std::string::npos);

    const auto locate = session.execute("locate");
    CHECK(!locate.has_value());

    const auto status = session.execute("status");
    CHECK(status.has_value());
    CHECK(status->find("空闲") != std::string::npos);

    const auto unknown = session.execute("nonsense");
    CHECK(!unknown.has_value());
    CHECK(unknown.error().find("未知命令") != std::string::npos);

    CHECK(!session.should_quit());
    const auto quit = session.execute("quit");
    CHECK(quit.has_value());
    CHECK(session.should_quit());
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(session_open_missing_artifact_reports_reason)
{
    const auto root = temp_root("missing");
    Session session(temp_config(root));
    const auto opened = session.execute("open no/such/artifact.bin --pid 4");
    CHECK(!opened.has_value());
    CHECK(opened.error().find("产物不存在") != std::string::npos);
    CHECK(!session.should_quit());
    std::filesystem::remove_all(root);
}

TEST_CASE(session_open_dead_pid_reports_openprocess_failure)
{
    // 产物存在、PID 不存在：定位必须明确报 OpenProcess 失败，而不是崩或静默空表。
    const auto root = temp_root("deadpid");
    const auto artifact = make_artifact(root / "artifacts", 4096);
    const auto lock_file = std::filesystem::path(temp_config(root).lock_dir) / "game.lock";
    {
        Session session(temp_config(root));
        const auto opened = session.execute("open \"" + artifact.generic_string() + "\" --pid 999999");
        CHECK(!opened.has_value());
        CHECK(opened.error().find("OpenProcess") != std::string::npos);
        CHECK(std::filesystem::exists(lock_file)); // open 期间持锁
    }
    // Session 析构即释放：不留残锁堵下一次（RAII 与异常路径同一套）。
    CHECK(!std::filesystem::exists(lock_file));
    std::filesystem::remove_all(root);
}

TEST_CASE(session_open_uses_game_name_when_pid_missing)
{
    // 没给 --pid 就按 config 的进程名找；本测试机没有这个进程名 → 明确报没找到。
    const auto root = temp_root("byName");
    const auto artifact = make_artifact(root / "artifacts", 4096);
    Config config = temp_config(root);
    config.game_exe_name = "definitely-not-running-1234.exe";
    {
        Session session(config);
        const auto opened = session.execute("open \"" + artifact.generic_string() + "\"");
        CHECK(!opened.has_value());
        CHECK(opened.error().find("没找到进程") != std::string::npos);
    }
    std::filesystem::remove_all(root);
}
