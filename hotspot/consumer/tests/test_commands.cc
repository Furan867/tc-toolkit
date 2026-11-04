// 命令解析 / config.ini / SampledProfile 负载解码 / freq 读数解析 —— 全是纯函数。
#include "commands.hh"
#include "config.hh"
#include "run_dir.hh"
#include "sha256.hh"
#include "test_support.hh"

#include <vector>

using hotspot::Backend;
using hotspot::Command;
using hotspot::config_from_ini;
using hotspot::parse_command;
using hotspot::parse_sim_speed_hz;

TEST_CASE(parse_open_with_pid_and_interval)
{
    const auto command = parse_command("open work/hook/hook_out_013.bin --pid 4321 --interval-ms 2");
    CHECK(command.has_value());
    CHECK(command->kind == Command::Kind::open);
    CHECK_EQ(command->open.artifact.generic_string(), std::string{"work/hook/hook_out_013.bin"});
    CHECK(command->open.pid.has_value());
    CHECK_EQ(*command->open.pid, std::uint32_t{4321});
    CHECK(command->open.interval_ms.has_value());
    CHECK_EQ(*command->open.interval_ms, 2);
}

TEST_CASE(parse_open_requires_artifact)
{
    const auto command = parse_command("open --pid 1");
    CHECK(!command.has_value());
    CHECK(command.error().find("open <artifact>") != std::string::npos);
}

TEST_CASE(parse_open_rejects_bad_pid)
{
    const auto command = parse_command("open a.bin --pid zero");
    CHECK(!command.has_value());
    CHECK(command.error().find("--pid") != std::string::npos);
}

TEST_CASE(parse_window_with_optional_save)
{
    const auto plain = parse_command("window 10");
    CHECK(plain.has_value());
    CHECK(plain->kind == Command::Kind::window);
    CHECK_EQ(plain->window_sec, 10);
    CHECK(!plain->window_save);

    const auto saving = parse_command("window 3 save");
    CHECK(saving.has_value());
    CHECK(saving->window_save);
    CHECK_EQ(saving->window_sec, 3);

    CHECK(!parse_command("window 0").has_value());
    CHECK(!parse_command("window").has_value());
    CHECK(!parse_command("window 10 keep").has_value());
}

TEST_CASE(parse_save_status_locate_quit)
{
    const auto save = parse_command("save");
    CHECK(save.has_value());
    CHECK(save->kind == Command::Kind::save);
    CHECK(!save->save_dir.has_value());
    CHECK_EQ(parse_command("save out/run1")->save_dir->generic_string(), std::string{"out/run1"});
    CHECK(parse_command("status")->kind == Command::Kind::status);
    CHECK(parse_command("locate")->kind == Command::Kind::locate);
    CHECK(parse_command("quit")->kind == Command::Kind::quit);
    CHECK(!parse_command("frobnicate").has_value());
    CHECK(!parse_command("   ").has_value());
}

TEST_CASE(parse_quoted_path_with_spaces)
{
    const auto command = parse_command("open \"C:/some dir/a.bin\"");
    CHECK(command.has_value());
    CHECK_EQ(command->open.artifact.generic_string(), std::string{"C:/some dir/a.bin"});
}

TEST_CASE(config_defaults_and_overrides)
{
    const auto config = config_from_ini(R"(
# 注释行
; 另一种注释
[game]
exe_name = "Turing Complete.exe"

[sampler]
backend = "suspend"
interval_ms = 2
window_sec = 5
suspend_probe_rounds = 12

[etw]
session_name = "hs"
keep_etl = true

[lock]
dir = "work/tmp/locks"

[paths]
workspace = "hotspot/workspace"
artifacts = "work/hook"

[freq]
command = "python tools/read_freq.py --samples 1"
timeout_sec = 2.5
)");
    CHECK(config.has_value());
    CHECK_EQ(config->game_exe_name, std::string{"Turing Complete.exe"});
    CHECK(config->backend == Backend::suspend);
    CHECK_EQ(config->interval_ms, 2);
    CHECK_EQ(config->window_sec, 5);
    CHECK_EQ(config->suspend_probe_rounds, 12);
    CHECK_EQ(config->etw_session_name, std::string{"hs"});
    CHECK(config->etw_keep_etl);
    CHECK_EQ(config->freq_timeout_sec, 2.5);
}

TEST_CASE(config_rejects_unknown_key_and_wrong_type)
{
    const auto unknown = config_from_ini("[sampler]\nintervl_ms = 3\n");
    CHECK(!unknown.has_value());
    CHECK(unknown.error().find("未知键") != std::string::npos);

    const auto wrong_type = config_from_ini("[sampler]\nbackend = 3\n");
    CHECK(!wrong_type.has_value());

    const auto bad_backend = config_from_ini("[sampler]\nbackend = \"pmi\"\n");
    CHECK(!bad_backend.has_value());
    CHECK(bad_backend.error().find("etw") != std::string::npos);

    const auto duplicate = config_from_ini("[sampler]\ninterval_ms = 1\ninterval_ms = 2\n");
    CHECK(!duplicate.has_value());
    CHECK(duplicate.error().find("重复") != std::string::npos);
}

TEST_CASE(config_comments_only_outside_quotes)
{
    const auto config = config_from_ini("[lock]\ndir = \"work/a#b;c\"  ; 尾注释\n");
    CHECK(config.has_value());
    CHECK_EQ(config->lock_dir, std::string{"work/a#b;c"});
}


TEST_CASE(parse_sim_speed_hz_from_read_freq_output)
{
    const auto hz = parse_sim_speed_hz("PID 1234   模块基址 0x7FF6\nsim_speed   100.000 MHz   (100000000 Hz)\n");
    CHECK(hz.has_value());
    CHECK_EQ(*hz, 100000000.0);
    CHECK(!parse_sim_speed_hz("Turing Complete.exe 没在运行\n").has_value());
    CHECK(!parse_sim_speed_hz("sim_speed        0        ← 仿真没在跑（哨兵值）").has_value());
}

TEST_CASE(sha256_matches_known_vectors)
{
    // 两个标准向量（FIPS 180-4 附录 B），够定位实现走样。
    CHECK_EQ(hotspot::sha256_hex(reinterpret_cast<const std::uint8_t *>("abc"), 3),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    CHECK_EQ(hotspot::sha256_hex(nullptr, 0),
             std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
}
